#include "TestUtil.h"

using namespace nss;
namespace fs = std::filesystem;

TEST_CASE("config graph: override chains across imports")
{
	test::TempDir dir;
	test::writeBytes(dir / "shell.nss",
		"$dir = 'imports'\r\n"
		"theme\r\n"
		"{\r\n"
		"\tdark = true\r\n"
		"\tbackground.opacity = 10\r\n"
		"}\r\n"
		"import dir + '/a.nss'\r\n"
		"import 'imports/b.nss'\r\n"
		"theme.background.opacity = 30\r\n"
		"import if(sys.is11, 'x.nss', 'y.nss')\r\n"
		"import 'missing.nss'\r\n"
		"// import 'imports/disabled.nss'\r\n");
	test::writeBytes(dir / "imports/a.nss",
		"theme { background.opacity = 20  font.size = 12 }\r\n"
		"import 'nested/c.nss'\r\n");
	test::writeBytes(dir / "imports/nested/c.nss",
		"theme.font.size = 14\r\n"
		"settings { tip.enabled = false }\r\n");
	test::writeBytes(dir / "imports/b.nss",
		"theme\r\n{\r\n\timport 'b-theme.nss'\r\n}\r\n");
	test::writeBytes(dir / "imports/b-theme.nss",
		"background { opacity = 25 }\r\nname = \"black\"\r\ntip.width = 300\r\n");
	test::writeBytes(dir / "imports/disabled.nss", "theme.dark = false\r\n");

	auto g = ConfigGraph::load(dir / "shell.nss");

	REQUIRE(g.files().size() == 6);
	CHECK(g.files()[0].path.filename() == "shell.nss");
	CHECK(g.files()[1].path.filename() == "a.nss");
	CHECK(g.files()[2].path.filename() == "c.nss");
	CHECK(g.files()[2].parent == 1);
	CHECK(g.files()[3].path.filename() == "b.nss");
	CHECK(g.files()[4].path.filename() == "b-theme.nss");
	CHECK(g.files()[4].context.mode == ParseMode::Settings);
	CHECK(g.files()[4].context.prefix == "theme");
	CHECK(g.files()[5].path.filename() == "missing.nss");
	CHECK(g.files()[5].error == "file not found");
	CHECK_FALSE(g.files()[5].doc);

	REQUIRE(g.unresolved().size() == 1);
	CHECK(g.unresolved()[0].line == 10);
	CHECK(g.unresolved()[0].reason.find("function call") != std::string::npos);

	auto opacity = g.find("theme.background.opacity");
	REQUIRE(opacity);
	REQUIRE(opacity->chain.size() == 4);
	CHECK(opacity->chain[0].value == "10");
	CHECK(opacity->chain[0].line == 5);
	CHECK(opacity->chain[1].value == "20");
	CHECK(opacity->chain[1].file.filename() == "a.nss");
	CHECK(opacity->chain[2].value == "25");
	CHECK(opacity->chain[2].file.filename() == "b-theme.nss");
	CHECK(opacity->effective().value == "30");
	CHECK(opacity->effective().file.filename() == "shell.nss");
	CHECK(opacity->effective().line == 9);
	CHECK(opacity->overriddenBy(0).size() == 3);
	CHECK(opacity->overriddenBy(2).size() == 1);
	CHECK(opacity->overriddenBy(3).empty());

	auto font = g.find("theme.font.size");
	REQUIRE(font);
	REQUIRE(font->chain.size() == 2);
	CHECK(font->effective().value == "14");
	CHECK(font->effective().file.filename() == "c.nss");
	CHECK(font->effective().literal);

	auto name = g.find("theme");  // alias of theme.name
	REQUIRE(name);
	CHECK(name->key == "theme.name");
	CHECK(name->effective().value == "\"black\"");

	CHECK(g.find("settings.tip.enabled")->effective().value == "false");
	auto tipWidth = g.find("theme.tip.width");  // theme.tip.* is settings.tip.*
	REQUIRE(tipWidth);
	CHECK(tipWidth->key == "settings.tip.width");
	CHECK(tipWidth->effective().writtenAs == "theme.tip.width");

	auto dark = g.find("theme.dark");
	REQUIRE(dark);
	CHECK(dark->chain.size() == 1);  // the disabled import is not followed
}

TEST_CASE("config graph: import cycles and expression evaluation")
{
	test::TempDir dir;
	test::writeBytes(dir / "shell.nss", "import 'a.nss'\r\n");
	test::writeBytes(dir / "a.nss", "theme.dark = true\r\nimport 'shell.nss'\r\n");
	auto g = ConfigGraph::load(dir / "shell.nss");
	CHECK(g.files().size() == 2);
	REQUIRE(g.unresolved().size() == 1);
	CHECK(g.unresolved()[0].reason == "import cycle");

	std::map<std::string, std::string> vars{ { "app.dir", "C:\\Shell" }, { "base", "x" } };
	CHECK(ConfigGraph::evaluate("app.dir + '\\imports\\' + \"a.nss\"", vars) == "C:\\Shell\\imports\\a.nss");
	CHECK(ConfigGraph::evaluate("'@app.dir\\@base'", vars) == "C:\\Shell\\x");
	// As in the core, a dot after @name continues the identifier.
	CHECK_FALSE(ConfigGraph::evaluate("'@base.nss'", vars));
	CHECK(ConfigGraph::evaluate("$base + '.nss'", vars) == "x.nss");
	CHECK(ConfigGraph::evaluate("'a@@b'", vars) == "a@b");
	CHECK(ConfigGraph::evaluate("'%NSS_TEST_UNSET_VAR%\\a'", vars) == "%NSS_TEST_UNSET_VAR%\\a");
	std::string reason;
	CHECK_FALSE(ConfigGraph::evaluate("path.join(a, b)", vars, &reason));
	CHECK_FALSE(ConfigGraph::evaluate("unknown + 'x'", vars, &reason));
	CHECK(reason == "unknown identifier 'unknown'");
	CHECK_FALSE(ConfigGraph::evaluate("'@str.lower(x)'", vars));
	CHECK_FALSE(ConfigGraph::evaluate("a - b", vars));
}

TEST_CASE("config graph: the shipped configuration")
{
	auto g = ConfigGraph::load(test::binDir() / "shell.nss");
	for(auto &f : g.files())
	{
		CAPTURE(f.path.string());
		CHECK(f.error.empty());
		CHECK(f.doc);
		if(f.doc)
			CHECK(f.doc->diagnostics().empty());
	}
	CHECK(g.files().size() == 11);  // root, en.nss and the nine imports
	REQUIRE(g.files().size() > 1);
	CHECK(g.files()[1].path.filename() == "en.nss");
	CHECK(g.files()[1].context.mode == ParseMode::Lang);

	// The locale-specific lang import is an if(...) expression.
	REQUIRE(g.unresolved().size() == 1);
	CHECK(g.unresolved()[0].line == 28);

	CHECK(g.find("theme.name")->effective().value == "\"modern\"");
	CHECK(g.find("theme.name")->effective().file.filename() == "theme.nss");
	CHECK(g.find("theme.image.display")->effective().writtenAs == "theme.image.align");
	CHECK(g.find("settings.priority")->effective().value == "1");
	CHECK(g.find("settings.exclude.where")->effective().literal == false);
	CHECK(g.find("settings.showdelay") == nullptr);
}
