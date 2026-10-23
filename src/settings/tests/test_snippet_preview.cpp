#include "doctest.h"

#include "SnippetPreview.h"

#include "nss/NssDocument.h"
#include "nss/Text.h"

#include <filesystem>
#include <fstream>
#include <map>

namespace fs = std::filesystem;
using namespace snippetpreview;

namespace {

std::string identity(const char *s) { return s; }

// A tree of Shell's items as `shell.exe -config:tree` prints them for the snippet below.
const char *kTree = R"({
  "ok": true, "root": "C:\\tmp\\p\\root.nss", "context": "file", "sample": "C:\\tmp\\p\\Sample.zip",
  "errors": [], "warnings": [],
  "tree": {"shell":[
    {"kind":"menu","title":"Archive","state":"text","source":"","file":"C:\\tmp\\p\\snippet.nss","line":1,"column":1,"vis":"","conditions":["type"],"dynamic":false,
     "items":[{"kind":"item","title":"Extract &here","state":"text","source":"","file":"C:\\tmp\\p\\snippet.nss","line":2,"column":2,"vis":"","conditions":["find"],"dynamic":false,"items":[]},
              {"kind":"separator","title":"","state":"none","source":"","file":"C:\\tmp\\p\\snippet.nss","line":3,"column":2,"vis":"","conditions":[],"dynamic":false,"items":[]},
              {"kind":"item","title":"","state":"dynamic","source":"","file":"C:\\tmp\\p\\snippet.nss","line":4,"column":2,"vis":"","conditions":[],"dynamic":true,"items":[]},
              {"kind":"item","title":"Gone","state":"text","source":"","file":"C:\\tmp\\p\\snippet.nss","line":5,"column":2,"vis":"hidden","conditions":[],"dynamic":false,"items":[]}]},
    {"kind":"menu","title":"","state":"none","source":"","file":"C:\\tmp\\p\\snippet.nss","line":7,"column":1,"vis":"","conditions":[],"dynamic":false,
     "items":[{"kind":"item","title":"Inline one","state":"text","source":"","file":"C:\\tmp\\p\\snippet.nss","line":8,"column":2,"vis":"","conditions":[],"dynamic":false,"items":[]}]},
    {"kind":"item","title":"At the top","state":"text","source":"","file":"C:\\tmp\\p\\snippet.nss","line":10,"column":1,"vis":"","conditions":[],"dynamic":false,"items":[]},
    {"kind":"item","title":"From the theme","state":"text","source":"","file":"C:\\cfg\\imports\\theme.nss","line":1,"column":1,"vis":"","conditions":[],"dynamic":false,"items":[]}
  ], "system": [], "rules": []}
})";

const char *kSnippet =
    "menu(type='file' title='Archive' image=icon.compressed) {\n"
    "\titem(find='.zip|.7z' title='Extract here' image=inherit cmd='x')\n"
    "\tseparator()\n"
    "\titem(title=sel.name image=image.mdl(\\uE785) cmd='x')\n"
    "\titem(title='Gone' vis=vis.remove cmd='x')\n"
    "}\n"
    "menu(expanded=true) {\n"
    "\titem(title='Inline one' image='@sys.prog\\7-Zip\\7zFM.exe' cmd='x')\n"
    "}\n"
    "item(title='At the top' pos='top' sep='after' image=\\uE0AA cmd='x')\n";

const char *kImages =
    "$color3 = @if(theme.islight,image.color3,'none')\n"
    "$clip='<defs id=\"c\"/>'\n"
    "@compressed, zip='<svg viewBox=\"0 0 16 16\">\n"
    "  <path fill=\"@image.color1\" d=\"M0 0\"/><path fill=\"@color3\" d=\"M1 1\"/>@clip\n"
    "</svg>'\n"
    "@copy='<svg><path fill=\"@image.color2\"/></svg>'\n"
    "@extract_all='<svg>\n  <path fill=\"@if(theme.islight,'#fff','none')\"/>\n</svg>'\n"
    "@after='<svg/>'\n";

ImageDefs imageDefs()
{
    ImageDefs defs;
    collectImageDefs(kImages, defs);
    return defs;
}

} // namespace

TEST_CASE("snippet preview: contexts")
{
    CHECK(std::string(contextName(Context::Back)) == "back");
    CHECK(std::string(contextLabel(Context::Back)) == "Background");
    CHECK(std::size(kContexts) == 6);
}

TEST_CASE("snippet preview: the scratch root imports the theme and images, then the snippet")
{
    const fs::path dir = L"C:\\cfg";
    const std::map<std::string, std::string> files = {
        { nss::pathKey(dir / L"imports" / L"theme.nss"), "theme\n{\n\tname = \"modern\"\n}\n" },
        { nss::pathKey(dir / L"imports" / L"images.nss"), kImages },
        { nss::pathKey(dir / L"imports" / L"terminal.nss"), "menu(title='Terminal') { }\n" },
        { nss::pathKey(dir / L"imports" / L"lang" / L"en.nss"), "a = 'b'\n" },
    };
    ReadFile read = [&](const fs::path &p) -> std::optional<std::string> {
        auto it = files.find(nss::pathKey(p));
        if (it == files.end())
            return std::nullopt;
        return it->second;
    };
    auto root = nss::NssDocument::fromText("settings { priority=1 }\n"
                                           "import 'imports/theme.nss'\n"
                                           "import 'imports/images.nss'\n"
                                           "// import 'imports/old-theme.nss'\n"
                                           "import 'imports/terminal.nss'\n"
                                           "import 'imports/missing.nss'\n"
                                           "import app.dir + '\\x.nss'\n"
                                           "theme { dark = true }\n"
                                           "menu(title='Mine') { }\n");
    auto src = themeSource(root, dir, read);
    REQUIRE(src.imports.size() == 2);
    CHECK(nss::pathKey(src.imports[0]) == nss::pathKey(dir / L"imports" / L"theme.nss"));
    CHECK(nss::pathKey(src.imports[1]) == nss::pathKey(dir / L"imports" / L"images.nss"));
    CHECK(src.inlineTheme == "theme { dark = true }");
    REQUIRE(src.lang);

    auto text = scratchRoot(src, "snippet.nss");
    CHECK(text.find("import lang 'C:\\cfg\\imports\\lang\\en.nss'") != std::string::npos);
    const auto theme = text.find("import 'C:\\cfg\\imports\\theme.nss'");
    const auto images = text.find("import 'C:\\cfg\\imports\\images.nss'");
    const auto inlineTheme = text.find("theme { dark = true }");
    const auto snippet = text.find("import 'snippet.nss'");
    CHECK(theme != std::string::npos);
    CHECK(images > theme);
    CHECK(inlineTheme > images);
    CHECK(snippet > inlineTheme);
    CHECK(text.find("terminal.nss") == std::string::npos);
    CHECK(text.find("Mine") == std::string::npos);
    // The scratch root parses.
    auto doc = nss::NssDocument::fromText(text);
    CHECK(doc.diagnostics().empty());
    CHECK(doc.imports().size() == 4);

    // No config: only the snippet.
    CHECK(scratchRoot({}, "snippet.nss").find("import 'snippet.nss'") != std::string::npos);
}

TEST_CASE("snippet preview: the sample file follows find= and where=")
{
    auto s = sampleFor("item(type='file' find='.zip' title='x' cmd='x')");
    CHECK(s.extension == ".zip");
    CHECK(s.fromSnippet);

    s = sampleFor("item(type='file' find='.txt|.md|.log' title='x' cmd='x')");
    CHECK(s.extension == ".txt");
    CHECK(s.fromSnippet);

    s = sampleFor("item(type='file' find='*.PNG' title='x' cmd='x')");
    CHECK(s.extension == ".png");

    s = sampleFor("menu(type='file' title='m') { item(where=sel.file.ext == '.pdf' title='x' cmd='x') }");
    CHECK(s.extension == ".pdf");
    CHECK(s.fromSnippet);

    // A name or a pattern that isn't an extension: the default sample.
    s = sampleFor("item(type='file' find='readme' title='x' cmd='x')");
    CHECK(s.extension == ".txt");
    CHECK_FALSE(s.fromSnippet);
    s = sampleFor("item(type='file|dir' title='x' cmd='x')");
    CHECK_FALSE(s.fromSnippet);
}

TEST_CASE("snippet preview: icons")
{
    auto defs = imageDefs();
    REQUIRE(defs.svgs.count("compressed"));
    CHECK(defs.svgs.count("zip"));
    CHECK(defs.svgs.count("copy"));
    CHECK(defs.vars.count("color3"));

    auto i = iconOf("\\uE0AA", defs);
    CHECK(i.kind == IconKind::Glyph);
    CHECK(i.font == GlyphFont::Shell);
    CHECK(i.glyph == "\xEE\x82\xAA");
    CHECK(iconOf("'\\uE0AA'", defs).glyph == "\xEE\x82\xAA");
    CHECK(iconOf("[\\uE0AA, #ff0000]", defs).glyph == "\xEE\x82\xAA");

    i = iconOf("image.mdl(\\uE785)", defs);
    CHECK(i.kind == IconKind::Glyph);
    CHECK(i.font == GlyphFont::Segoe);
    CHECK(iconOf("icon.fluent(\\uE721, #fff)", defs).font == GlyphFont::Segoe);
    CHECK(iconOf("image.glyph(\\uE001)", defs).font == GlyphFont::Shell);

    i = iconOf("icon.compressed", defs);
    CHECK(i.kind == IconKind::Svg);
    CHECK(i.svg.find("<svg") == 0);
    CHECK(iconOf("image.zip", defs).kind == IconKind::Svg);
    CHECK(iconOf("icon.unknown_name", defs).kind == IconKind::None);

    i = iconOf("'@sys.prog\\7-Zip\\7zFM.exe'", defs);
    CHECK(i.kind == IconKind::File);
    CHECK(i.path == "@sys.prog\\7-Zip\\7zFM.exe");
    i = iconOf("'C:\\x\\app.dll,3'", defs);
    CHECK(i.path == "C:\\x\\app.dll");
    CHECK(i.index == 3);
    i = iconOf("icon('C:\\x\\app.dll', -5)", defs);
    CHECK(i.kind == IconKind::File);
    CHECK(i.index == -5);

    i = iconOf("image.svg('<svg><path/></svg>')", defs);
    CHECK(i.kind == IconKind::Svg);
    CHECK(i.svg == "<svg><path/></svg>");

    // inherit and a bare `image` take the menu's; none is none.
    const Icon parent{ IconKind::Glyph, "x", GlyphFont::Shell };
    CHECK(iconOf("inherit", defs, &parent) == parent);
    CHECK(iconOf("", defs, &parent) == parent);
    CHECK(iconOf("none", defs, &parent).kind == IconKind::None);

    // A variable holding an image.
    ImageDefs withVar = defs;
    collectImageDefs("$svg_info=image.svg('<svg viewBox=\"0 0 1 1\">\n<path/>\n</svg>')\n", withVar);
    REQUIRE(withVar.vars.count("svg_info"));
    i = iconOf("svg_info", withVar);
    CHECK(i.kind == IconKind::Svg);
    CHECK(i.svg.find("<path/>") != std::string::npos);
}

TEST_CASE("snippet preview: SVG colours and variables")
{
    auto defs = imageDefs();
    const uint32_t colors[3] = { 0xFF555555, 0xFF0078D4, 0xFFFAFAFA };
    auto light = svgFor(defs.svgs["compressed"], defs, colors, true);
    CHECK(light.find("fill=\"#555555\"") != std::string::npos);
    CHECK(light.find("fill=\"#FAFAFA\"") != std::string::npos);
    CHECK(light.find("<defs id=\"c\"/>") != std::string::npos);
    CHECK(light.find('@') == std::string::npos);
    auto dark = svgFor(defs.svgs["compressed"], defs, colors, false);
    CHECK(dark.find("fill=\"none\"") != std::string::npos);
    CHECK(svgFor(defs.svgs["copy"], defs, colors, true).find("#0078D4") != std::string::npos);
    // @if(...) written in the SVG itself (images.nss: extract_all).
    const char *inlineIf = "<path fill=\"@if(theme.islight,'#fff','none')\" stroke=\"@if(theme.islight, @image.color1, '#000')\"/>";
    CHECK(svgFor(inlineIf, defs, colors, true) == "<path fill=\"#fff\" stroke=\"#555555\"/>");
    CHECK(svgFor(inlineIf, defs, colors, false) == "<path fill=\"none\" stroke=\"#000\"/>");
    // ...and its quotes don't end the image definition.
    REQUIRE(defs.svgs.count("extract_all"));
    CHECK(defs.svgs["extract_all"] == "<svg>\n  <path fill=\"@if(theme.islight,'#fff','none')\"/>\n</svg>");
    CHECK(svgFor(defs.svgs["extract_all"], defs, colors, true) == "<svg>\n  <path fill=\"#fff\"/>\n</svg>");
    CHECK(defs.svgs.count("after"));
    // A transparent colour is none; an unknown variable too.
    const uint32_t clear[3] = { 0x00000000, 0, 0 };
    CHECK(svgFor("<path fill=\"@image.color1\" stroke=\"@nope\"/>", defs, clear, true) ==
          "<path fill=\"none\" stroke=\"none\"/>");
}

TEST_CASE("snippet preview: file icon paths")
{
    std::map<std::string, std::string> vars{ { "sys.prog", "C:\\Program Files" },
                                             { "sys.prog32", "C:\\Program Files (x86)" },
                                             { "sys.dir", "C:\\Windows" } };
    CHECK(expandPath("'@sys.prog\\7-Zip\\7zFM.exe'", vars) == "C:\\Program Files\\7-Zip\\7zFM.exe");
    CHECK(expandPath("@sys.prog32\\x.exe", vars) == "C:\\Program Files (x86)\\x.exe");
    CHECK(expandPath("@sys.dir\\notepad.exe", vars) == "C:\\Windows\\notepad.exe");
    CHECK(expandPath("@user.x\\a.ico", vars) == "@user.x\\a.ico");
}

TEST_CASE("snippet preview: the tree's items become the snippet's rows")
{
    auto tree = menumap::parseTree(kTree);
    REQUIRE(tree);
    auto doc = nss::NssDocument::fromText(kSnippet);
    auto defs = imageDefs();
    auto items = itemsOf(tree->shell, L"C:\\tmp\\p\\snippet.nss", doc, defs);
    REQUIRE(items.size() == 3); // the theme's item is not the snippet's

    const auto &archive = items[0];
    CHECK(archive.kind == "menu");
    CHECK(archive.title == "Archive");
    CHECK(archive.icon.kind == IconKind::Svg);
    REQUIRE(archive.children.size() == 3); // the hidden one is left out
    CHECK(archive.children[0].title == "Extract here");
    CHECK(archive.children[0].icon == archive.icon); // image=inherit
    CHECK(archive.children[1].kind == "separator");
    CHECK(archive.children[2].dynamicTitle);
    CHECK(archive.children[2].icon.font == GlyphFont::Segoe);

    CHECK(items[1].expanded);
    REQUIRE(items[1].children.size() == 1);
    CHECK(items[1].children[0].icon.kind == IconKind::File);

    CHECK(items[2].pos == Pos::Top);
    CHECK(items[2].sepAfter);
    CHECK_FALSE(items[2].sepBefore);
    CHECK(items[2].icon.kind == IconKind::Glyph);

    SUBCASE("composed between Windows' items")
    {
        auto c = compose(Context::File, items, identity);
        // At the top, then Open and Copy, the menu and the expanded item, then Properties.
        std::vector<std::string> labels;
        for (const auto &r : c.main.rows)
            labels.push_back(r.separator ? "-" : r.label);
        CHECK(labels == std::vector<std::string>{ "At the top", "-", "Open", "Copy", "-", "Archive", "Inline one", "-",
                                                  "Properties" });
        CHECK(c.own == 3);
        CHECK(c.main.rows[0].own);
        CHECK_FALSE(c.main.rows[2].own);
        CHECK(c.main.rows[2].disabled); // greyed
        CHECK(c.main.rows[2].icon.font == GlyphFont::Segoe);
        CHECK(c.main.rows[6].icon.kind == IconKind::File);

        // The menu opens at first; its submenu has the items and the placeholder.
        CHECK(c.open == 5);
        REQUIRE(c.main.rows[5].sub == 0);
        REQUIRE(c.subs.size() == 1);
        const auto &sub = c.subs[0].rows;
        REQUIRE(sub.size() == 3);
        CHECK(sub[0].label == "Extract here");
        CHECK(sub[1].separator);
        CHECK(sub[2].placeholder);
        CHECK(sub[2].label == "Name from the selection");
        CHECK(sub[2].own);
    }

    SUBCASE("the desktop and the taskbar have their own frames")
    {
        auto desktop = compose(Context::Desktop, items, identity);
        CHECK(desktop.main.rows.back().label == "Personalize");
        auto taskbar = compose(Context::Taskbar, { items[2] }, identity);
        // pos=top, then the taskbar's items; no separator first.
        REQUIRE(taskbar.main.rows.size() == 4);
        CHECK(taskbar.main.rows[0].label == "At the top");
        CHECK(taskbar.main.rows[1].separator);
        CHECK(taskbar.main.rows[2].label == "Task Manager");
        CHECK(taskbar.open == -1);
    }
}

TEST_CASE("snippet preview: separators never lead, trail or double")
{
    Item sep;
    sep.kind = "separator";
    Item a;
    a.kind = "item";
    a.title = "A";
    a.sepBefore = true;
    a.sepAfter = true;
    auto c = compose(Context::Drive, { sep, a, sep }, identity);
    std::vector<std::string> labels;
    for (const auto &r : c.main.rows)
        labels.push_back(r.separator ? "-" : r.label);
    CHECK(labels == std::vector<std::string>{ "Open", "-", "A", "-", "Properties" });

    // Separators only: nothing of the snippet's.
    auto tree = menumap::parseTree(R"({"ok":true,"root":"","context":"file","sample":"","errors":[],"warnings":[],
        "tree":{"shell":[{"kind":"separator","title":"","state":"none","source":"","file":"C:\\s.nss","line":1,"column":1,"vis":"","conditions":[],"dynamic":false,"items":[]}],"system":[],"rules":[]}})");
    REQUIRE(tree);
    CHECK(itemsOf(tree->shell, L"C:\\s.nss", nss::NssDocument::fromText("separator()\n"), {}).empty());
}

TEST_CASE("snippet preview: a bundled snippet's file parses into icons")
{
    // Every bundled snippet's image= is one the preview understands.
    const fs::path dir = fs::path(SETTINGS_SOURCE_BIN) / L"snippets";
    int checked = 0;
    for (const auto &e : fs::directory_iterator(dir)) {
        if (e.path().extension() != L".nss")
            continue;
        std::string text;
        REQUIRE(nss::readFile(e.path(), text));
        auto doc = nss::NssDocument::fromText(text);
        ImageDefs defs;
        collectImageDefs(text, defs);
        std::string bundledImages;
        REQUIRE(nss::readFile(fs::path(SETTINGS_SOURCE_BIN) / L"imports" / L"images.nss", bundledImages));
        collectImageDefs(bundledImages, defs);
        doc.visit([&](const nss::Node &n) {
            if (n.kind != nss::NodeKind::MenuDecl)
                return true;
            for (const auto &p : n.props)
                if (p.name == "image" && p.hasValue) {
                    INFO(nss::narrow(e.path().filename().wstring()) << ": " << doc.slice(p.valueSpan));
                    const auto icon = iconOf(doc.slice(p.valueSpan), defs);
                    CHECK(icon.kind != IconKind::None);
                    if (icon.kind == IconKind::Svg) {
                        // Every @ is filled in, so the SVG loads.
                        const uint32_t colors[3] = { 0xFF555555, 0xFF0078D4, 0xFFFAFAFA };
                        for (bool light : { true, false }) {
                            const auto svg = svgFor(icon.svg, defs, colors, light);
                            CHECK(svg.find('@') == std::string::npos);
                            CHECK(svg.find("if(") == std::string::npos);
                            CHECK(svg.find("</svg>") != std::string::npos);
                        }
                    }
                    ++checked;
                }
            return true;
        });
    }
    CHECK(checked >= 10);
}
