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
