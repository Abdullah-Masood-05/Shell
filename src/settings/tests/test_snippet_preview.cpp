#include "doctest.h"

#include "SnippetPreview.h"

#include "nss/NssDocument.h"
#include "nss/Text.h"

#include <algorithm>
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

    // '."zip"': exactly that extension.
    s = sampleFor("item(type='file' find='.\"zip\"' title='x' cmd='x')");
    CHECK(s.extension == ".zip");
    CHECK(s.fromSnippet);
    s = sampleFor("item(type='file' find='.\"md5\"|.\"sha1\"' title='x' cmd='x')");
    CHECK(s.extension == ".md5");

    // A variable holding the list.
    s = sampleFor("$ext_list = '.mkv|.mp4'\nitem(type='file' find=ext_list title='x' cmd='x')");
    CHECK(s.extension == ".mkv");

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

TEST_CASE("snippet preview: icon colours")
{
    auto c = colorOf("#f00");
    REQUIRE(c);
    CHECK(c->argb == 0xFFFF0000);
    CHECK(colorOf("#00FF0080")->argb == 0x8000FF00);
    CHECK(colorOf("#123456")->argb == 0xFF123456);
    CHECK(colorOf("color.greenyellow")->argb == 0xFFADFF2F);
    CHECK(colorOf("image.color2")->theme == 2);
    CHECK_FALSE(colorOf("auto")->given());
    CHECK(colorOf("rgb(255, 0, 0)")->argb == 0xFFFF0000);
    CHECK_FALSE(colorOf("#12"));
    CHECK_FALSE(colorOf("sel.path"));

    auto defs = imageDefs();
    const uint32_t theme[3] = { 0xFF111111, 0xFF222222, 0xFF333333 };

    // A glyph's colour: [glyph, colour], image.mdl(glyph, colour, size).
    auto i = iconOf("[\\uE187, image.color2]", defs);
    CHECK(i.kind == IconKind::Glyph);
    CHECK(glyphColor(i, theme) == 0xFF222222);
    i = iconOf("image.mdl(\\uE8B7, #f00, 12)", defs);
    CHECK(i.font == GlyphFont::Segoe);
    CHECK(glyphColor(i, theme) == 0xFFFF0000);
    CHECK(glyphColor(iconOf("image.glyph(\\uE0CE, 14)", defs), theme) == 0); // the theme's

    // icon.copy(auto, color.red): the SVG with its second colour replaced.
    i = iconOf("icon.copy(auto, color.red)", defs);
    REQUIRE(i.kind == IconKind::Svg);
    uint32_t colors[3];
    iconColors(i, theme, colors);
    CHECK(colors[0] == 0xFF111111);
    CHECK(colors[1] == 0xFFFF0000);
    CHECK(colors[2] == 0xFF333333);
    CHECK(svgFor(i.svg, defs, colors, true).find("#FF0000") != std::string::npos);
}

TEST_CASE("snippet preview: icons Shell works out")
{
    ImageDefs defs;
    collectImageDefs("$exe_64 = '@sys.prog\\Everything\\Everything.exe'\n"
                     "$exe_32 = '@sys.prog32\\Everything\\Everything.exe'\n"
                     "$path_ev = eval(if(path.exists(exe_64), exe_64, if(path.exists(exe_32), exe_32, '')))\n"
                     "$dll = path.combine(sys.bin, 'imageres.dll')\n",
                     defs);

    // if(path.exists(x), x, y): x, then y when x can't be read.
    auto i = iconOf("if(path.exists(path_ev), path_ev, [\\uE187, image.color2])", defs);
    REQUIRE(i.kind == IconKind::File);
    CHECK(i.path == "@sys.prog\\Everything\\Everything.exe");
    REQUIRE(i.fallback);
    CHECK(i.fallback->path == "@sys.prog32\\Everything\\Everything.exe");
    REQUIRE(i.fallback->fallback);
    CHECK(i.fallback->fallback->kind == IconKind::Glyph);
    CHECK(i.fallback->fallback->colors[0].theme == 2);
    CHECK_FALSE(i.fallback->fallback->fallback);

    // A path from path.combine() and a variable.
    i = iconOf("icon.res(dll, -5)", defs);
    REQUIRE(i.kind == IconKind::File);
    CHECK(i.path == "@sys.bin\\imageres.dll");
    CHECK(i.index == -5);
    i = iconOf("image.res('@sys.bin\\desk.cpl', 0)", defs);
    CHECK(i.path == "@sys.bin\\desk.cpl");

    // Can't be told without running it: nothing, never a guess.
    CHECK(iconOf("icon.res(reg.get('HKCU\\x'), 0)", defs).kind == IconKind::None);
    CHECK(iconOf("eval(appx('Some', 'logo'))", defs).kind == IconKind::None);
    CHECK(iconOf("if(path.exists(nope), nope, '')", defs).kind == IconKind::None);

    // An images.nss name the config doesn't define: a Segoe glyph like it.
    i = iconOf("icon.select_all", defs);
    CHECK(i.kind == IconKind::Glyph);
    CHECK(i.font == GlyphFont::Segoe);
    CHECK(iconOf("icon.copy", defs).kind == IconKind::Glyph);
    CHECK(iconOf("icon.copy", imageDefs()).kind == IconKind::Svg); // images.nss wins
    CHECK(iconOf("icon.no_such_image", defs).kind == IconKind::None);
}

TEST_CASE("snippet preview: titles as the menu shows them")
{
    auto t = splitTitle("Copy &lnk path");
    CHECK(t.label == "Copy lnk path");
    CHECK(t.accel == 5);
    CHECK(t.keys.empty());
    t = splitTitle("Save && close");
    CHECK(t.label == "Save & close");
    CHECK(t.accel == -1);
    t = splitTitle("&Open\tCtrl+O");
    CHECK(t.label == "Open");
    CHECK(t.accel == 0);
    CHECK(t.keys == "Ctrl+O");
    t = splitTitle("Go To\tTemporary");
    CHECK(t.label == "Go To");
    CHECK(t.keys == "Temporary");

    // keys= and where=key.shift() from the declaration.
    auto tree = menumap::parseTree(R"({"ok":true,"root":"","context":"file","sample":"","errors":[],"warnings":[],
        "tree":{"shell":[
          {"kind":"item","title":"Select &all","state":"text","source":"","file":"C:\\s.nss","line":1,"column":1,"vis":"","conditions":[],"dynamic":false,"items":[]},
          {"kind":"item","title":"Copy MD2","state":"text","source":"","file":"C:\\s.nss","line":2,"column":1,"vis":"","conditions":["where"],"dynamic":false,"items":[]},
          {"kind":"item","title":"Go To\tTemporary","state":"text","source":"","file":"C:\\s.nss","line":3,"column":1,"vis":"","conditions":[],"dynamic":false,"items":[]},
          {"kind":"item","title":"Network path","state":"text","source":"","file":"C:\\s.nss","line":4,"column":1,"vis":"","conditions":["where"],"dynamic":false,"items":[]},
          {"kind":"item","title":"Not with Shift","state":"text","source":"","file":"C:\\s.nss","line":5,"column":1,"vis":"","conditions":["where"],"dynamic":false,"items":[]}
        ],"system":[],"rules":[]}})");
    REQUIRE(tree);
    auto doc = nss::NssDocument::fromText("item(title='Select &all' keys='Ctrl+A' image=icon.select_all cmd='x')\n"
                                          "item(title='Copy MD2' where=key.Shift() cmd='x')\n"
                                          "item(title='Go To@\"\\t\"Temporary' cmd='x')\n"
                                          "item(title='Network path' where=keys.shift() and str.contains(sel.path, ':') cmd='x')\n"
                                          "item(title='Not with Shift' where=!key.shift() cmd='x')\n");
    auto items = itemsOf(tree->shell, L"C:\\s.nss", doc, {});
    REQUIRE(items.size() == 3); // Copy MD2 and Network path show only with Shift held
    CHECK(items[2].title == "Not with Shift");
    CHECK(items[0].title == "Select all");
    CHECK(items[0].accel == 7);
    CHECK(items[0].keys == "Ctrl+A");
    CHECK(items[0].icon.kind == IconKind::Glyph);
    CHECK(items[1].title == "Go To");
    CHECK(items[1].keys == "Temporary");
    auto c = compose(Context::File, items, identity);
    bool found = false;
    for (const auto &r : c.main.rows)
        if (r.label == "Select all") {
            found = true;
            CHECK(r.accel == 7);
            CHECK(r.keys == "Ctrl+A");
        }
    CHECK(found);
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
        CHECK(c.main.rows[2].ghost); // faded, not disabled
        CHECK_FALSE(c.main.rows[2].disabled);
        // The outline goes around each run of the snippet's rows.
        CHECK(c.main.rows[0].groupFirst);
        CHECK(c.main.rows[0].groupLast);
        CHECK(c.main.rows[5].groupFirst);
        CHECK_FALSE(c.main.rows[5].groupLast);
        CHECK(c.main.rows[6].groupLast);
        CHECK(c.main.rows[2].icon.font == GlyphFont::Segoe);
        CHECK(c.main.rows[6].icon.kind == IconKind::File);

        // The menu opens at first; its submenu has the items and the placeholder.
        CHECK(c.open == 5);
        REQUIRE(c.main.rows[5].sub == 0);
        REQUIRE(c.subs.size() == 1);
        const auto &sub = c.subs[0].rows;
        REQUIRE(sub.size() == 3);
        CHECK(sub[0].label == "Extract here");
        CHECK(sub[0].accel == 8); // Extract &here
        CHECK(sub[1].separator);
        CHECK(sub[2].placeholder);
        CHECK(sub[2].label == "Name from the selection");
        CHECK(sub[2].own);
    }

    SUBCASE("the desktop and the taskbar have their own frames")
    {
        // At most two of Windows' rows before the snippet's and one after.
        auto desktop = compose(Context::Desktop, items, identity);
        std::vector<std::string> labels;
        for (const auto &r : desktop.main.rows)
            labels.push_back(r.separator ? "-" : r.label);
        CHECK(labels == std::vector<std::string>{ "At the top", "-", "Sort by", "Refresh", "-", "Archive", "Inline one",
                                                  "-", "Display settings" });
        auto taskbar = compose(Context::Taskbar, { items[2] }, identity);
        // pos=top, then the taskbar's items; no separator first.
        REQUIRE(taskbar.main.rows.size() == 3);
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
    // Worked out only when the menu shows: the registry, an installed Store app.
    const std::vector<std::string> unresolved = { "icon.res(path_db_dll, 0)", "ico_mpcbe", "ico_od_win" };
    int checked = 0;
    for (const auto &e : fs::recursive_directory_iterator(dir)) {
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
                    const std::string value(nss::trim(doc.slice(p.valueSpan)));
                    INFO(nss::narrow(e.path().filename().wstring()) << ": " << value);
                    const Icon parent{ IconKind::Glyph, "x", GlyphFont::Shell };
                    const auto icon = iconOf(value, defs, &parent);
                    if (std::find(unresolved.begin(), unresolved.end(), value) == unresolved.end())
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
    CHECK(checked >= 100);
}

TEST_CASE("snippet preview: example values for titles and where= that depend on the selection")
{
    Sample zip;
    zip.extension = ".zip";
    zip.fromSnippet = true;
    const auto file = exampleFor(Context::File, zip);
    CHECK(exampleValue("sel.path", file) == "D:\\Work\\Photos.zip");
    CHECK(exampleValue("@sel.file.name", file) == "Photos.zip");
    CHECK(exampleValue("@sel.file.title", file) == "Photos");
    CHECK(exampleValue("@sel.file.ext", file) == ".zip");
    CHECK(exampleValue("@sel.parent", file) == "D:\\Work");
    CHECK(exampleValue("'\\\\localhost\\' + str.replace(sel.path, ':', '$')", file) ==
          "\\\\localhost\\D$\\Work\\Photos.zip");
    CHECK(exampleValue("path.wsl(sel.path)", file) == "/mnt/d/Work/Photos.zip");
    CHECK(exampleValue("str.replace(path.wsl(sel.path), '/mnt/', '/cygdrive/')", file) ==
          "/cygdrive/d/Work/Photos.zip");
    CHECK(exampleValue("str.right(path.wsl(sel.path), len(path.wsl(sel.path))-len('/mnt/d'))", file) ==
          "/Work/Photos.zip");
    CHECK(exampleValue("'file://' + path.separator(sel.path)", file) == "file://D:/Work/Photos.zip");
    CHECK(exampleValue("str.fmt('{0} ({1})', sel.file.title, sel.count)", file) == "Photos (1)");

    // Conditions: told when they can be, else unknown.
    CHECK(exampleTest("(sel.raw != sel.path)", file) == false);
    CHECK(exampleTest("(sel.short != sel.path) and len(sel.short)>0", file) == false); // 8.3 already
    CHECK(exampleTest("(sel.short != sel.path) and len(sel.short)>0", exampleFor(Context::Folder, {})) == true);
    CHECK(exampleTest("sel.parent.len>3 and !str.start(sel.raw, '::')", file) == true);
    CHECK(exampleTest("sel.file.ext.len>0", file) == true);
    CHECK(exampleTest("app.exists('bookmark')", file) == false);
    CHECK(exampleTest("path.exists(ip[toint(do[0])])", file) == false);
    CHECK_FALSE(exampleTest("sel.type == type.file", file).has_value());
    CHECK_FALSE(exampleValue("ip[toint(do[0])]", file).has_value());
    CHECK_FALSE(exampleValue("sys.screen.width", file).has_value());
    CHECK_FALSE(exampleValue("'Go To@\"\\t\"x'", file).has_value());

    const auto folder = exampleFor(Context::Folder, {});
    CHECK(exampleValue("sel.path", folder) == "D:\\Work\\Projects");
    CHECK(exampleTest("sel.file.ext.len>0", folder) == false);
    CHECK(exampleTest("sel.count > 0", exampleFor(Context::Taskbar, {})) == false);
}

TEST_CASE("snippet preview: with an example, dynamic titles read like the selection")
{
    const char *tree = R"({
  "ok": true, "root": "C:\\s\\root.nss", "context": "file", "sample": "C:\\s\\Sample.txt",
  "tree": {"shell":[
    {"kind":"menu","title":"Copy path","state":"text","source":"","file":"C:\\s.nss","line":1,"column":1,"vis":"","conditions":[],"dynamic":false,
     "items":[{"kind":"item","title":"","state":"dynamic","source":"","file":"C:\\s.nss","line":2,"column":2,"vis":"","conditions":[],"dynamic":true,"items":[]},
              {"kind":"item","title":"","state":"dynamic","source":"","file":"C:\\s.nss","line":3,"column":2,"vis":"","conditions":["where"],"dynamic":true,"items":[]},
              {"kind":"item","title":"","state":"dynamic","source":"","file":"C:\\s.nss","line":4,"column":2,"vis":"","conditions":[],"dynamic":true,"items":[]}]}],
  "system":[], "rules":[]}})";
    const auto doc = nss::NssDocument::fromText("menu(title='Copy path') {\n"
                                                "\titem(title=sel.path)\n"
                                                "\titem(title=sel.raw where=(sel.raw != sel.path))\n"
                                                "\titem(title=ip[0])\n"
                                                "}\n");
    auto t = menumap::parseTree(tree);
    REQUIRE(t);
    const auto ex = exampleFor(Context::File, {});
    auto items = itemsOf(t->shell, L"C:\\s.nss", doc, {}, &ex);
    REQUIRE(items.size() == 1);
    const auto &rows = items[0].children;
    REQUIRE(rows.size() == 2); // sel.raw is the same as sel.path here: left out
    CHECK(rows[0].title == "D:\\Work\\report.txt");
    CHECK_FALSE(rows[0].dynamicTitle);
    CHECK(rows[1].dynamicTitle); // can't be told: the placeholder stays

    // Without an example, as before.
    auto plain = itemsOf(t->shell, L"C:\\s.nss", doc, {});
    REQUIRE(plain.size() == 1);
    CHECK(plain[0].children.size() == 3);
}

TEST_CASE("snippet preview: example values work like Shell's")
{
    const auto file = exampleFor(Context::File, {});
    const auto folder = exampleFor(Context::Folder, {});
    const auto back = exampleFor(Context::Back, {});
    // sel.dir: a file's folder, else the item itself; sel.file: the name.
    CHECK(exampleValue("sel.dir", file) == "D:\\Work");
    CHECK(exampleValue("sel.dir", folder) == "D:\\Work\\Projects");
    CHECK(exampleValue("sel.dir.name", folder) == "Projects");
    CHECK(exampleValue("sel.dir", back) == "D:\\Work");
    CHECK(exampleValue("sel.parent", folder) == "D:\\Work");
    CHECK(exampleValue("sel.file", file) == "report.txt");
    CHECK(exampleTest("sel.file.len == 10", file) == true);
    CHECK(exampleValue("sel.dir", exampleFor(Context::Drive, {})) == "D:\\");
    CHECK(exampleValue("sel.dir.name", exampleFor(Context::Drive, {})) == "D:\\");
    // sel.type: a file is 1, a folder 3.
    CHECK(exampleTest("(sel.count==1 and (sel.type==3 or sel.type==5)) or sel.count>1", file) == false);
    CHECK(exampleTest("(sel.count==1 and (sel.type==3 or sel.type==5)) or sel.count>1", folder) == true);

    // path.exists: the example's own paths; a list entry saved later is not there yet;
    // any other path (a program's) is unknown, so its row stays.
    CHECK(exampleTest("path.exists(sel.dir)", file) == true);
    CHECK(exampleTest("path.exists(ip[toint(do[0])])", file) == false);
    CHECK_FALSE(exampleTest("path.exists(sys.prog + '\\7-Zip\\7z.exe')", file).has_value());
    CHECK_FALSE(exampleTest("path.exists('C:\\Tools\\tool.exe')", file).has_value());

    // if(): the branch not taken may be unknown.
    CHECK(exampleValue("'Rename' + if(sel.count>1, ' @sel.count Selected') + '...'", file) == "Rename...");
    CHECK_FALSE(exampleValue("if(sys.is11, 'a', 'b')", file).has_value());
    // @ in a string: read in place.
    CHECK(exampleValue("'Create shortcut@if(sel.count>1, 's')'", file) == "Create shortcut");
    CHECK(exampleValue("'Copy (@sel.count) item@if(sel.count>1, 's')'", file) == "Copy (1) item");
    CHECK(exampleValue("'Open @sel.file.title.'", file) == "Open report.");
    CHECK(exampleValue("'@(sel.count + 1) more'", file) == "2 more");
    CHECK_FALSE(exampleValue("'OS: @sys.ver'", file).has_value());

    // str.fmt: one pass, a value with {0} in it stays.
    CHECK(exampleValue("str.fmt('{0}', '{0}')", file) == "{0}");
    CHECK(exampleValue("str.fmt('{0}-{1}-{0}', 'a{0}', 'b')", file) == "a{0}-b-a{0}");

    // The snippet's own variables.
    auto ex = file;
    ex.snippetVars["menu_db_name"] = "'Dropbox'";
    ex.snippetVars["ni"] = "0";
    ex.snippetVars["loop"] = "loop";
    ex.snippetVars["off"] = "false";
    CHECK(exampleValue("menu_db_name", ex) == "Dropbox");
    CHECK(exampleTest("ni>0", ex) == false);
    CHECK(exampleTest("!off", ex) == true);
    CHECK_FALSE(exampleValue("loop", ex).has_value());
}

TEST_CASE("snippet preview: items for a menu of Windows' own go in it")
{
    const char *tree = R"({
  "ok": true, "root": "C:\\s\\root.nss", "context": "file", "sample": "C:\\s\\Sample.txt",
  "tree": {"shell":[
    {"kind":"item","title":"Send to Executable","state":"text","source":"","file":"C:\\s.nss","line":1,"column":1,"vis":"","conditions":[],"dynamic":false,"menu":"Se&nd to","moved":false,"items":[]},
    {"kind":"item","title":"Up","state":"text","source":"","file":"C:\\s.nss","line":2,"column":1,"vis":"","conditions":[],"dynamic":false,"menu":"&View","moved":false,"items":[]},
    {"kind":"item","title":"Mine","state":"text","source":"","file":"C:\\s.nss","line":3,"column":1,"vis":"","conditions":[],"dynamic":false,"items":[]}],
  "system":[{"kind":"menu","title":"Se&nd to","items":[]}], "rules":[]}})";
    const auto doc = nss::NssDocument::fromText("item(menu=title.send_to title='Send to Executable' pos='top')\n"
                                                "item(menu=title.view title='Up')\n"
                                                "item(title='Mine')\n");
    auto t = menumap::parseTree(tree);
    REQUIRE(t);
    auto items = itemsOf(t->shell, L"C:\\s.nss", doc, {});
    REQUIRE(items.size() == 3);
    CHECK(items[0].host == "Se&nd to");
    CHECK(items[2].host.empty());

    // No View on a file: that one is left out.
    auto file = items;
    dropMissingHosts(file, Context::File, t->system);
    REQUIRE(file.size() == 2);
    auto c = compose(Context::File, file, identity);
    // Open, Copy, Mine, Send to (with the snippet's item), Properties.
    std::vector<std::string> labels;
    for (const auto &r : c.main.rows)
        if (!r.separator)
            labels.push_back(r.label);
    CHECK(labels == std::vector<std::string>{ "Open", "Copy", "Mine", "Send to", "Properties" });
    REQUIRE(c.open >= 0);
    const auto &host = c.main.rows[size_t(c.open)];
    CHECK(host.label == "Send to");
    CHECK(host.ghost);
    REQUIRE(host.sub >= 0);
    // pos=top: before Windows' own items there (faded).
    const auto &sendTo = c.subs[size_t(host.sub)].rows;
    REQUIRE(sendTo.size() == 5);
    CHECK(sendTo[0].label == "Send to Executable");
    CHECK(sendTo[0].own);
    CHECK(sendTo[0].groupFirst);
    CHECK(sendTo[0].groupLast);
    CHECK(sendTo[1].separator);
    CHECK(sendTo[2].label == "Compressed (zipped) folder");
    CHECK(sendTo[2].ghost);
    CHECK_FALSE(sendTo[2].own);
    CHECK(c.own == 1);

    // The background's View row is in the frame: it gets the item.
    auto back = items;
    dropMissingHosts(back, Context::Back, {});
    REQUIRE(back.size() == 2);
    auto b = compose(Context::Back, back, identity);
    REQUIRE(b.open >= 0);
    CHECK(b.main.rows[size_t(b.open)].label == "View");
    // Windows' own View items first, then the snippet's.
    const auto &view = b.subs[size_t(b.main.rows[size_t(b.open)].sub)].rows;
    REQUIRE(view.size() == 5);
    CHECK(view[0].label == "Large icons");
    CHECK(view[0].ghost);
    CHECK(view[3].separator);
    CHECK(view[4].label == "Up");
    CHECK(view[4].own);

    auto bar = items;
    dropMissingHosts(bar, Context::Taskbar, {});
    CHECK(bar.size() == 1);
}

TEST_CASE("snippet preview: a long menu is cut around its open row")
{
    const Metrics m{ 4, 4, 28, 9 };
    Item menu;
    menu.kind = "menu";
    menu.title = "Copy path";
    for (int i = 0; i < 20; ++i) {
        Item it;
        it.kind = "item";
        it.title = "Form " + std::to_string(i);
        menu.children.push_back(it);
    }
    Item plain;
    plain.kind = "item";
    plain.title = "Plain";
    auto c = compose(Context::File, { plain, menu }, identity);
    REQUIRE(c.open >= 0);
    CHECK(pairHeight(c, m) > 344);
    fitHeight(c, m, 344);
    CHECK(pairHeight(c, m) <= 344);
    const auto &sub = c.subs[size_t(c.main.rows[size_t(c.open)].sub)];
    CHECK(sub.hidden > 0);
    CHECK(sub.hidden + int(sub.rows.size()) == 20);
    CHECK(c.main.hidden == 0);
    CHECK(sub.rows.front().label == "Form 0");

    // A main menu taller than the room: rows after the open one go first.
    std::vector<Item> many;
    for (int i = 0; i < 16; ++i) {
        Item it;
        it.kind = "item";
        it.title = "Row " + std::to_string(i);
        many.push_back(it);
    }
    many.insert(many.begin() + 3, menu);
    auto d = compose(Context::File, many, identity);
    fitHeight(d, m, 344);
    CHECK(heightOf(d.main, m) <= 344);
    CHECK(d.main.hidden > 0);
    REQUIRE(d.open >= 0);
    CHECK(d.main.rows[size_t(d.open)].label == "Copy path");
}

TEST_CASE("snippet preview: a title not from the selection says so")
{
    const char *tree = R"({
  "ok": true, "root": "C:\\s\\root.nss", "context": "file", "sample": "C:\\s\\Sample.txt",
  "tree": {"shell":[
    {"kind":"item","title":"","state":"dynamic","source":"","file":"C:\\s.nss","line":1,"column":1,"vis":"","conditions":[],"dynamic":true,"items":[]},
    {"kind":"item","title":"","state":"dynamic","source":"","file":"C:\\s.nss","line":2,"column":1,"vis":"","conditions":[],"dynamic":true,"items":[]}],
  "system":[], "rules":[]}})";
    const auto doc = nss::NssDocument::fromText("item(title=sys.ver.name)\n"
                                                "item(title=str.fmt('{0} x', sel.path(1)))\n");
    auto t = menumap::parseTree(tree);
    REQUIRE(t);
    const auto ex = exampleFor(Context::File, {});
    auto items = itemsOf(t->shell, L"C:\\s.nss", doc, {}, &ex);
    REQUIRE(items.size() == 2);
    CHECK(items[0].dynamicTitle);
    CHECK_FALSE(items[0].fromSelection);
    CHECK(items[1].dynamicTitle);
    CHECK(items[1].fromSelection);
    auto c = compose(Context::File, items, identity);
    bool worked = false, named = false;
    for (const auto &r : c.main.rows) {
        worked |= r.label == "Worked out when the menu opens";
        named |= r.label == "Name from the selection";
    }
    CHECK(worked);
    CHECK(named);
}

TEST_CASE("snippet preview: a host menu's title with a bracketed accelerator (Chinese Windows)")
{
    // menu=title.view on Chinese Windows reads "查看(&V)"; the app in Chinese calls the
    // frame's View row "查看".
    const char *tree = R"J({
  "ok": true, "root": "C:\\s\\root.nss", "context": "back", "sample": "",
  "tree": {"shell":[
    {"kind":"item","title":"Up","state":"text","source":"","file":"C:\\s.nss","line":1,"column":1,"vis":"","conditions":[],"dynamic":false,"menu":"查看(&V)","moved":false,"items":[]},
    {"kind":"item","title":"Back","state":"text","source":"","file":"C:\\s.nss","line":2,"column":1,"vis":"","conditions":[],"dynamic":false,"menu":"查看(&V)","moved":false,"items":[]},
    {"kind":"item","title":"Tool","state":"text","source":"","file":"C:\\s.nss","line":3,"column":1,"vis":"","conditions":[],"dynamic":false,"menu":"工具(&T)","moved":false,"items":[]}],
  "system":[{"kind":"menu","title":"查看(&V)","items":[]},{"kind":"menu","title":"工具(&T)","items":[]}], "rules":[]}})J";
    const auto doc = nss::NssDocument::fromText("item(menu=title.view title='Up')\n"
                                                "item(menu='查看(&V)' title='Back')\n"
                                                "item(menu='工具(&T)' title='Tool')\n");
    auto t = menumap::parseTree(tree);
    REQUIRE(t);
    auto items = itemsOf(t->shell, L"C:\\s.nss", doc, {});
    REQUIRE(items.size() == 3);
    CHECK(items[0].hostId == "view");
    CHECK(items[1].hostId.empty());
    dropMissingHosts(items, Context::Back, t->system);
    REQUIRE(items.size() == 3);

    const std::map<std::string, std::string> zh{ { "View", "查看" }, { "Sort by", "排序方式" }, { "Refresh", "刷新" },
                                                 { "Large icons", "大图标" } };
    const Translate zhTr = [&](const char *s) {
        auto it = zh.find(s);
        return it == zh.end() ? std::string(s) : it->second;
    };
    auto c = compose(Context::Back, items, zhTr);
    std::vector<std::string> labels;
    for (const auto &r : c.main.rows)
        if (!r.separator)
            labels.push_back(r.label);
    // One View row with both items; the menu the preview doesn't know keeps its own
    // title, without "(T)".
    CHECK(labels == std::vector<std::string>{ "查看", "排序方式", "刷新", "工具", "Properties" });
    REQUIRE(c.open >= 0);
    const auto &view = c.main.rows[size_t(c.open)];
    CHECK(view.label == "查看");
    std::vector<std::string> sub;
    for (const auto &r : c.subs[size_t(view.sub)].rows)
        if (!r.separator)
            sub.push_back(r.label);
    CHECK(sub == std::vector<std::string>{ "大图标", "Medium icons", "Small icons", "Up", "Back" });
    for (const auto &r : c.main.rows)
        if (r.label == "工具")
            CHECK(r.accel == -1);

    // The app in English on Chinese Windows: title.view still finds View.
    auto en = compose(Context::Back, items, identity);
    REQUIRE(en.open >= 0);
    CHECK(en.main.rows[size_t(en.open)].label == "View");
    size_t mine = 0;
    for (const auto &r : en.subs[size_t(en.main.rows[size_t(en.open)].sub)].rows)
        mine += r.own ? 1 : 0;
    CHECK(mine == 1); // "Back" names it only as Chinese Windows does: a row of its own
    // A tree without Windows' items: title.view is one Windows always has there.
    auto bare = itemsOf(t->shell, L"C:\\s.nss", doc, {});
    dropMissingHosts(bare, Context::Back, {});
    REQUIRE(bare.size() == 1);
    CHECK(bare[0].title == "Up");
}

TEST_CASE("snippet preview: the window is known on the background and the desktop")
{
    const auto back = exampleFor(Context::Back, {});
    const auto desk = exampleFor(Context::Desktop, {});
    const auto file = exampleFor(Context::File, {});
    CHECK(exampleTest("wnd.is_desktop", back) == false);
    CHECK(exampleTest("wnd.is_desktop", desk) == true);
    CHECK(exampleTest("window.name=='CabinetWClass'", back) == true);
    CHECK(exampleTest("window.name=='CabinetWClass'", desk) == false);
    CHECK(exampleTest("wnd.name=='cabinetwclass'", back) == true);
    CHECK(exampleTest("!wnd.is_desktop", back) == true);
    // A file may be on the desktop or in a folder: not known.
    CHECK_FALSE(exampleTest("wnd.is_desktop", file).has_value());
    CHECK_FALSE(exampleTest("window.name=='CabinetWClass'", file).has_value());
    CHECK_FALSE(exampleTest("wnd.is_desktop", exampleFor(Context::Folder, {})).has_value());
    CHECK_FALSE(exampleTest("wnd.is_desktop", exampleFor(Context::Drive, {})).has_value());
}

TEST_CASE("snippet preview: regex.matches against the example's path")
{
    auto folder = exampleFor(Context::Folder, {});
    folder.snippetVars["ext_dir"] =
        "regex.matches(sel, '\\.\\{[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}\\}$')";
    // D:\Work\Projects has no {GUID}: nothing matches, ext_dir[0] is ''.
    CHECK(exampleTest("len(ext_dir[0])>0", folder) == false);
    CHECK(exampleValue("ext_dir[0]", folder) == "");
    CHECK(exampleValue("ext_dir[3]", folder) == "");
    CHECK_FALSE(exampleValue("ext_dir", folder).has_value()); // a list is no text
    CHECK_FALSE(exampleValue("ext_dir + 'x'", folder).has_value());

    auto guid = folder;
    guid.vars["sel"] = "D:\\Work\\Fonts.{BD84B380-8CA2-1069-AB1D-08000B88E8B5}";
    guid.vars["sel.file.name"] = "Fonts.{BD84B380-8CA2-1069-AB1D-08000B88E8B5}";
    CHECK(exampleTest("len(ext_dir[0])>0", guid) == true);
    CHECK(exampleValue("ext_dir[0]", guid) == ".{BD84B380-8CA2-1069-AB1D-08000B88E8B5}");
    CHECK(exampleValue("str.replace(sel.file.name, ext_dir[0], \"\")", guid) == "Fonts");

    const auto file = exampleFor(Context::File, {});
    CHECK(exampleTest("regex.match(sel.path, '.*\\.TXT')", file) == true); // no case
    CHECK(exampleTest("regex.match(sel.path, 'report')", file) == false);  // the whole text
    CHECK(exampleValue("regex.matches(sel.path, 'o.')[1]", file) == "or");
    // A pattern std::regex can't read matches nothing, as in Shell.
    CHECK(exampleTest("regex.match(sel.path, '(')", file) == false);
    CHECK(exampleValue("regex.matches(sel.path, '(')[0]", file) == "");
    CHECK_FALSE(exampleValue("regex.matches(sel.path, sys.x)[0]", file).has_value());

    // The bundled Copy path snippet's two rows for a {GUID} folder are left out.
    const char *tree = R"({
  "ok": true, "root": "C:\\s\\root.nss", "context": "folder", "sample": "",
  "tree": {"shell":[
    {"kind":"menu","title":"Paths","state":"text","source":"","file":"C:\\s.nss","line":1,"column":1,"vis":"","conditions":[],"dynamic":false,
     "items":[{"kind":"item","title":"","state":"dynamic","source":"","file":"C:\\s.nss","line":3,"column":2,"vis":"","conditions":["where"],"dynamic":true,"items":[]},
              {"kind":"item","title":"","state":"dynamic","source":"","file":"C:\\s.nss","line":4,"column":2,"vis":"","conditions":["where"],"dynamic":true,"items":[]},
              {"kind":"item","title":"","state":"dynamic","source":"","file":"C:\\s.nss","line":5,"column":2,"vis":"","conditions":[],"dynamic":true,"items":[]}]}],
  "system":[], "rules":[]}})";
    const char *text = "menu(title='Paths') {\n"
                       "\t$ext_dir = regex.matches(sel, '\\.\\{[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}\\}$')\n"
                       "\titem(where=len(ext_dir[0])>0 title=str.replace(sel.file.name, ext_dir[0], \"\"))\n"
                       "\titem(where=len(ext_dir[0])>0 title=ext_dir[0])\n"
                       "\titem(title=sel.path)\n"
                       "}\n";
    const auto doc = nss::NssDocument::fromText(text);
    auto t = menumap::parseTree(tree);
    REQUIRE(t);
    ImageDefs own;
    collectImageDefs(text, own);
    auto ex = exampleFor(Context::Folder, {});
    ex.snippetVars = own.vars;
    auto items = itemsOf(t->shell, L"C:\\s.nss", doc, {}, &ex);
    REQUIRE(items.size() == 1);
    REQUIRE(items[0].children.size() == 1);
    CHECK(items[0].children[0].title == "D:\\Work\\Projects");
}

TEST_CASE("snippet preview: \"+N more\" counts only the snippet's rows")
{
    const Metrics m{ 4, 4, 28, 9 };
    std::vector<Item> many;
    for (int i = 0; i < 14; ++i) {
        Item it;
        it.kind = "item";
        it.title = "Row " + std::to_string(i);
        many.push_back(it);
    }
    auto c = compose(Context::File, many, identity);
    const auto rows = [](const Menu &menu, bool own) {
        int n = 0;
        for (const auto &r : menu.rows)
            n += !r.separator && r.own == own ? 1 : 0;
        return n;
    };
    CHECK(rows(c.main, false) == 3); // Open, Copy, Properties
    fitHeight(c, m, 344);
    CHECK(heightOf(c.main, m) <= 344);
    // Windows' rows go first and aren't counted.
    CHECK(rows(c.main, false) == 0);
    CHECK(c.main.hidden == 14 - rows(c.main, true));
    CHECK(c.main.hidden > 0);

    // Room for all of the snippet's rows once Windows' are gone: nothing to count.
    auto d = compose(Context::File, std::vector<Item>(many.begin(), many.begin() + 10), identity);
    fitHeight(d, m, 4 + 4 + 10 * 28 + 9);
    CHECK(rows(d.main, true) == 10);
    CHECK(d.main.hidden == 0);

    // In Send to: Windows' items there go first, then the snippet's from the bottom.
    std::vector<Item> sendTo;
    for (int i = 0; i < 8; ++i) {
        Item it;
        it.kind = "item";
        it.title = "Target " + std::to_string(i);
        it.host = "Se&nd to";
        it.hostId = "send_to";
        sendTo.push_back(it);
    }
    auto e = compose(Context::File, sendTo, identity);
    REQUIRE(e.open >= 0);
    const auto &sub = e.subs[size_t(e.main.rows[size_t(e.open)].sub)];
    CHECK(rows(sub, false) == 3);
    // Send to is the fourth row (Open, Copy, a separator): its submenu has room for six.
    fitHeight(e, m, 4 + 4 + 6 * 28 + 2 * 28 + 9);
    const auto &cut = e.subs[size_t(e.main.rows[size_t(e.open)].sub)];
    CHECK(rows(cut, false) == 0);
    CHECK(cut.hidden == 8 - rows(cut, true));
    CHECK(cut.hidden == 2);
    CHECK_FALSE(cut.rows.front().separator);
}

TEST_CASE("snippet preview: keys a snippet's modify() gives Windows' own items")
{
    const auto doc = nss::NssDocument::fromText(
        "modify(where=this.title==title.copy type='*' keys='Ctrl+C')\n"
        "modify(where=this.title == title.properties & sel.count>0 keys='Alt+Enter')\n"
        "modify(where=this.title==title.large_icons in=str.replace(title.view, '&', '') keys='Ctrl+Shift+2')\n"
        "modify(where=this.title==title.open keys=@x)\n"
        "item(title='Up' menu=title.view)\n");
    const auto keys = frameKeysOf(doc);
    CHECK(keys.size() == 3);
    CHECK(keys.at("copy") == "Ctrl+C");
    CHECK(keys.at("properties") == "Alt+Enter");
    CHECK(keys.at("large_icons") == "Ctrl+Shift+2");

    Item mine;
    mine.kind = "item";
    mine.title = "Mine";
    auto c = compose(Context::File, { mine }, identity, keys);
    std::map<std::string, std::string> shown;
    for (const auto &r : c.main.rows)
        if (r.ghost)
            shown[r.label] = r.keys;
    CHECK(shown["Copy"] == "Ctrl+C");
    CHECK(shown["Open"] == "");
    CHECK(shown["Properties"] == "Alt+Enter");

    Item up;
    up.kind = "item";
    up.title = "Up";
    up.host = "&View";
    up.hostId = "view";
    auto b = compose(Context::Back, { up }, identity, keys);
    REQUIRE(b.open >= 0);
    const auto &view = b.subs[size_t(b.main.rows[size_t(b.open)].sub)].rows;
    REQUIRE_FALSE(view.empty());
    CHECK(view[0].label == "Large icons");
    CHECK(view[0].keys == "Ctrl+Shift+2");
}
