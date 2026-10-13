#include "TestUtil.h"

using namespace nss;

namespace
{
	NssDocument doc(std::string text, Eol eol = Eol::CRLF)
	{
		TextFormat f;
		f.eol = eol;
		return NssDocument::fromText(std::move(text), f);
	}

	const std::string themeNss =
		"\xEF\xBB\xBFtheme\r\n{\r\n\tname=\"modern\"\r\n\tdark=auto\r\n\tbackground\r\n\t{\r\n"
		"\t\tcolor=auto\r\n\t\topacity=auto\r\n\t\teffect=auto\r\n\t}\r\n\timage.align=2\r\n}";
}

TEST_CASE("every shipped .nss round-trips byte-identically")
{
	size_t count = 0;
	for(auto &file : test::corpus())
	{
		CAPTURE(file.string());
		auto bytes = test::readBytes(file);
		std::string err;
		auto d = NssDocument::load(file, {}, &err);
		REQUIRE_MESSAGE(d.has_value(), err);
		CHECK(d->lossless());
		CHECK_FALSE(d->modified());
		CHECK(d->serialize() == bytes);
		CHECK(d->diagnostics().empty());

		// No statement fell back to a single-token Opaque node: everything was
		// recognised as a block, assignment, import, declaration or named value.
		size_t stray = 0;
		d->visit([&](const Node &n)
		{
			if(n.kind == NodeKind::Opaque && n.path.empty() && n.valueSpan.empty())
				stray++;
			return true;
		});
		CHECK(stray == 0);

		// The same text stored as UTF-16 LE/BE (with BOM) round-trips as well.
		for(auto enc : { Encoding::Utf16LE, Encoding::Utf16BE })
		{
			TextFormat f = d->format();
			f.encoding = enc;
			f.bom = true;
			auto wide = encode(d->text(), f);
			auto w = NssDocument::fromBytes(wide);
			REQUIRE(w.has_value());
			CHECK(w->format().encoding == enc);
			CHECK(w->serialize() == wide);
			CHECK(w->text() == d->text());
		}
		count++;
	}
	CHECK(count >= 26);
	MESSAGE("round-tripped " << count << " shipped files");
}

TEST_CASE("the shipped shell.nss parses into the expected tree")
{
	auto d = NssDocument::load(test::binDir() / "shell.nss");
	REQUIRE(d.has_value());
	CHECK(d->diagnostics().empty());

	auto settings = d->findBlock("settings");
	REQUIRE(settings);
	CHECK(d->findAssign("settings.priority"));
	CHECK(d->slice(d->findAssign("settings.exclude.where")->valueSpan) == "!process.is_explorer");
	CHECK(d->slice(d->findAssign("settings.modify.remove.duplicate")->valueSpan) == "1");
	CHECK(d->slice(d->findAssign("settings.tip.enabled")->valueSpan) == "true");
	CHECK(d->slice(d->findAssign("settings.showdelay")->valueSpan) == "200");
	CHECK(d->assigns().size() == 5);

	auto imports = d->imports();
	REQUIRE(imports.size() == 11);
	CHECK(imports[0]->importKind == ImportKind::Lang);
	CHECK(d->slice(imports[0]->valueSpan) == "loc_path + \"en.nss\"");
	CHECK(imports[1]->importKind == ImportKind::Lang);
	CHECK(d->slice(imports[2]->valueSpan) == "'imports/theme.nss'");
	CHECK(d->slice(imports.back()->valueSpan) == "'imports/my-tools.nss'");
	for(auto n : imports)
		CHECK(n->enabled);

	size_t menus = 0, vars = 0;
	for(auto &n : d->nodes())
	{
		if(n.kind == NodeKind::MenuDecl) menus++;
		if(n.kind == NodeKind::Opaque && !n.path.empty() && n.path[0] == '$') vars++;
	}
	CHECK(menus == 2);
	CHECK(vars == 3);
}

TEST_CASE("menus, props and children are parsed")
{
	auto d = NssDocument::load(test::binDir() / "imports" / "file-manage.nss", { ParseMode::Config, {} });
	REQUIRE(d.has_value());
	CHECK(d->diagnostics().empty());
	REQUIRE(d->nodes().size() == 1);
	auto &menu = d->nodes()[0];
	CHECK(menu.menuKind == MenuKind::Menu);
	REQUIRE(menu.props.size() == 5);
	CHECK(menu.props[0].name == "where");
	CHECK(d->slice(menu.props[0].valueSpan) == "sel.count>0");
	CHECK(menu.props[4].name == "image");
	CHECK(d->slice(menu.props[4].valueSpan) == "\\uE253");
	CHECK(menu.children.size() >= 8);

	auto t = NssDocument::load(test::binDir() / "imports" / "terminal.nss");
	REQUIRE(t.has_value());
	auto &m = t->nodes()[0];
	REQUIRE(m.children.size() == 5);  // two $vars and three items
	auto &item = m.children[2];
	CHECK(item.menuKind == MenuKind::Item);
	REQUIRE(item.props.size() == 5);
	CHECK(item.props[3].name == "image");
	CHECK_FALSE(item.props[3].hasValue);
	CHECK(item.props[4].name == "cmd-prompt");

	auto mod = NssDocument::load(test::binDir() / "imports" / "modify.nss");
	REQUIRE(mod.has_value());
	size_t modifies = 0;
	for(auto &n : mod->nodes())
		if(n.menuKind == MenuKind::Modify) modifies++;
	CHECK(modifies == 8);
}

TEST_CASE("setValue changes only the value text (CRLF, BOM kept)")
{
	auto d = NssDocument::fromBytes(themeNss);
	REQUIRE(d.has_value());
	REQUIRE(d->findAssign("theme.background.opacity"));
	CHECK(d->setValue("theme.background.opacity", "80"));
	CHECK(d->setValue("Theme.Name", "\"classic\""));
	CHECK(d->modified());
	CHECK(d->serialize() ==
		  "\xEF\xBB\xBFtheme\r\n{\r\n\tname=\"classic\"\r\n\tdark=auto\r\n\tbackground\r\n\t{\r\n"
		  "\t\tcolor=auto\r\n\t\topacity=80\r\n\t\teffect=auto\r\n\t}\r\n\timage.align=2\r\n}");
	CHECK_FALSE(d->setValue("theme.font.size", "12"));  // not present

	// Expressions are locked unless forced.
	auto e = doc("theme{ dark = if(sys.is11, true, false) }");
	CHECK_FALSE(e.setValue("theme.dark", "true"));
	CHECK(e.setValue("theme.dark", "true", true));
	CHECK(e.text() == "theme{ dark = true }");
}

TEST_CASE("insertAssign matches indentation and line endings")
{
	auto d = NssDocument::fromBytes(themeNss);
	REQUIRE(d.has_value());
	CHECK(d->insertAssign("theme.background", "tintcolor", "#ff000080"));
	CHECK(d->insertAssign("theme", "font.size", "12"));
	CHECK(d->serialize() ==
		  "\xEF\xBB\xBFtheme\r\n{\r\n\tname=\"modern\"\r\n\tdark=auto\r\n\tbackground\r\n\t{\r\n"
		  "\t\tcolor=auto\r\n\t\topacity=auto\r\n\t\teffect=auto\r\n\t\ttintcolor=#ff000080\r\n\t}\r\n"
		  "\timage.align=2\r\n\tfont.size=12\r\n}");
	CHECK(d->findAssign("theme.font.size"));
	CHECK(d->findAssign("theme.background.tintcolor"));

	// LF file, spaces, " = " style, brace on the same line as the content.
	auto e = doc("settings {\n    priority = 1\n}\ntheme { dark = true }\n", Eol::LF);
	CHECK(e.insertAssign("settings", "showdelay", "200"));
	CHECK(e.insertAssign("theme", "view", "view.compact"));
	CHECK(e.text() == "settings {\n    priority = 1\n    showdelay = 200\n}\ntheme { dark = true \n    view = view.compact\n}\n");

	// An empty block gets one indent level.
	auto f = doc("theme\r\n{\r\n}\r\n");
	CHECK(f.insertAssign("theme", "dark", "true"));
	CHECK(f.text() == "theme\r\n{\r\n\tdark=true\r\n}\r\n");

	CHECK_FALSE(f.insertAssign("settings", "priority", "1"));  // no such block
}

TEST_CASE("removeAssign removes whole lines")
{
	auto d = NssDocument::fromBytes(themeNss);
	REQUIRE(d.has_value());
	CHECK(d->removeAssign("theme.dark"));
	CHECK(d->removeAssign("theme.image.align"));
	CHECK(d->serialize() ==
		  "\xEF\xBB\xBFtheme\r\n{\r\n\tname=\"modern\"\r\n\tbackground\r\n\t{\r\n"
		  "\t\tcolor=auto\r\n\t\topacity=auto\r\n\t\teffect=auto\r\n\t}\r\n}");
	CHECK_FALSE(d->removeAssign("theme.dark"));

	auto e = doc("theme { dark = true view = view.small }");
	CHECK(e.removeAssign("theme.view"));
	CHECK(e.text() == "theme { dark = true }");
}

TEST_CASE("imports: toggle, append, remove")
{
	std::string src =
		"settings { priority=1 }\r\n"
		"import 'imports/theme.nss'\r\n"
		"// import 'imports/goto.nss'\r\n"
		"//import \"imports/dev.nss\" // dev tools\r\n"
		"// or import lang 'imports/lang/en.nss'\r\n"
		"\timport 'imports/taskbar.nss'";
	auto d = doc(src);
	auto list = d.imports();
	REQUIRE(list.size() == 4);
	CHECK(list[0]->enabled);
	CHECK_FALSE(list[1]->enabled);
	CHECK(d.slice(list[1]->valueSpan) == "'imports/goto.nss'");
	CHECK(list[1]->line == 3);
	CHECK_FALSE(list[2]->enabled);
	CHECK(list[3]->enabled);

	CHECK(d.toggleImport(1, true));
	CHECK(d.toggleImport(0, false));
	CHECK(d.toggleImport(2, true));
	CHECK(d.toggleImport(3, false));
	CHECK(d.text() ==
		  "settings { priority=1 }\r\n"
		  "// import 'imports/theme.nss'\r\n"
		  "import 'imports/goto.nss'\r\n"
		  "import \"imports/dev.nss\" // dev tools\r\n"
		  "// or import lang 'imports/lang/en.nss'\r\n"
		  "\t// import 'imports/taskbar.nss'");
	CHECK(d.toggleImport(0, true));
	CHECK(d.toggleImport(0, true));  // already enabled: no change
	CHECK(d.text().rfind("settings { priority=1 }\r\nimport 'imports/theme.nss'\r\n", 0) == 0);

	CHECK(d.appendImport("imports/settings-app.nss"));
	CHECK(d.text().ends_with("\t// import 'imports/taskbar.nss'\r\nimport 'imports/settings-app.nss'"));
	CHECK(d.imports().size() == 5);

	CHECK(d.removeImport(4));
	CHECK(d.text().ends_with("\t// import 'imports/taskbar.nss'"));

	// A multi-line import cannot be commented out line by line.
	auto m = doc("import if(x,\r\n  'a.nss', 'b.nss')\r\n");
	CHECK_FALSE(m.toggleImport(0, false));

	// Trailing EOL is kept after appending.
	auto e = doc("menu(title='x') {}\n", Eol::LF);
	e.appendImport("it's@here.nss");
	CHECK(e.text() == "menu(title='x') {}\nimport \"it's@here.nss\"\n");
}

TEST_CASE("edits on a UTF-16 LE file keep encoding, BOM and CRLF")
{
	TextFormat f;
	f.encoding = Encoding::Utf16LE;
	f.bom = true;
	f.eol = Eol::CRLF;
	std::string text = "theme\r\n{\r\n\tfont.name=\"\xE5\xAE\x8B\xE4\xBD\x93\"\r\n}\r\n";
	auto bytes = encode(text, f);
	auto d = NssDocument::fromBytes(bytes);
	REQUIRE(d.has_value());
	CHECK(d->format().encoding == Encoding::Utf16LE);
	CHECK(d->serialize() == bytes);
	CHECK(d->insertAssign("theme", "font.size", "14"));
	auto expected = encode("theme\r\n{\r\n\tfont.name=\"\xE5\xAE\x8B\xE4\xBD\x93\"\r\n\tfont.size=14\r\n}\r\n", f);
	CHECK(d->serialize() == expected);
	CHECK(d->serialize().substr(0, 2) == "\xFF\xFE");
}

TEST_CASE("top-level dotted theme keys and block-own values")
{
	auto d = doc("theme.background.opacity = 50\r\ntheme.background { effect = [3, #202020, 50] }\r\ntheme = \"black\"\r\n");
	auto all = d.assigns();
	REQUIRE(all.size() == 3);
	CHECK(all[0]->path == "theme.background.opacity");
	CHECK(all[1]->path == "theme.background.effect");
	CHECK(all[2]->path == "theme");
	CHECK(d.setValue("theme.background.effect", "2"));
	CHECK(d.text() == "theme.background.opacity = 50\r\ntheme.background { effect = 2 }\r\ntheme = \"black\"\r\n");
}

TEST_CASE("literals")
{
	CHECK(Literal::parse("50").kind == LiteralKind::Number);
	CHECK(Literal::parse("-1.5").number == -1.5);
	CHECK(Literal::parse("0x10").number == 16);
	auto s = Literal::parse("\"a\\\"b\"");
	CHECK(s.kind == LiteralKind::String);
	CHECK(s.string == "a\"b");
	CHECK(Literal::parse("'plain'").string == "plain");
	CHECK(Literal::parse("'@app.dir'").kind == LiteralKind::Expression);
	auto c = Literal::parse("#f80");
	CHECK(c.kind == LiteralKind::Color);
	CHECK(c.rgba == 0xff8800ffu);
	CHECK(Literal::parse("#ff880080").rgba == 0xff880080u);
	CHECK(Literal::parse("#f808").rgba == 0xff880088u);
	CHECK(Literal::parse("auto").kind == LiteralKind::Keyword);
	CHECK(Literal::parse("TRUE").isBool());
	CHECK(Literal::parse("color.accent").kind == LiteralKind::Name);
	CHECK(Literal::parse("view.compact").kind == LiteralKind::Name);
	CHECK(Literal::parse("sep.top").kind == LiteralKind::Expression);
	auto a = Literal::parse("[3, #tint, 50]");
	CHECK(a.kind == LiteralKind::Expression);  // #tint is not hex
	auto b = Literal::parse("[3, #202020, 50]");
	REQUIRE(b.kind == LiteralKind::Array);
	REQUIRE(b.items.size() == 3);
	CHECK(b.items[1].text == "#202020");
	CHECK(Literal::parse("[[0.5, color.accent], [1, #fff]]").kind == LiteralKind::Array);
	CHECK(Literal::parse("#fff.opacity(50)").kind == LiteralKind::Expression);
	CHECK(Literal::parse("if(a, 1, 2)").kind == LiteralKind::Expression);
	CHECK(Literal::parse("1 + 2").kind == LiteralKind::Expression);

	CHECK(Literal::makeNumber(12).text == "12");
	CHECK(Literal::makeNumber(0.5).text == "0.5");
	CHECK(Literal::makeString("C:\\x \"y\"").text == "\"C:\\\\x \\\"y\\\"\"");
	CHECK(Literal::makeColor(0x202020ff).text == "#202020");
	CHECK(Literal::makeColor(0x20202080).text == "#20202080");
	CHECK(Literal::makeArray({ Literal::makeNumber(3), Literal::makeColor(0x112233ff), Literal::makeNumber(50) }).text ==
		  "[3, #112233, 50]");
}
