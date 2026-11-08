#include "doctest.h"

#include "MapRules.h"
#include "MenuMap.h"
#include "PendingChanges.h"

#include "nss/NssDocument.h"
#include "nss/Text.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace menumap;

namespace {

// What `shell.exe -config:tree` prints (trimmed): one menu of the config with an
// item in it, and Windows' Copy as path (moved by a rule), Send to with one item,
// and an item of 7-Zip.
const char *kTreeJson = R"({
  "ok": true,
  "root": "C:\\cfg\\shell.nss",
  "context": "file",
  "sample": "C:\\tmp\\sample.txt",
  "errors": [],
  "warnings": [],
  "tree": {"shell":[{"kind":"menu","title":"File &manage","state":"text","source":"loc.file_manage","file":"C:\\cfg\\imports\\file-manage.nss","line":3,"column":1,"vis":"","conditions":["type"],"dynamic":false,
     "items":[{"kind":"item","title":"Copy path","state":"text","source":"","file":"C:\\cfg\\imports\\file-manage.nss","line":5,"column":2,"vis":"hidden","conditions":[],"dynamic":false,"items":[]},
              {"kind":"separator","title":"","state":"none","source":"","file":"C:\\cfg\\imports\\file-manage.nss","line":6,"column":2,"vis":"","conditions":[],"dynamic":false,"items":[]}]}],
   "system":[{"kind":"item","title":"Copy &as path","name":"Copy as path","path":"","uid":3621442399,"id":"copy_as_path","verb":"copyaspath","disabled":false,"owner_drawn":false,"source":"windows","program":"","module":"","clsid":"","handler":"","rules":[0],"maybe":[],"hidden":false,"maybe_hidden":false,"moved":"File manage","renamed":"","items":[]},
     {"kind":"separator","title":"","name":"","path":"","uid":0,"id":"","verb":"","disabled":false,"owner_drawn":false,"source":"","program":"","module":"","clsid":"","handler":"","rules":[],"maybe":[],"hidden":false,"maybe_hidden":false,"moved":"","renamed":"","items":[]},
     {"kind":"menu","title":"Se&nd to","name":"Send to","path":"","uid":1,"id":"send_to","verb":"","disabled":false,"owner_drawn":false,"source":"windows","program":"","module":"","clsid":"","handler":"","rules":[],"maybe":[1],"hidden":false,"maybe_hidden":true,"moved":"","renamed":"",
      "items":[{"kind":"item","title":"Mail recipient","name":"Mail recipient","path":"send to","uid":2,"id":"","verb":"","disabled":false,"owner_drawn":false,"source":"windows","program":"","module":"","clsid":"","handler":"","rules":[],"maybe":[],"hidden":false,"maybe_hidden":false,"moved":"","renamed":"","items":[]}]},
     {"kind":"item","title":"Add to \"sample.7z\"","name":"Add to \"sample.7z\"","path":"7-zip","uid":5,"id":"","verb":"SevenZipCompressTo7z","disabled":false,"owner_drawn":false,"source":"program","program":"7-Zip","module":"C:\\Program Files\\7-Zip\\7-zip.dll","clsid":"{23170F69-40C1-278A-1000-000100020000}","handler":"7-Zip Shell Extension","rules":[],"maybe":[],"hidden":false,"maybe_hidden":false,"moved":"","renamed":"","items":[]}],
   "rules":[{"index":0,"file":"C:\\cfg\\imports\\modify.nss","line":17,"column":1,"applies":true,"clsid":false,"find":"","find_dynamic":false,"in":"","where":"item","vis":"","menu":"File manage","menu_dynamic":false,"title":"","title_dynamic":false,"pos":false,"sep":false,"image":false},
            {"index":1,"file":"C:\\cfg\\imports\\settings-app-map.nss","line":2,"column":1,"applies":true,"clsid":false,"find":"","find_dynamic":false,"in":"","where":"selection","vis":"hidden","menu":"","menu_dynamic":false,"title":"","title_dynamic":false,"pos":false,"sep":false,"image":false}],
   "system_error":""}
})";

struct TempDir
{
    fs::path path;
    TempDir()
    {
        static std::atomic<int> n{ 0 };
        path = fs::temp_directory_path() /
               (L"settings-menumap-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++n));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void writeFile(const fs::path &p, std::string_view bytes)
{
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), std::streamsize(bytes.size()));
}

} // namespace

TEST_CASE("menu map: the -config:tree JSON")
{
    std::string error;
    auto t = parseTree(kTreeJson, &error);
    REQUIRE(t);
    CHECK(t->ok);
    CHECK(t->context == "file");
    REQUIRE(t->shell.size() == 1);
    const auto &menu = t->shell[0];
    CHECK(menu.origin == Origin::Shell);
    CHECK(menu.kind == "menu");
    CHECK(menu.rawTitle == "File &manage");
    CHECK(menu.title == "File manage");
    CHECK(menu.titleSource == "loc.file_manage");
    CHECK(menu.file == fs::path(L"C:\\cfg\\imports\\file-manage.nss"));
    CHECK(menu.line == 3);
    CHECK(menu.conditions == std::vector<std::string>{ "type" });
    REQUIRE(menu.children.size() == 2);
    CHECK(menu.children[0].vis == "hidden");
    CHECK(menu.children[1].kind == "separator");

    REQUIRE(t->system.size() == 4);
    const auto &cap = t->system[0];
    CHECK(cap.origin == Origin::System);
    CHECK(cap.title == "Copy as path");
    CHECK(cap.id == "copy_as_path");
    CHECK(cap.rules == std::vector<int>{ 0 });
    CHECK(cap.moved == "File manage");
    CHECK(t->system[2].maybeHidden);
    CHECK(t->system[2].children.size() == 1);
    CHECK(t->system[2].children[0].path == "send to");
    CHECK(t->system[3].program == "7-Zip");

    REQUIRE(t->rules.size() == 2);
    CHECK(t->rules[0].menu == "File manage");
    CHECK(t->rules[0].line == 17);
    CHECK(isMapFile(t->rules[1].file));
    CHECK_FALSE(isMapFile(t->rules[0].file));

    // A config that doesn't load: no tree, the errors.
    auto bad = parseTree(R"({"ok":false,"errors":[{"file":"C:\\cfg\\shell.nss","line":4,"column":2,"message":"syntax error"}],"tree":null})");
    REQUIRE(bad);
    CHECK_FALSE(bad->ok);
    REQUIRE(bad->errors.size() == 1);
    CHECK(bad->errors[0] == "shell.nss:4: syntax error");

    CHECK_FALSE(parseTree("not json"));
    CHECK_FALSE(parseTree(R"({"ok":true})"));
}

TEST_CASE("menu map: the main menu in the order it shows")
{
    // shell.exe gives the order: Windows' Open, your Terminal, Windows' Properties,
    // then your Power (pos=bottom). Cleaning isn't in this menu, so it isn't in it.
    const char *json = R"({"ok":true,"errors":[],"tree":{"shell":[
        {"kind":"item","title":"Power","state":"text","file":"C:\\cfg\\a.nss","line":1,"column":1,"shows":"yes","items":[]},
        {"kind":"item","title":"Terminal","state":"text","file":"C:\\cfg\\a.nss","line":2,"column":1,"shows":"maybe","items":[]},
        {"kind":"item","title":"Cleaning","state":"text","file":"C:\\cfg\\a.nss","line":3,"column":1,"shows":"no","items":[]}],
      "system":[{"kind":"item","title":"&Open","name":"Open","items":[]},{"kind":"item","title":"P&roperties","name":"Properties","items":[]}],
      "rules":[],"system_error":"",
      "order":[{"from":"system","index":0},{"from":"shell","index":1},{"from":"system","index":1},{"from":"shell","index":0},
               {"from":"shell","index":2},{"from":"shell","index":9},{"from":"system","index":0}]}})";
    auto t = parseTree(json);
    REQUIRE(t);
    CHECK(t->hasOrder);
    auto titles = [](const std::vector<MenuRef> &menu) {
        std::vector<std::string> out;
        for (const auto &r : menu)
            out.push_back(r.node->title);
        return out;
    };
    auto menu = mergedMenu(*t);
    REQUIRE(menu);
    // Past the end and twice over are skipped; one not in this menu is left out.
    CHECK(titles(*menu) == std::vector<std::string>{ "Open", "Terminal", "Properties", "Power" });
    CHECK((*menu)[1].index == 1);
    CHECK((*menu)[2].index == 1);

    // Items the order doesn't name (one typed by its title on the page) go at the end.
    addUnlisted(*t, { "Scan with Microsoft Defender..." });
    menu = mergedMenu(*t);
    REQUIRE(menu);
    REQUIRE(menu->size() == 5);
    CHECK(menu->back().node->unlisted);
    CHECK(menu->back().index == 2);
}

TEST_CASE("menu map: which of your items are in this menu")
{
    // A newer shell.exe says, per item, whether it shows for the sample.
    const char *json = R"({"ok":true,"errors":[],"tree":{"shell":[
        {"kind":"item","title":"Power","state":"text","file":"C:\\cfg\\a.nss","line":1,"column":1,"vis":"","conditions":["where"],
         "shows":"yes","shows_sample":false,"shows_why":"","items":[]},
        {"kind":"menu","title":"Cleaning","state":"text","file":"C:\\cfg\\a.nss","line":2,"column":1,"vis":"","conditions":["where"],
         "shows":"no","shows_sample":true,"shows_why":"where","items":[
            {"kind":"item","title":"Empty","state":"text","file":"C:\\cfg\\a.nss","line":3,"column":2,"vis":"","conditions":[],
             "shows":"no","shows_sample":true,"shows_why":"menu","items":[]}]},
        {"kind":"item","title":"Develop","state":"text","file":"C:\\cfg\\a.nss","line":5,"column":1,"vis":"dynamic","conditions":["vis"],
         "shows":"maybe","shows_sample":false,"shows_why":"vis","items":[]},
        {"kind":"item","title":"Odd","state":"text","file":"C:\\cfg\\a.nss","line":6,"column":1,"vis":"","conditions":[],
         "shows":"perhaps","items":[]}],
      "system":[],"rules":[],"system_error":""}})";
    auto t = parseTree(json);
    REQUIRE(t);
    REQUIRE(t->shell.size() == 4);
    const auto &power = t->shell[0], &cleaning = t->shell[1], &develop = t->shell[2], &odd = t->shell[3];
    CHECK(power.shows == "yes");
    CHECK_FALSE(notInMenu(power));
    // where= that shell.exe could work out: no "Shows only sometimes".
    CHECK_FALSE(showsSometimes(power));

    CHECK(cleaning.shows == "no");
    CHECK(cleaning.showsSample);
    CHECK(cleaning.showsWhy == "where");
    CHECK(notInMenu(cleaning));
    CHECK_FALSE(showsSometimes(cleaning));
    REQUIRE(cleaning.children.size() == 1);
    CHECK(notInMenu(cleaning.children[0]));
    CHECK(cleaning.children[0].showsWhy == "menu");

    CHECK(develop.shows == "maybe");
    CHECK_FALSE(notInMenu(develop));
    CHECK(showsSometimes(develop));

    // A value it doesn't know: as from an older shell.exe.
    CHECK(odd.shows.empty());
    CHECK_FALSE(notInMenu(odd));
    CHECK_FALSE(showsSometimes(odd));

    // An older shell.exe has no "shows": every item is in the menu, and a where=,
    // find= or vis= that isn't a constant means "only sometimes", as before.
    auto old = parseTree(kTreeJson);
    REQUIRE(old);
    const auto &menu = old->shell[0];
    CHECK(menu.shows.empty());
    CHECK_FALSE(notInMenu(menu));
    CHECK_FALSE(showsSometimes(menu)); // conditions: type only
    Node n = menu;
    n.conditions = { "where" };
    CHECK(showsSometimes(n));
    n.conditions.clear();
    n.vis = "dynamic";
    CHECK(showsSometimes(n));
    // No order from an older shell.exe: your items and Windows' stay apart.
    CHECK_FALSE(old->hasOrder);
    CHECK_FALSE(mergedMenu(*old));

    // Windows' items are never "not in this menu": they come from the menu itself.
    Node w;
    w.origin = Origin::System;
    w.shows = "no";
    CHECK_FALSE(notInMenu(w));
}

TEST_CASE("menu map: rules for hide, rename and move")
{
    auto t = parseTree(kTreeJson);
    REQUIRE(t);
    const auto &cap = t->system[0];
    const auto &mail = t->system[2].children[0];
    const auto &sevenZip = t->system[3];

    // Known to Shell: by id, language-independent.
    CHECK(hideRule(cap) == "remove(where=this.id==id.copy_as_path)");
    CHECK(renameRule(cap, "  Path  ") == "modify(where=this.id==id.copy_as_path title=\"Path\")");
    CHECK(moveRule(cap, "File manage") == "modify(where=this.id==id.copy_as_path menu=\"File manage\")");

    // Not known: by its exact title, in its submenu.
    CHECK(hideRule(mail) == "remove(find=\"\\\"Mail recipient\\\"\" in=\"send to\")");
    CHECK(moveRule(mail, "More options") == "modify(find=\"\\\"Mail recipient\\\"\" in=\"send to\" menu=\"More options\")");

    // A title find= can't hold, separators and Shell's own items: no rule.
    CHECK(hideRule(sevenZip).empty());
    CHECK(hideRule(t->system[1]).empty());
    CHECK(hideRule(t->shell[0]).empty());
    CHECK(renameRule(cap, "").empty());
    CHECK(renameRule(cap, "two\nlines").empty());

    // Each rule parses as one remove/modify declaration.
    for (const auto &rule : { hideRule(cap), renameRule(cap, "Path \"x\""), moveRule(mail, "More options") }) {
        auto doc = nss::NssDocument::fromText(rule);
        CHECK(doc.diagnostics().empty());
        REQUIRE(doc.nodes().size() == 1);
        CHECK(doc.nodes()[0].kind == nss::NodeKind::MenuDecl);
    }
}

TEST_CASE("menu map: the rules file and the pending rule changes")
{
    maprules::Rules existing{ "remove(where=this.id==id.cast_to_device)", "// remove(where=this.id==id.share)",
                              "modify(where=this.id==id.print menu=\"More options\")" };
    auto text = maprules::generate(existing);
    CHECK(text == std::string(maprules::header) +
                      "\r\nremove(where=this.id==id.cast_to_device)\r\n// remove(where=this.id==id.share)\r\n"
                      "modify(where=this.id==id.print menu=\"More options\")\r\n");
    auto doc = nss::NssDocument::fromText(text);
    CHECK(doc.diagnostics().empty());
    CHECK(maprules::read(doc) == existing);
    CHECK(maprules::read(nss::NssDocument::fromText(maprules::generate({}))).empty());

    PendingChanges p;
    // Show "Cast to device" again; hide "Share" (turned off on Context Menus: on again
    // in place); hide "Copy as path" (new).
    p.removeRule("remove(where=this.id==id.cast_to_device)", "show Cast to device");
    p.addRule("remove(where=this.id==id.share)", "hide Share");
    p.addRule("remove(where=this.id==id.copy_as_path)", "hide Copy as path");
    CHECK(p.count() == 3);
    CHECK_FALSE(p.empty());
    auto after = p.applyRules(existing);
    CHECK(after == maprules::Rules{ "remove(where=this.id==id.share)", "modify(where=this.id==id.print menu=\"More options\")",
                                    "remove(where=this.id==id.copy_as_path)" });
    CHECK(p.describe().find("hide Share") != std::string::npos);

    // Undoing a pending change drops it; removing a rule turned off removes it too.
    p.removeRule("remove(where=this.id==id.copy_as_path)", "show Copy as path");
    CHECK(p.findRule("remove(where=this.id==id.copy_as_path)") == nullptr);
    p.removeRule("// remove(where=this.id==id.share)", "x"); // cancels the pending add
    CHECK(p.findRule("remove(where=this.id==id.share)") == nullptr);
    p.removeRule("remove(where=this.id==id.share)", "remove it");
    CHECK(p.applyRules(existing) == maprules::Rules{ "modify(where=this.id==id.print menu=\"More options\")" });
    p.discard();
    CHECK(p.empty());
}

TEST_CASE("menu map: saving rules writes settings-app-map.nss and imports it first")
{
    TempDir dir;
    auto root = dir.path / L"shell.nss";
    const std::string rootText = "settings { priority = 1 }\r\n"
                                 "import lang 'imports/lang/en.nss'\r\n"
                                 "import 'imports/theme.nss'\r\n"
                                 "import 'imports/modify.nss'\r\n";
    writeFile(root, rootText);

    PendingChanges p;
    p.addRule("remove(where=this.id==id.send_to)", "hide Send to");
    auto plan = planSave(root, p);
    REQUIRE(plan.ok);
    CHECK(plan.rootChanged);
    REQUIRE(plan.writes.size() == 2); // the map file and the root; settings-app.nss untouched
    CHECK(plan.writes[0].path == plan.mapPath);
    CHECK(plan.writes[0].text == maprules::generate({ "remove(where=this.id==id.send_to)" }));
    CHECK(plan.writes[1].text == "settings { priority = 1 }\r\n"
                                 "import lang 'imports/lang/en.nss'\r\n"
                                 "import 'imports/settings-app-map.nss'\r\n"
                                 "import 'imports/theme.nss'\r\n"
                                 "import 'imports/modify.nss'\r\n");

    // Once it is there, the root is left alone, and showing the item again empties
    // the file (the import stays).
    writeFile(root, plan.writes[1].text);
    writeFile(plan.mapPath, plan.writes[0].text);
    PendingChanges q;
    q.removeRule("remove(where=this.id==id.send_to)", "show Send to");
    auto plan2 = planSave(root, q);
    REQUIRE(plan2.ok);
    CHECK_FALSE(plan2.rootChanged);
    REQUIRE(plan2.writes.size() == 1);
    CHECK(plan2.writes[0].text == maprules::generate({}));

    // Theme changes alone don't touch the map file.
    PendingChanges r;
    CHECK(r.set("theme.item.radius", "2").empty());
    auto plan3 = planSave(root, r);
    REQUIRE(plan3.ok);
    for (const auto &w : plan3.writes)
        CHECK(w.path != plan3.mapPath);
}

TEST_CASE("menu map: one declaration, found by line and replaced exactly")
{
    const std::string text = "// tools\r\n"
                             "menu(title='Tools' image=icon.settings)\r\n"
                             "{\r\n"
                             "\titem(title='Notepad' cmd='notepad.exe')   // the editor\r\n"
                             "\titem(title=\"Calc\"\r\n"
                             "\t     cmd='calc.exe')\r\n"
                             "\tsep\r\n"
                             "}\r\n"
                             "modify(find='\"Share\"' vis=vis.remove)\r\n";
    auto doc = nss::NssDocument::fromText(text);
    REQUIRE(doc.diagnostics().empty());

    auto notepad = findBlock(doc, 4, 2);
    REQUIRE(notepad);
    CHECK(notepad->text == "\titem(title='Notepad' cmd='notepad.exe')");
    auto calc = findBlock(doc, 5);
    REQUIRE(calc);
    CHECK(calc->text == "\titem(title=\"Calc\"\r\n\t     cmd='calc.exe')");
    auto menu = findBlock(doc, 2);
    REQUIRE(menu);
    CHECK(menu->text.rfind("menu(title='Tools'", 0) == 0);
    CHECK(menu->text.ends_with("\tsep\r\n}"));
    CHECK(findBlock(doc, 9));
    CHECK_FALSE(findBlock(doc, 1)); // a comment
    CHECK_FALSE(findBlock(doc, 3)); // inside the menu, no declaration starts there

    // Replace one item: everything else stays byte for byte.
    auto edited = doc;
    std::string error;
    REQUIRE(replaceBlock(edited, *notepad, "\titem(title='Notepad++' cmd='notepad++.exe')", &error));
    std::string expected = text;
    expected.replace(expected.find("\titem(title='Notepad'"), notepad->text.size(),
                     "\titem(title='Notepad++' cmd='notepad++.exe')");
    CHECK(edited.text() == expected);

    // Replacing with the same text gives the same file.
    auto same = doc;
    REQUIRE(replaceBlock(same, *calc, calc->text));
    CHECK(same.text() == text);

    // Rejected: not one declaration, a syntax error, or a stale block.
    auto rejected = doc;
    CHECK_FALSE(replaceBlock(rejected, *notepad, "item(title='A') item(title='B')", &error));
    CHECK_FALSE(replaceBlock(rejected, *notepad, "$x = 1", &error));
    CHECK_FALSE(replaceBlock(rejected, *notepad, "item(title='A'", &error));
    CHECK(rejected.text() == text);
    auto stale = doc;
    stale.replace({ 0, 0 }, "// more\r\n");
    CHECK_FALSE(replaceBlock(stale, *notepad, "\titem(title='X')", &error));
}

TEST_CASE("menu map: hiding and renaming one of Shell's items")
{
    const std::string text = "item(title='Notepad' cmd='notepad.exe')\r\n"
                             "item(title='Paint' vis=key.shift() cmd='mspaint.exe')\r\n"
                             "item(image=icon.copy cmd='x.exe')\r\n";
    auto doc = nss::NssDocument::fromText(text);
    std::string error;

    CHECK_FALSE(shellItemHidden(doc, 1, 1));
    REQUIRE(setShellItemHidden(doc, 1, 1, true, &error));
    CHECK(doc.text().rfind("item(vis=vis.remove title='Notepad' cmd='notepad.exe')\r\n", 0) == 0);
    CHECK(shellItemHidden(doc, 1, 1));
    REQUIRE(setShellItemHidden(doc, 1, 1, true)); // already hidden: no change
    CHECK(doc.text().rfind("item(vis=vis.remove title='Notepad'", 0) == 0);
    REQUIRE(setShellItemHidden(doc, 1, 1, false));
    CHECK(doc.text() == text); // shown again: exactly as before

    // Its own vis= is left to Edit code.
    CHECK_FALSE(setShellItemHidden(doc, 2, 1, true, &error));
    CHECK(doc.text() == text);

    REQUIRE(setShellItemTitle(doc, 1, 1, "Editor \"plain\"", &error));
    CHECK(doc.text().rfind("item(title=\"Editor \\\"plain\\\"\" cmd='notepad.exe')", 0) == 0);
    REQUIRE(setShellItemTitle(doc, 3, 1, "Copy it"));
    CHECK(doc.text().find("item(title=\"Copy it\" image=icon.copy cmd='x.exe')") != std::string::npos);
    CHECK(doc.diagnostics().empty());

    CHECK(conditionText(doc, 2, 1) == "vis=key.shift()");
}

TEST_CASE("menu map: icons, as the Icon field writes them")
{
    // A file typed as it is goes in quotes; an .svg through image.svgf().
    CHECK(imageValue("  C:\\Icons\\a.ico ") == "\"C:\\\\Icons\\\\a.ico\"");
    CHECK(imageValue("C:\\Tools\\app.exe,0") == "\"C:\\\\Tools\\\\app.exe,0\"");
    CHECK(imageValue("shell32.dll,-16") == "\"shell32.dll,-16\"");
    CHECK(imageValue("%SystemRoot%\\notepad.exe") == "\"%SystemRoot%\\\\notepad.exe\"");
    CHECK(imageValue("C:\\Icons\\logo.svg") == "image.svgf(\"C:\\\\Icons\\\\logo.svg\")");
    // @: an expression, so single quotes.
    CHECK(imageValue("@sys.dir\\shell32.dll,4") == "'@sys.dir\\shell32.dll,4'");
    // Anything else is an expression already.
    CHECK(imageValue("image.mdl(\\uE8C8)") == "image.mdl(\\uE8C8)");
    CHECK(imageValue("\\uE8C8") == "\\uE8C8");
    CHECK(imageValue("icon.copy") == "icon.copy");
    CHECK(imageValue("'C:\\x.ico'") == "'C:\\x.ico'");
    CHECK(imageValue("  ").empty());

    // The field shows the file of a plain quoted path, and gives the same value back.
    for (const char *typed : { "C:\\Icons\\a.ico", "C:\\Tools\\app.exe,0", "C:\\Icons\\logo.svg", "image.mdl(\\uE8C8)",
                               "icon.copy" }) {
        CAPTURE(typed);
        CHECK(imageText(imageValue(typed)) == typed);
    }
    CHECK(imageText("'@sys.prog\\7-Zip\\7zFM.exe'") == "'@sys.prog\\7-Zip\\7zFM.exe'"); // @ inside: as written
    CHECK(imageText("[\\uE194,#f00]") == "[\\uE194,#f00]");
    CHECK(imageText("\"C:\\\\a.svg\"") == "\"C:\\\\a.svg\""); // a plain .svg string isn't image.svgf()

    // One of your items: image= on its line, replaced, taken out (icon= counts too).
    const std::string text = "item(title='Notepad' cmd='notepad.exe')\r\n"
                             "item(title='Paint' icon=\\uE8C8 cmd='mspaint.exe')\r\n";
    auto doc = nss::NssDocument::fromText(text);
    REQUIRE(setShellItemProp(doc, 1, 1, "image", imageValue("C:\\Icons\\a.ico")));
    CHECK(doc.text().rfind("item(title='Notepad' cmd='notepad.exe' image=\"C:\\\\Icons\\\\a.ico\")\r\n", 0) == 0);
    CHECK(shellItemProp(doc, 2, 1, "image") == std::optional<std::string>("\\uE8C8"));
    REQUIRE(setShellItemProp(doc, 2, 1, "image", "image.mdl(\\uE8C9)"));
    CHECK(doc.text().find("item(title='Paint' icon=image.mdl(\\uE8C9) cmd='mspaint.exe')") != std::string::npos);
    REQUIRE(setShellItemProp(doc, 1, 1, "image", ""));
    REQUIRE(setShellItemProp(doc, 2, 1, "image", "\\uE8C8"));
    CHECK(doc.text() == text);
    std::string error;
    CHECK_FALSE(setShellItemProp(doc, 1, 1, "image", "\"unclosed", &error));
    CHECK(doc.text() == text);

    // A Windows item: a rule made here, and its image= read back.
    auto t = parseTree(kTreeJson);
    REQUIRE(t);
    const auto &cap = t->system[0];
    auto rule = imageRule(cap, imageValue("C:\\Tools\\app.exe,0"));
    CHECK(rule == "modify(where=this.id==id.copy_as_path image=\"C:\\\\Tools\\\\app.exe,0\")");
    CHECK(ruleImage(rule) == std::optional<std::string>("\"C:\\\\Tools\\\\app.exe,0\""));
    CHECK(imageRule(cap, "null") == "modify(where=this.id==id.copy_as_path image=null)");
    CHECK(ruleImage("modify(where=this.id==id.copy_as_path title=\"Path\")") == std::nullopt);
    CHECK(imageRule(cap, "").empty());
    CHECK(imageRule(t->system[3], "icon.copy").empty()); // a title find= can't hold
    for (const auto &r : { rule, imageRule(cap, "image.mdl(\\uE8C8)") }) {
        auto d = nss::NssDocument::fromText(r);
        CHECK(d.diagnostics().empty());
    }
}

TEST_CASE("menu map: hand edit survives an app save")
{
    TempDir dir;
    auto root = dir.path / L"shell.nss";
    writeFile(root, "import 'imports/settings-app-map.nss'\r\nimport 'imports/modify.nss'\r\n");
    auto file = dir.path / L"imports" / L"settings-app-map.nss";

    // The page wrote two rules.
    PendingChanges p;
    p.addRule("remove(where=this.id==id.cast_to_device)", "hide Cast to device");
    p.addRule("remove(where=this.id==id.share)", "hide Share");
    auto plan = planSave(root, p);
    REQUIRE(plan.ok);
    REQUIRE(plan.writes.size() == 1);
    std::string text = plan.writes[0].text;

    // The user edits it in the Config Editor: a comment, a blank line, an
    // indented statement the page doesn't know, and a rule the page knows,
    // turned off by hand ("Share" shows again).
    auto at = text.find("remove(where=this.id==id.share)");
    REQUIRE(at != std::string::npos);
    text.replace(at, 0, "// ");
    text += "\r\n// my own rules\r\n  modify(find='Print' sep=sep.top)\r\n";
    writeFile(file, text);

    // The page hides "Copy as path" and shows "Cast to device" again.
    PendingChanges q;
    q.addRule("remove(where=this.id==id.copy_as_path)", "hide Copy as path");
    q.removeRule("remove(where=this.id==id.cast_to_device)", "show Cast to device");
    auto plan2 = planSave(root, q);
    REQUIRE_MESSAGE(plan2.ok, plan2.error);
    REQUIRE(plan2.writes.size() == 1);
    // The rule turned off by hand stays off: it's read back as it is.
    CHECK(std::find(plan2.rulesBefore.begin(), plan2.rulesBefore.end(), "// remove(where=this.id==id.share)") !=
          plan2.rulesBefore.end());
    CHECK(plan2.writes[0].text == std::string(maprules::header) +
                                      "\r\n// remove(where=this.id==id.share)\r\n"
                                      "\r\n// my own rules\r\n  modify(find='Print' sep=sep.top)\r\n"
                                      "remove(where=this.id==id.copy_as_path)\r\n");
    CHECK(nss::NssDocument::fromText(plan2.writes[0].text).diagnostics().empty());

    // A file from an older version gets today's header, nothing else changes.
    writeFile(file, "// generated by Nilesoft Shell Pro (Menu map), do not edit\r\n// mine\r\n");
    auto plan3 = planSave(root, q);
    REQUIRE(plan3.ok);
    REQUIRE(plan3.writes.size() == 1);
    CHECK(plan3.writes[0].text == std::string(maprules::header) + "\r\n// mine\r\nremove(where=this.id==id.copy_as_path)\r\n");
}

TEST_CASE("menu map: a hand edit that doesn't parse is left alone")
{
    TempDir dir;
    auto root = dir.path / L"shell.nss";
    writeFile(root, "import 'imports/settings-app-map.nss'\r\n");
    auto file = dir.path / L"imports" / L"settings-app-map.nss";
    const std::string broken = std::string(maprules::header) + "\r\nremove(where=this.id==id.share\r\n";
    writeFile(file, broken);
    REQUIRE_FALSE(nss::NssDocument::fromText(broken).diagnostics().empty());

    PendingChanges p;
    p.addRule("remove(where=this.id==id.copy_as_path)", "hide Copy as path");
    auto plan = planSave(root, p);
    CHECK_FALSE(plan.ok);
    CHECK(plan.writes.empty());
    CHECK(plan.error.find("settings-app-map.nss") != std::string::npos);
    CHECK(plan.error.find("Config Editor") != std::string::npos);
}

TEST_CASE("menu map: where an item of the menu goes")
{
    auto shellNode = [](const char *json) {
        auto t = parseTree(std::string(R"({"ok":true,"errors":[],"tree":{"shell":[)") + json + R"(],"system":[],"rules":[],"system_error":""}})");
        REQUIRE(t);
        REQUIRE(t->shell.size() == 1);
        return t->shell[0];
    };
    CHECK(placingOf(shellNode(R"({"kind":"item","title":"A","state":"text","shows":"yes","items":[]})")) == Placing::Menu);
    CHECK(placingOf(shellNode(R"({"kind":"item","title":"A","state":"text","shows":"maybe","items":[]})")) == Placing::Menu);
    CHECK(placingOf(shellNode(R"({"kind":"item","title":"A","state":"text","shows":"no","shows_why":"where","items":[]})")) == Placing::Out);
    CHECK(placingOf(shellNode(R"({"kind":"item","title":"A","state":"text","shows":"yes","vis":"hidden","items":[]})")) == Placing::Hidden);
    // menu= names a menu this tab doesn't have; "/" is the main menu; moved here is fine.
    CHECK(placingOf(shellNode(R"({"kind":"item","title":"A","state":"text","shows":"yes","menu":"Nowhere","items":[]})")) == Placing::Out);
    CHECK(placingOf(shellNode(R"({"kind":"menu","title":"A","state":"text","shows":"yes","menu":"/","items":[]})")) == Placing::Menu);
    CHECK(placingOf(shellNode(R"({"kind":"item","title":"A","state":"text","shows":"yes","menu":"More","moved":true,"items":[]})")) == Placing::Menu);

    Node w;
    w.origin = Origin::System;
    CHECK(placingOf(w) == Placing::Menu);
    w.moved = "Send to";
    CHECK(placingOf(w) == Placing::Moved);
    w.moved = "/";
    CHECK(placingOf(w) == Placing::Menu); // kept in the main menu
    w.hidden = true;
    CHECK(placingOf(w) == Placing::Hidden);
    w.hidden = false;
    w.unlisted = true;
    CHECK(placingOf(w) == Placing::Out);
}

TEST_CASE("menu map: menu paths, separators and titles of a menu")
{
    CHECK(menuPathKey("/Pin/Unpin/") == "pin/unpin");
    CHECK(menuPathKey("File &manage / Copy path") == "file manage/copy path");
    CHECK(menuPathKey(" More options ") == menuPathKey("more options"));
    CHECK(menuPathKey("/") == "");

    // No separator first or last, never two in a row.
    auto kept = [](std::string_view pattern) {
        std::vector<bool> sep;
        for (char c : pattern)
            sep.push_back(c == '-');
        std::string out;
        for (size_t i : collapseSeparators(sep))
            out += pattern[i];
        return out;
    };
    CHECK(kept("a-b") == "a-b");
    CHECK(kept("-a--b-") == "a-b");
    CHECK(kept("---") == "");
    CHECK(kept("") == "");
    CHECK(kept("ab--c-d--") == "ab-c-d");

    // A title that depends on the selection counts, when shell.exe worked it out.
    auto t = parseTree(R"({"ok":true,"errors":[],"tree":{"shell":[
        {"kind":"menu","title":"","state":"none","shows":"yes","items":[
          {"kind":"item","title":"Virus protection","state":"text","shows":"yes","items":[]},
          {"kind":"separator","title":"","state":"none","shows":"yes","items":[]},
          {"kind":"item","title":"","state":"dynamic","title_sample":"Win&dows 11","shows":"yes","items":[]},
          {"kind":"item","title":"","state":"dynamic","shows":"yes","items":[]},
          {"kind":"item","title":"Third","state":"text","shows":"yes","items":[]},
          {"kind":"item","title":"Fourth","state":"text","shows":"yes","items":[]}]}],
        "system":[],"rules":[],"system_error":""}})");
    REQUIRE(t);
    const auto &menu = t->shell[0];
    CHECK(menu.children[2].titleSample == "Windows 11");
    bool more = false;
    CHECK(childTitles(menu, 3, &more) == std::vector<std::string>{ "Virus protection", "Windows 11", "Third" });
    CHECK(more);
    CHECK(childTitles(menu, 10, &more).size() == 4);
    CHECK_FALSE(more);
}

namespace {

// A This PC like tab: Windows' Open, two items a rule moves into your Pin/Unpin and
// More options menus, Manage and a stray removed by rules, Delete and Properties; your
// Pin/Unpin and More options (empty but for what moves in), a menu with a separator
// first and last, a switched off item, one that names a menu that isn't here, one this
// menu leaves out, and a menu that moves to Windows' Send to.
const char *kThisPcJson = R"({"ok":true,"context":"thispc","errors":[],"tree":{
  "shell":[
    {"kind":"menu","title":"Pin/Unpin","state":"text","file":"C:\\cfg\\a.nss","line":1,"column":1,"shows":"yes","items":[]},
    {"kind":"menu","title":"More options","state":"text","file":"C:\\cfg\\a.nss","line":2,"column":1,"shows":"yes","items":[]},
    {"kind":"menu","title":"Power","state":"text","file":"C:\\cfg\\a.nss","line":3,"column":1,"shows":"yes","pos":"bottom","items":[
      {"kind":"separator","title":"","state":"none","file":"C:\\cfg\\a.nss","line":4,"column":1,"shows":"yes","items":[]},
      {"kind":"item","title":"Restart","state":"text","file":"C:\\cfg\\a.nss","line":5,"column":1,"shows":"yes","items":[]},
      {"kind":"separator","title":"","state":"none","file":"C:\\cfg\\a.nss","line":6,"column":1,"shows":"yes","items":[]},
      {"kind":"separator","title":"","state":"none","file":"C:\\cfg\\a.nss","line":7,"column":1,"shows":"yes","items":[]},
      {"kind":"item","title":"Shut Down","state":"text","file":"C:\\cfg\\a.nss","line":8,"column":1,"shows":"yes","items":[]},
      {"kind":"item","title":"Gone","state":"text","file":"C:\\cfg\\a.nss","line":9,"column":1,"shows":"yes","vis":"hidden","items":[]},
      {"kind":"separator","title":"","state":"none","file":"C:\\cfg\\a.nss","line":10,"column":1,"shows":"yes","items":[]}]},
    {"kind":"item","title":"Off","state":"text","file":"C:\\cfg\\a.nss","line":11,"column":1,"shows":"yes","vis":"hidden","items":[]},
    {"kind":"item","title":"Elsewhere","state":"text","file":"C:\\cfg\\a.nss","line":12,"column":1,"shows":"yes","menu":"Nowhere","items":[]},
    {"kind":"item","title":"Files only","state":"text","file":"C:\\cfg\\a.nss","line":13,"column":1,"shows":"no","shows_why":"where","items":[]},
    {"kind":"item","title":"Hosts","state":"text","file":"C:\\cfg\\a.nss","line":14,"column":1,"shows":"yes","items":[]}],
  "system":[
    {"kind":"item","title":"&Open","name":"Open","items":[]},
    {"kind":"item","title":"Pin to &Quick access","name":"Pin to Quick access","moved":"Pin/Unpin","items":[]},
    {"kind":"item","title":"Mana&ge","name":"Manage","hidden":true,"items":[]},
    {"kind":"separator","title":"","items":[]},
    {"kind":"item","title":"Map &network drive...","name":"Map network drive","moved":"More options","items":[]},
    {"kind":"item","title":"Add to Mediathek","name":"Add to Mediathek","moved":"Nowhere else","items":[]},
    {"kind":"separator","title":"","items":[]},
    {"kind":"menu","title":"Se&nd to","name":"Send to","moved":"More options","items":[
      {"kind":"item","title":"Desktop","name":"Desktop","path":"send to","items":[]},
      {"kind":"item","title":"Mail","name":"Mail","path":"send to","moved":"Hosts","items":[]}]},
    {"kind":"item","title":"&Delete","name":"Delete","items":[]},
    {"kind":"separator","title":"","items":[]},
    {"kind":"item","title":"P&roperties","name":"Properties","items":[]}],
  "rules":[],"system_error":"",
  "order":[{"from":"system","index":0},{"from":"system","index":1},{"from":"system","index":2},{"from":"system","index":3},
           {"from":"system","index":4},{"from":"system","index":5},{"from":"system","index":6},{"from":"system","index":7},
           {"from":"shell","index":0},{"from":"shell","index":1},{"from":"shell","index":3},{"from":"shell","index":4},
           {"from":"shell","index":5},{"from":"shell","index":6},{"from":"system","index":8},{"from":"system","index":9},
           {"from":"system","index":10},{"from":"shell","index":7},{"from":"shell","index":2}]}})";

// "Title" for an entry; separators as "-".
std::string label(const MenuList &list, int i)
{
    const auto &e = list.entries[size_t(i)];
    return !e.node ? "?" : e.node->kind == "separator" ? "-" : e.node->title;
}

std::vector<std::string> kidsOf(const MenuList &list, int i)
{
    std::vector<std::string> out;
    for (int k : list.entries[size_t(i)].kids)
        out.push_back(label(list, k));
    return out;
}

int groupIndex(const MenuList &list, int group)
{
    for (size_t i = 0; i < list.entries.size(); ++i)
        if (!list.entries[i].node && list.entries[i].group == group)
            return int(i);
    return -1;
}

int find(const MenuList &list, int parent, const std::string &title)
{
    for (int k : list.entries[size_t(parent)].kids)
        if (label(list, k) == title)
            return k;
    return -1;
}

} // namespace

TEST_CASE("menu map: the list holds what the menu has, in its order")
{
    auto t = parseTree(kThisPcJson);
    REQUIRE(t);
    auto list = buildMenuList(*t);
    REQUIRE(list);
    const int menu = groupIndex(*list, 3), hidden = groupIndex(*list, 4), out = groupIndex(*list, 2);
    REQUIRE(menu >= 0);
    REQUIRE(hidden >= 0);
    REQUIRE(out >= 0);
    // The three groups come in this order.
    CHECK(menu < hidden);
    CHECK(hidden < out);

    // In this menu: only what shows, nothing moved elsewhere, switched off or left out,
    // and the separators collapsed (the one Manage left behind, the two doubled).
    CHECK(kidsOf(*list, menu) ==
          std::vector<std::string>{ "Open", "-", "Pin/Unpin", "More options", "Hosts", "Delete", "-", "Properties", "Power" });

    // Windows' items moved into your menus are in those menus, after their own items.
    CHECK(kidsOf(*list, find(*list, menu, "Pin/Unpin")) == std::vector<std::string>{ "Pin to Quick access" });
    const int more = find(*list, menu, "More options");
    CHECK(kidsOf(*list, more) == std::vector<std::string>{ "Map network drive...", "Send to" });
    const auto &moved = list->entries[size_t(find(*list, more, "Map network drive..."))];
    CHECK(moved.movedIn);
    CHECK(moved.from.empty()); // from the main menu
    CHECK(moved.group == 1);
    // A Windows menu moved there brings its items, but the one a rule moves out of it is not in it.
    const int sendTo = find(*list, more, "Send to");
    CHECK(kidsOf(*list, sendTo) == std::vector<std::string>{ "Desktop" });
    // ...that one goes where it was sent: Hosts isn't a menu, so it is out of the menu.
    CHECK(find(*list, out, "Mail") >= 0);

    // Power: no separator first, in a row or last; its switched off item is hidden.
    CHECK(kidsOf(*list, find(*list, menu, "Power")) == std::vector<std::string>{ "Restart", "-", "Shut Down" });

    // Hidden: switched off by vis= or removed by a rule, with the menu it was in.
    CHECK(kidsOf(*list, hidden) == std::vector<std::string>{ "Manage", "Off", "Gone" });
    CHECK(list->entries[size_t(find(*list, hidden, "Gone"))].from == "Power");
    CHECK(list->entries[size_t(find(*list, hidden, "Gone"))].group == 4);

    // Not in this menu: where= leaves it out, a menu that isn't here, a menu to nowhere.
    CHECK(find(*list, out, "Files only") >= 0);
    CHECK(find(*list, out, "Elsewhere") >= 0);
    CHECK(find(*list, out, "Add to Mediathek") >= 0);
    CHECK(list->entries[size_t(find(*list, out, "Add to Mediathek"))].group == 2);
    // None of these is in the menu too.
    for (const char *title : { "Files only", "Elsewhere", "Add to Mediathek", "Off", "Manage", "Gone" })
        CHECK(find(*list, menu, title) < 0);
}

TEST_CASE("menu map: titles typed on the page that this menu doesn't have are not in it")
{
    auto t = parseTree(kThisPcJson);
    REQUIRE(t);
    addUnlisted(*t, { "Search Everything..." });
    auto list = buildMenuList(*t);
    REQUIRE(list);
    const int menu = groupIndex(*list, 3), out = groupIndex(*list, 2);
    CHECK(find(*list, menu, "Search Everything") < 0);
    const int typed = find(*list, out, "Search Everything");
    REQUIRE(typed >= 0);
    CHECK(list->entries[size_t(typed)].node->unlisted);
    CHECK(list->entries[size_t(typed)].group == 2);
}

TEST_CASE("menu map: a row stays in its group after a switch until the list is rebuilt")
{
    auto t = parseTree(kThisPcJson);
    REQUIRE(t);
    auto first = buildMenuList(*t);
    REQUIRE(first);
    // Hosts is in the menu; Off is switched off.
    const auto key = [&](const char *title) {
        for (const auto &e : first->entries)
            if (e.node && e.node->title == title)
                return e.key;
        return std::string();
    };
    REQUIRE(first->placed.count(key("Hosts")));
    CHECK(first->placed[key("Hosts")] == 0);
    CHECK(first->placed[key("Off")] == 1);

    // Hosts is switched off and Off on, as the reloaded tree says; pinned they stay.
    t->shell[6].vis = "hidden";
    t->shell[3].vis = "";
    auto regrouped = buildMenuList(*t);
    REQUIRE(regrouped);
    CHECK(find(*regrouped, groupIndex(*regrouped, 4), "Hosts") >= 0);
    CHECK(find(*regrouped, groupIndex(*regrouped, 3), "Off") >= 0);

    auto pinned = buildMenuList(*t, first->placed);
    REQUIRE(pinned);
    CHECK(find(*pinned, groupIndex(*pinned, 3), "Hosts") >= 0);
    CHECK(find(*pinned, groupIndex(*pinned, 4), "Off") >= 0);
    CHECK(find(*pinned, groupIndex(*pinned, 4), "Hosts") < 0);
    // The row is where it was in the menu's order.
    auto titles = kidsOf(*pinned, groupIndex(*pinned, 3));
    CHECK(std::find(titles.begin(), titles.end(), "Hosts") < std::find(titles.begin(), titles.end(), "Delete"));
}

TEST_CASE("menu map: Windows' items moved into a Windows menu are listed in it")
{
    auto t = parseTree(R"({"ok":true,"errors":[],"tree":{"shell":[],"system":[
        {"kind":"item","title":"&Share","name":"Share","moved":"Send to","items":[]},
        {"kind":"menu","title":"Se&nd to","name":"Send to","items":[
           {"kind":"item","title":"Desktop","name":"Desktop","path":"send to","items":[]}]}],
        "rules":[],"system_error":"","order":[{"from":"system","index":0},{"from":"system","index":1}]}})");
    REQUIRE(t);
    auto list = buildMenuList(*t);
    REQUIRE(list);
    const int menu = groupIndex(*list, 3);
    CHECK(kidsOf(*list, menu) == std::vector<std::string>{ "Send to" });
    CHECK(kidsOf(*list, find(*list, menu, "Send to")) == std::vector<std::string>{ "Desktop", "Share" });
    // Without an order (an older shell.exe) there is no list: the page keeps its two groups.
    t->hasOrder = false;
    CHECK_FALSE(buildMenuList(*t));
}
