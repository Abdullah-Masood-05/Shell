// Moving and ordering Shell's own items on the Menu map page (MenuMap.h): menu=
// and pos= written onto an item's declaration and taken out again, a declaration
// moved up or down in its file, and the order pos= gives a menu
// (src/dll/src/Include/MenuPlacement.h, as `shell -config:tree` and the menu use it).

#include "doctest.h"

#include "MenuMap.h"

#include "nss/NssDocument.h"

#include "../../dll/src/Include/MenuPlacement.h"

#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <windows.h>

namespace fs = std::filesystem;

using namespace menumap;

namespace {

namespace Placement = Nilesoft::Shell::Placement;

// One item of a menu for Placement::arrange: its title and its pos=.
struct It
{
    std::string title;
    Placement::Spec spec;
    std::string pattern; // indexof(): the whole title it names
};

std::vector<std::string> arrange(const std::vector<It> &items)
{
    std::vector<const It *> list;
    for (const auto &i : items)
        list.push_back(&i);
    auto out = Placement::arrange(
        list, [](const It *i) -> const Placement::Spec & { return i->spec; },
        [](const std::vector<const It *> &menu, const It *i) -> std::ptrdiff_t {
            for (size_t k = 0; k < menu.size(); ++k)
                if (menu[k]->title == i->pattern)
                    return std::ptrdiff_t(k);
            return -1;
        },
        [](const It *a, const It *b) {
            return a->pattern == b->pattern && a->spec.offset == b->spec.offset && a->spec.def == b->spec.def;
        });
    std::vector<std::string> titles;
    for (auto *i : out)
        titles.push_back(i->title);
    return titles;
}

It plain(std::string t) { return It{ std::move(t), {}, {} }; }
It at(std::string t, Placement::Kind k, int index = 0) { return It{ std::move(t), { k, index, 0, -1 }, {} }; }
It rel(std::string t, std::string anchor, int offset) { return It{ std::move(t), { Placement::Kind::IndexOf, 0, offset, -1 }, std::move(anchor) }; }

using Titles = std::vector<std::string>;
using K = Placement::Kind;

Sibling own(std::string title, int line, bool fixed = false, std::string file = "C:\\cfg\\a.nss")
{
    Sibling s;
    s.own = true;
    s.titled = true;
    s.step = { std::move(title), "" };
    s.file = file;
    s.line = line;
    s.column = 2;
    s.fixed = fixed;
    return s;
}

Sibling separator(int line)
{
    Sibling s;
    s.own = true;
    s.separator = true;
    s.file = "C:\\cfg\\a.nss";
    s.line = line;
    s.column = 2;
    return s;
}

} // namespace

TEST_CASE("menu move: the order pos= gives a menu")
{
    // No pos=: the order they are written in.
    CHECK(arrange({ plain("a"), plain("b"), plain("c") }) == Titles{ "a", "b", "c" });
    // top first, then the rest, then bottom; middle at the middle.
    CHECK(arrange({ plain("a"), at("B", K::Bottom), at("T", K::Top), plain("b") }) == Titles{ "T", "a", "b", "B" });
    CHECK(arrange({ plain("a"), plain("b"), plain("c"), plain("d"), at("M", K::Middle) }) ==
          Titles{ "a", "b", "M", "c", "d" });
    // pos=N: at index N, the end when past it.
    CHECK(arrange({ plain("a"), plain("b"), at("N", K::Index, 1) }) == Titles{ "a", "N", "b" });
    CHECK(arrange({ plain("a"), at("N", K::Index, 9) }) == Titles{ "a", "N" });
    // indexof(): right before (0) or right after (1) the item it names; the end without it.
    CHECK(arrange({ plain("Restart"), plain("Lock"), rel("Empty", "Restart", 1) }) == Titles{ "Restart", "Empty", "Lock" });
    CHECK(arrange({ rel("Empty", "Lock", 0), plain("Restart"), plain("Lock") }) == Titles{ "Restart", "Empty", "Lock" });
    CHECK(arrange({ plain("a"), rel("X", "missing", 0), plain("b") }) == Titles{ "a", "b", "X" });
    // Two with the same indexof() keep their order in the file.
    CHECK(arrange({ plain("a"), plain("b"), rel("X", "a", 1), rel("Y", "a", 1) }) == Titles{ "a", "X", "Y", "b" });
    // An anchor placed by pos= itself counts once it is in.
    CHECK(arrange({ plain("a"), at("T", K::Top), rel("X", "T", 1) }) == Titles{ "T", "X", "a" });
}

TEST_CASE("menu move: menu= and pos= written onto an item and taken out again")
{
    const std::string text = "menu(title='System')\r\n"
                             "{\r\n"
                             "\titem(title='Restart' cmd='x')\r\n"
                             "\titem(title=loc.tools_empty_recycle_bin image=image.mdl(\\uE74D)\r\n"
                             "\t\tcmd='y')   // asks first\r\n"
                             "\titem(title='Lock' pos=\"top\" cmd='z')\r\n"
                             "\titem()\r\n"
                             "}\r\n";
    auto doc = nss::NssDocument::fromText(text);
    REQUIRE(doc.diagnostics().empty());
    std::string error;

    CHECK_FALSE(shellItemProp(doc, 4, 2, "menu"));
    REQUIRE(setShellItemProp(doc, 4, 2, "menu", "loc.develop", &error));
    // After its last property, on that line; nothing else changes.
    std::string expected = text;
    expected.replace(expected.find("cmd='y')"), 8, "cmd='y' menu=loc.develop)");
    CHECK(doc.text() == expected);
    CHECK(shellItemProp(doc, 4, 2, "menu") == "loc.develop");
    // Again: the value is replaced, not added twice.
    REQUIRE(setShellItemProp(doc, 4, 2, "menu", "\"/\""));
    CHECK(shellItemProp(doc, 4, 2, "menu") == "\"/\"");
    // Taken out: exactly the file as it was.
    REQUIRE(setShellItemProp(doc, 4, 2, "menu", ""));
    CHECK(doc.text() == text);

    // pos= the item already has: its value is replaced, then taken out with its space.
    CHECK(shellItemProp(doc, 6, 2, "pos") == "\"top\"");
    REQUIRE(setShellItemProp(doc, 6, 2, "pos", "indexof(quote(loc.x), 1)"));
    CHECK(doc.text().find("\titem(title='Lock' pos=indexof(quote(loc.x), 1) cmd='z')\r\n") != std::string::npos);
    REQUIRE(setShellItemProp(doc, 6, 2, "pos", ""));
    CHECK(doc.text().find("\titem(title='Lock' cmd='z')\r\n") != std::string::npos);

    // position= is the same property.
    auto other = nss::NssDocument::fromText("item(title='A' position=1)\n");
    CHECK(shellItemProp(other, 1, 1, "pos") == "1");
    REQUIRE(setShellItemProp(other, 1, 1, "pos", "\"bottom\""));
    CHECK(other.text() == "item(title='A' position=\"bottom\")\n");

    // An item without properties.
    REQUIRE(setShellItemProp(doc, 7, 2, "pos", "\"top\""));
    CHECK(doc.text().find("\titem(pos=\"top\")\r\n") != std::string::npos);
    REQUIRE(setShellItemProp(doc, 7, 2, "pos", ""));
    CHECK(doc.text().find("\titem()\r\n") != std::string::npos);

    // Rejected: a value that wouldn't parse, or no declaration there.
    auto before = doc.text();
    CHECK_FALSE(setShellItemProp(doc, 3, 2, "menu", "'open", &error));
    CHECK_FALSE(setShellItemProp(doc, 2, 1, "menu", "'x'", &error));
    CHECK(doc.text() == before);
    CHECK(doc.diagnostics().empty());
}

TEST_CASE("menu move: the values written for menu= and pos=")
{
    CHECK(menuValue({}) == "\"/\"");
    CHECK(menuValue({ { "System", "loc.tools_system" } }) == "loc.tools_system");
    CHECK(menuValue({ { "File &manage", "" } }) == "\"File &manage\"");
    CHECK(menuValue({ { "Tools", "" }, { "More", "" } }) == "\"Tools/More\"");
    CHECK(menuValue({ { "&Develop", "loc.develop" }, { "Tools", "" } }) == "loc.develop + \"/Tools\"");
    CHECK(menuValue({ { "Tools", "" }, { "System", "loc.tools_system" } }) == "\"Tools/\" + loc.tools_system");
    CHECK(menuValue({ { "A", "loc.a" }, { "B", "loc.b" } }) == "loc.a + \"/\" + loc.b");

    CHECK(posValue({ "Lock", "loc.tools_lock" }, false) == "indexof(quote(loc.tools_lock), 0)");
    CHECK(posValue({ "Lock", "loc.tools_lock" }, true) == "indexof(quote(loc.tools_lock), 1)");
    CHECK(posValue({ "Copy &path...", "" }, true) == "indexof(\"\\\"Copy path\\\"\", 1)");
    CHECK(posValue({ "a|b", "" }, true).empty());
    CHECK(posValue({ "", "" }, true).empty());
}

TEST_CASE("menu move: planning a move up or down")
{
    // Two items one after the other in the same file: the declaration moves.
    std::vector<Sibling> menu = { own("Restart", 3), own("Empty", 5), own("Lock", 8) };
    auto up = planReorder(menu, 1, true);
    CHECK(up.possible);
    CHECK(up.text);
    CHECK(up.anchor == 0);
    CHECK_FALSE(up.after);
    CHECK(up.pos == "indexof(\"\\\"Restart\\\"\", 0)"); // the way when it can't
    auto down = planReorder(menu, 1, false);
    CHECK(down.text);
    CHECK(down.anchor == 2);
    CHECK(down.after);
    CHECK(down.pos == "indexof(\"\\\"Lock\\\"\", 1)");
    // At the ends: nothing to do.
    CHECK_FALSE(planReorder(menu, 0, true).possible);
    CHECK_FALSE(planReorder(menu, 2, false).possible);

    // In another file, or placed by pos=/menu=: pos= only.
    std::vector<Sibling> mixed = { own("A", 1, false, "C:\\cfg\\b.nss"), own("B", 4), own("C", 6, true) };
    auto m = planReorder(mixed, 1, true);
    CHECK(m.possible);
    CHECK_FALSE(m.text);
    CHECK(m.pos == "indexof(\"\\\"A\\\"\", 0)");
    CHECK_FALSE(planReorder(mixed, 1, false).text);

    // Past a separator: right after the item above it (right before the one below).
    std::vector<Sibling> sep = { own("A", 1, true), separator(2), own("B", 3), separator(4) };
    auto s = planReorder(sep, 2, true);
    CHECK(s.text); // a separator of the same block: its line can move too
    CHECK(s.pos == "indexof(\"\\\"A\\\"\", 1)");
    CHECK(planReorder(sep, 2, false).pos == "\"bottom\"");
    std::vector<Sibling> top = { separator(1), own("B", 2) };
    CHECK(planReorder(top, 1, true).pos == "\"top\"");

    // Windows' items and separators don't move this way.
    std::vector<Sibling> windows = { own("A", 1), Sibling{} };
    CHECK_FALSE(planReorder(windows, 1, true).possible);
    CHECK_FALSE(planReorder(sep, 1, true).possible);
}

TEST_CASE("menu move: the places offered are next to items that show")
{
    auto hidden = [](Sibling s) {
        s.shown = false;
        return s;
    };
    Sibling windows;            // one of Windows' items with a title
    windows.titled = true;
    windows.step = { "&Open", "" };
    Sibling untitled;           // a title that changes with the selection
    std::vector<Sibling> menu = { own("More options", 1), hidden(own("Terminal", 3)), separator(4), own("Self", 5),
                                  windows, untitled, own("Sometimes", 7) };
    auto places = placesNextTo(menu, 3);
    std::vector<std::string> got;
    for (const auto &p : places)
        got.push_back((p.after ? "After " : "Before ") + p.title + " = " + p.pos);
    CHECK(got == std::vector<std::string>{
                     "Before More options = indexof(\"\\\"More options\\\"\", 0)",
                     "After More options = indexof(\"\\\"More options\\\"\", 1)",
                     "Before Open = indexof(\"\\\"Open\\\"\", 0)",
                     "After Open = indexof(\"\\\"Open\\\"\", 1)",
                     "Before Sometimes = indexof(\"\\\"Sometimes\\\"\", 0)",
                     "After Sometimes = indexof(\"\\\"Sometimes\\\"\", 1)",
                 });
}

TEST_CASE("menu move: up or down steps over hidden items")
{
    auto hidden = [](Sibling s) {
        s.shown = false;
        return s;
    };
    // Hidden ones are left out of the menu: the move goes past the next item that shows,
    // and pos= never names a hidden one (indexof() wouldn't find it).
    std::vector<Sibling> menu = { own("Restart", 3), hidden(own("Terminal", 5)), own("Empty", 7),
                                  hidden(own("Lock", 9)), own("Sleep", 11) };
    auto up = planReorder(menu, 2, true);
    CHECK(up.possible);
    CHECK(up.text);
    CHECK(up.anchor == 0);
    CHECK_FALSE(up.after);
    CHECK(up.pos == "indexof(\"\\\"Restart\\\"\", 0)");
    auto down = planReorder(menu, 2, false);
    CHECK(down.text);
    CHECK(down.anchor == 4);
    CHECK(down.after);
    CHECK(down.pos == "indexof(\"\\\"Sleep\\\"\", 1)");

    // Only hidden items above (or below): already first (last) of what shows.
    std::vector<Sibling> ends = { hidden(own("A", 1)), own("B", 3), hidden(own("C", 5)) };
    CHECK_FALSE(planReorder(ends, 1, true).possible);
    CHECK_FALSE(planReorder(ends, 1, false).possible);

    // Past a separator: the item beyond it that shows, else the top.
    std::vector<Sibling> sep = { own("A", 1, true), hidden(own("H", 2, true)), separator(3), own("B", 4) };
    CHECK(planReorder(sep, 3, true).pos == "indexof(\"\\\"A\\\"\", 1)");
    std::vector<Sibling> top = { hidden(own("H", 1, true)), separator(2), own("B", 3) };
    CHECK(planReorder(top, 2, true).pos == "\"top\"");
}

TEST_CASE("menu move: which of a menu's items show and can be named, as Shell builds it")
{
    auto ownNode = [](const char *title, int line, const char *file = "C:/cfg/a.nss") {
        Node n;
        n.origin = Origin::Shell;
        n.kind = "item";
        n.title = n.rawTitle = title;
        n.state = "text";
        n.file = file;
        n.line = line;
        n.column = 2;
        return n;
    };
    auto winNode = [](const char *title, const char *path = "") {
        Node n;
        n.origin = Origin::System;
        n.kind = "item";
        n.title = n.rawTitle = title;
        n.name = itemName(title);
        n.path = path;
        return n;
    };
    const auto self = ownNode("Self", 10);

    // Your items: hidden (vis=hidden, or switched off here), sent elsewhere by menu=,
    // or brought here by another one's menu=.
    auto plain = ownNode("Plain", 3);
    CHECK(siblingOf(plain, &self, true, "", "").shown);
    CHECK_FALSE(siblingOf(plain, &self, false, "", "").shown);
    auto away = ownNode("Away", 4);
    away.menu = "Terminal";
    CHECK_FALSE(siblingOf(away, &self, true, "", "").shown);
    auto broughtHere = away;
    broughtHere.relocated = true;
    CHECK(siblingOf(broughtHere, &self, true, "", "").shown);
    // Left out of this menu for the sample (its where=): "Place in this menu" doesn't
    // offer a place next to it. One that shows only sometimes still counts.
    auto elsewhere = ownNode("Cleaning", 6);
    elsewhere.shows = "no";
    CHECK_FALSE(siblingOf(elsewhere, &self, true, "", "").shown);
    auto sometimes = ownNode("Develop", 7);
    sometimes.shows = "maybe";
    CHECK(siblingOf(sometimes, &self, true, "", "").shown);
    {
        std::vector<Sibling> menu = { siblingOf(plain, &self, true, "", ""), siblingOf(elsewhere, &self, true, "", ""),
                                      siblingOf(self, &self, true, "", ""), siblingOf(sometimes, &self, true, "", "") };
        std::vector<std::string> titles;
        for (const auto &p : placesNextTo(menu, 2))
            titles.push_back(p.title);
        CHECK(titles == std::vector<std::string>{ "Plain", "Plain", "Develop", "Develop" });
    }

    // Placed by its own pos=N or pos=indexof(): only one declared before `self` in
    // the same file is in the menu when `self` is placed.
    auto early = ownNode("Early", 5);
    early.pos = "indexof";
    CHECK(siblingOf(early, &self, true, "", "").titled);
    auto late = ownNode("Late", 12);
    late.pos = "2";
    CHECK_FALSE(siblingOf(late, &self, true, "", "").titled);
    auto otherFile = ownNode("Other", 1, "C:/cfg/b.nss");
    otherFile.pos = "indexof";
    CHECK_FALSE(siblingOf(otherFile, &self, true, "", "").titled);
    late.pos = "bottom"; // top, middle and bottom are placed before any of those
    CHECK(siblingOf(late, &self, true, "", "").titled);

    // Windows' items: hidden by a rule (saved or waiting, as stateOf gives it), moved
    // to another menu, kept in the main menu, or renamed.
    auto term = winNode("Open in Terminal");
    CHECK(siblingOf(term, &self, true, "", "").shown);
    CHECK_FALSE(siblingOf(term, &self, false, "", "").shown);
    CHECK_FALSE(siblingOf(term, &self, true, "Terminal", "").shown);
    CHECK(siblingOf(term, &self, true, "/", "").shown);
    auto sendTo = winNode("Mail recipient", "send to");
    CHECK_FALSE(siblingOf(sendTo, &self, true, "/", "").shown);
    CHECK(siblingOf(sendTo, &self, true, "Send &to", "").shown);
    auto renamed = siblingOf(term, &self, true, "", "Terminal here");
    CHECK(renamed.shown);
    CHECK(posValue(renamed.step, true) == "indexof(\"\\\"Terminal here\\\"\", 1)");

    // The value of a rule waiting to be saved, as moveRule and renameRule write it.
    CHECK(ruleProp(term, moveRule(term, "Terminal"), "menu") == std::optional<std::string>("Terminal"));
    CHECK(ruleProp(term, moveRule(term, "/"), "menu") == std::optional<std::string>("/"));
    CHECK(ruleProp(term, renameRule(term, "Say \"hi\""), "title") == std::optional<std::string>("Say \"hi\""));
    CHECK_FALSE(ruleProp(term, renameRule(term, "X"), "menu").has_value());
    CHECK_FALSE(ruleProp(winNode("Other"), moveRule(term, "Terminal"), "menu").has_value());
}

TEST_CASE("menu move: a declaration moved up or down in its file")
{
    const std::string text = "menu(title='System')\r\n"
                             "{\r\n"
                             "\titem(title='Restart' cmd='x')\r\n"
                             "\r\n"
                             "\t// Needs elevation\r\n"
                             "\titem(title='Flush DNS' admin=true\r\n"
                             "\t\tcmd='ipconfig')\r\n"
                             "\tseparator\r\n"
                             "\titem(title='Lock' cmd='z')\r\n"
                             "}\r\n"
                             "item(title='Top' cmd='t')";
    auto doc = nss::NssDocument::fromText(text);
    REQUIRE(doc.diagnostics().empty());
    std::string error;
    size_t line = 0;

    // Flush DNS (with its comment) above Restart: only those lines move.
    auto up = doc;
    REQUIRE(moveDeclaration(up, 6, 2, 3, 2, false, &error, &line));
    CHECK(up.text() == "menu(title='System')\r\n"
                       "{\r\n"
                       "\t// Needs elevation\r\n"
                       "\titem(title='Flush DNS' admin=true\r\n"
                       "\t\tcmd='ipconfig')\r\n"
                       "\titem(title='Restart' cmd='x')\r\n"
                       "\r\n"
                       "\tseparator\r\n"
                       "\titem(title='Lock' cmd='z')\r\n"
                       "}\r\n"
                       "item(title='Top' cmd='t')");
    CHECK(line == 4);
    // And back below Restart: the blank line stays where it was left.
    REQUIRE(moveDeclaration(up, 4, 2, 6, 2, true, &error, &line));
    CHECK(line == 5);
    CHECK(up.text() == "menu(title='System')\r\n"
                       "{\r\n"
                       "\titem(title='Restart' cmd='x')\r\n"
                       "\t// Needs elevation\r\n"
                       "\titem(title='Flush DNS' admin=true\r\n"
                       "\t\tcmd='ipconfig')\r\n"
                       "\r\n"
                       "\tseparator\r\n"
                       "\titem(title='Lock' cmd='z')\r\n"
                       "}\r\n"
                       "item(title='Top' cmd='t')");
    CHECK(up.diagnostics().empty());

    // Past a separator, and the last line of a file without a line end.
    auto lock = doc;
    REQUIRE(moveDeclaration(lock, 9, 2, 8, 2, false, &error, &line));
    CHECK(lock.text().find("\titem(title='Lock' cmd='z')\r\n\tseparator\r\n}") != std::string::npos);
    CHECK(line == 8);
    auto last = nss::NssDocument::fromText("item(title='A')\nitem(title='B')");
    REQUIRE(moveDeclaration(last, 2, 1, 1, 1, false, &error, &line));
    CHECK(last.text() == "item(title='B')\nitem(title='A')");
    CHECK(line == 1);
    REQUIRE(moveDeclaration(last, 1, 1, 2, 1, true));
    CHECK(last.text() == "item(title='A')\nitem(title='B')");

    // Not in the same menu of the file, or sharing a line: left alone.
    auto rejected = doc;
    CHECK_FALSE(moveDeclaration(rejected, 11, 1, 3, 2, false, &error));
    auto shared = nss::NssDocument::fromText("item(title='A') item(title='B')\nitem(title='C')\n");
    CHECK_FALSE(moveDeclaration(shared, 2, 1, 1, 1, false, &error));
    CHECK(rejected.text() == text);
    CHECK(shared.text() == "item(title='A') item(title='B')\nitem(title='C')\n");
}

TEST_CASE("menu move: the tree says where menu= and pos= put an item")
{
    const char *json = R"({"ok":true,"errors":[],"tree":{"shell":[
        {"kind":"menu","title":"Tools","state":"text","source":"","file":"C:\\cfg\\shell.nss","line":6,"column":1,"vis":"","conditions":[],"dynamic":false,
         "menu":"","menu_dynamic":false,"moved":false,"moved_from":"","pos":"",
         "items":[{"kind":"item","title":"Empty Recycle Bin","state":"text","source":"loc.tools_empty_recycle_bin","file":"C:\\cfg\\imports\\my-tools.nss","line":44,"column":2,"vis":"","conditions":[],"dynamic":false,
                   "menu":"Tools","menu_dynamic":false,"moved":true,"moved_from":"System","pos":"after:Lock","items":[]}]}],
        "system":[],"rules":[],"system_error":""}})";
    auto tree = parseTree(json);
    REQUIRE(tree);
    REQUIRE(tree->shell.size() == 1);
    REQUIRE(tree->shell[0].children.size() == 1);
    const auto &n = tree->shell[0].children[0];
    CHECK(n.menu == "Tools");
    CHECK(n.relocated);
    CHECK(n.movedFrom == "System");
    CHECK(n.pos == "after:Lock");
    CHECK_FALSE(tree->shell[0].relocated);
}

// --- The main menu as a target, and items the map can't list ---

TEST_CASE("menu move: the main menu for Windows' items")
{
    CHECK(isRootPath("/"));
    CHECK(isRootPath(" / "));
    CHECK(isRootPath("//"));
    CHECK_FALSE(isRootPath(""));
    CHECK_FALSE(isRootPath("More options"));
    CHECK_FALSE(isRootPath("/Tools/"));

    Node create;
    create.origin = Origin::System;
    create.kind = "item";
    create.title = create.name = "Create shortcut";
    create.id = "create_shortcut";
    CHECK(moveRule(create, "/") == "modify(where=this.id==id.create_shortcut menu=\"/\")");
    Node zip;
    zip.origin = Origin::System;
    zip.kind = "item";
    zip.title = zip.name = "Compressed (zipped) folder";
    zip.path = "Send to";
    CHECK(moveRule(zip, "/") == "modify(find=\"\\\"Compressed (zipped) folder\\\"\" in=\"Send to\" menu=\"/\")");
}

TEST_CASE("menu move: items the map can't list")
{
    CHECK(itemName("Scan with Microsoft &Defender...") == "Scan with Microsoft Defender");
    CHECK(itemName("  Troubleshoot compatibilit&y ") == "Troubleshoot compatibility");
    CHECK(itemName("...") == "");

    const char *json = R"({"ok":true,"errors":[],"tree":{"shell":[],
        "system":[{"kind":"item","title":"&Open","name":"Open","path":"","uid":0,"id":"","verb":"open","disabled":false,
                   "owner_drawn":false,"source":"windows","program":"","module":"","clsid":"","handler":"",
                   "rules":[1],"maybe":[],"hidden":false,"maybe_hidden":false,"moved":"","renamed":"Run","items":[]}],
        "rules":[
          {"index":0,"file":"C:\\cfg\\imports\\settings-app-map.nss","line":3,"column":1,"applies":true,"clsid":false,
           "find":"\"Scan with Microsoft Defender\"","find_dynamic":false,"in":"","where":"","vis":"hidden","menu":"",
           "menu_dynamic":false,"title":"","title_dynamic":false,"pos":false,"sep":false,"image":false},
          {"index":1,"file":"C:\\cfg\\imports\\settings-app-map.nss","line":4,"column":1,"applies":true,"clsid":false,
           "find":"\"Open\"","find_dynamic":false,"in":"","where":"","vis":"","menu":"","menu_dynamic":false,
           "title":"Run","title_dynamic":false,"pos":false,"sep":false,"image":false},
          {"index":2,"file":"C:\\cfg\\imports\\modify.nss","line":9,"column":1,"applies":true,"clsid":false,
           "find":"\"Elsewhere\"","find_dynamic":false,"in":"","where":"","vis":"hidden","menu":"","menu_dynamic":false,
           "title":"","title_dynamic":false,"pos":false,"sep":false,"image":false}],
        "quiet":[{"clsid":"{09A47860-11B0-4DA5-AFA5-26D86198A780}","name":"EPP","module":"C:\\Program Files\\Windows Defender\\shellext.dll",
                  "program":"Microsoft Defender","windows":false},
                 {"clsid":"{x}","name":"Sharing","module":"C:\\Windows\\System32\\ntshrui.dll","program":"","windows":true}],
        "system_error":""}})";
    auto tree = parseTree(json);
    REQUIRE(tree);
    // Windows' own quiet handlers are left out.
    CHECK(tree->quietPrograms == std::vector<std::string>{ "EPP (Windows Defender\\shellext.dll)" });

    // A title typed on the page; one already listed (any case) is not added again.
    // A rule made here for an item the map doesn't list: that item shows, hidden.
    addUnlisted(*tree, { "Troubleshoot compatibilit&y...", "open" });
    REQUIRE(tree->system.size() == 3);
    const auto &typed = tree->system[1];
    CHECK(typed.unlisted);
    CHECK(typed.title == "Troubleshoot compatibility");
    CHECK(typed.rules.empty());
    CHECK(matchProps(typed) == "find=\"\\\"Troubleshoot compatibility\\\"\"");
    CHECK(hideRule(typed) == "remove(find=\"\\\"Troubleshoot compatibility\\\"\")");
    const auto &defender = tree->system[2];
    CHECK(defender.unlisted);
    CHECK(defender.title == "Scan with Microsoft Defender");
    CHECK(defender.hidden);
    CHECK(defender.rules == std::vector<int>{ 0 });
    // Rules in other files don't add items.
    for (const auto &n : tree->system)
        CHECK(n.title != "Elsewhere");
}

// --- `shell.exe -config:tree` on scratch configs (the real shell.exe from src/bin) ---

namespace {

fs::path binDir() { return fs::path(SETTINGS_SOURCE_BIN).lexically_normal(); }
bool haveShell() { return fs::is_regular_file(binDir() / L"shell.exe") && fs::is_regular_file(binDir() / L"shell.dll"); }

struct Scratch
{
    fs::path path;
    Scratch()
    {
        static int n = 0;
        path = fs::temp_directory_path() /
               (L"settings-menumove-tests-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++n));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~Scratch()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    void write(const fs::path &rel, const std::string &text) const
    {
        fs::create_directories((path / rel).parent_path());
        std::ofstream(path / rel, std::ios::binary) << text;
    }
};

std::optional<Tree> runTree(const fs::path &root, const std::wstring &context, const fs::path &sample = {})
{
    auto out = root.parent_path() / L"tree.json";
    std::wstring cmd = L"\"" + (binDir() / L"shell.exe").wstring() + L"\" -silent -config:tree \"" + root.wstring() +
                       L"\" -context:" + context + (sample.empty() ? L"" : L" \"-sample:" + sample.wstring() + L"\"") +
                       L" \"-out:" + out.wstring() + L"\"";
    STARTUPINFOW si{ sizeof si };
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return std::nullopt;
    WaitForSingleObject(pi.hProcess, 60000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    std::ifstream in(out, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return parseTree(text);
}

const Node *findNode(const std::vector<Node> &list, const std::string &title)
{
    for (const auto &n : list) {
        if (n.title == title)
            return &n;
        if (auto *c = findNode(n.children, title))
            return c;
    }
    return nullptr;
}

} // namespace

TEST_CASE("menu move: -config:tree places your items by menu= and pos= (real shell.exe)")
{
    if (!haveShell()) {
        MESSAGE("shell.exe/shell.dll not in src/bin; skipped");
        return;
    }
    Scratch s;
    s.write("shell.nss", "settings { priority=1 }\r\n"
                         "menu(type='*' title='Tools')\r\n"
                         "{\r\n"
                         "\titem(title='Alpha' cmd='a')\r\n"
                         "\tseparator\r\n"
                         "\titem(title='Beta' cmd='b' menu='/')\r\n"
                         "\titem(title='Gamma' cmd='g')\r\n"
                         "}\r\n"
                         "menu(type='*' title='System')\r\n"
                         "{\r\n"
                         "\titem(title='Restart' cmd='r')\r\n"
                         "\titem(title='Lock' cmd='l')\r\n"
                         "\titem(title='Empty Recycle Bin' cmd='e' menu='Tools' pos=indexof('\"Alpha\"', 1))\r\n"
                         "\titem(title='Flush' cmd='f' pos='top')\r\n"
                         "}\r\n");
    auto tree = runTree(s.path / "shell.nss", L"taskbar");
    REQUIRE(tree);
    REQUIRE(tree->ok);

    // Beta: menu='/' takes it out of Tools into the main menu, at its end.
    REQUIRE(tree->shell.size() == 3);
    CHECK(tree->shell[2].title == "Beta");
    CHECK(tree->shell[2].relocated);
    CHECK(tree->shell[2].movedFrom == "Tools");
    CHECK(tree->shell[2].menu == "/");

    // Empty Recycle Bin: from System into Tools, right after Alpha.
    const auto &tools = tree->shell[0].children;
    std::vector<std::string> titles;
    for (const auto &n : tools)
        titles.push_back(n.kind == "separator" ? "-" : n.title);
    CHECK(titles == std::vector<std::string>{ "Alpha", "Empty Recycle Bin", "-", "Gamma" });
    const auto *bin = findNode(tools, "Empty Recycle Bin");
    REQUIRE(bin);
    CHECK(bin->relocated);
    CHECK(bin->movedFrom == "System");
    CHECK(bin->pos == "after:Alpha");

    // System: Flush (pos=top) first; Empty Recycle Bin no longer there.
    const auto &system = tree->shell[1].children;
    REQUIRE(system.size() == 3);
    CHECK(system[0].title == "Flush");
    CHECK(system[0].pos == "top");
    CHECK(system[1].title == "Restart");
    CHECK(system[2].title == "Lock");
}

TEST_CASE("menu move: menu='/' keeps a Windows item in the main menu (real shell.exe)")
{
    if (!haveShell()) {
        MESSAGE("shell.exe/shell.dll not in src/bin; skipped");
        return;
    }
    Scratch s;
    s.write("sample.txt", "x");
    // The map's rules come first, a shipped kind of move after them (as
    // settings-app-map.nss is imported before modify.nss).
    s.write("imports/settings-app-map.nss",
            "modify(where=this.id==id.create_shortcut menu=\"/\")\r\n"
            "modify(find=\"\\\"Compressed (zipped) folder\\\"\" in=\"Send to\" menu=\"/\")\r\n");
    s.write("imports/modify.nss", "modify(where=this.id(id.create_shortcut, id.copy) pos=1 menu='More')\r\n"
                                  "modify(find='\"Compressed (zipped) folder\"' menu='More')\r\n");
    s.write("shell.nss", "settings { priority=1 }\r\n"
                         "import 'imports/settings-app-map.nss'\r\n"
                         "import 'imports/modify.nss'\r\n"
                         "menu(title='More') { }\r\n");
    auto tree = runTree(s.path / "shell.nss", L"file", s.path / "sample.txt");
    REQUIRE(tree);
    REQUIRE(tree->ok);
    REQUIRE(tree->rules.size() == 4);

    const Node *create = nullptr, *copy = nullptr, *zip = nullptr;
    std::function<void(const std::vector<Node> &)> walk = [&](const std::vector<Node> &list) {
        for (const auto &n : list) {
            if (n.id == "create_shortcut")
                create = &n;
            else if (n.id == "copy")
                copy = &n;
            else if (n.name == "Compressed (zipped) folder")
                zip = &n;
            walk(n.children);
        }
    };
    walk(tree->system);
    REQUIRE(create);
    REQUIRE(copy);
    // Kept in the main menu by the first rule; the later move leaves it alone.
    CHECK(create->moved == "/");
    CHECK(create->rules == std::vector<int>{ 0 });
    // Copy has no such rule: the later file moves it.
    CHECK(copy->moved == "More");
    CHECK(copy->rules == std::vector<int>{ 2 });
    // Out of Send to into the main menu, and kept there too.
    if (zip) {
        CHECK(menumap::stripAccelerators(zip->path) == "send to"); // in= paths are normalized titles
        CHECK(zip->moved == "/");
        CHECK(zip->rules == std::vector<int>{ 1 });
    } else {
        MESSAGE("Send to has no Compressed (zipped) folder here; that part is skipped");
    }
}

TEST_CASE("menu map: the This PC context lists the items for type=computer (real shell.exe)")
{
    if (!haveShell()) {
        MESSAGE("shell.exe/shell.dll not in src/bin; skipped");
        return;
    }
    Scratch s;
    s.write("shell.nss", "item(title='Only for This PC' type='computer' cmd='a')\r\n"
                         "item(title='Only for files' type='file' cmd='b')\r\n"
                         "item(title='Everywhere' type='*' cmd='c')\r\n");
    auto tree = runTree(s.path / "shell.nss", L"thispc");
    REQUIRE(tree);
    REQUIRE(tree->ok);
    CHECK(findNode(tree->shell, "Only for This PC"));
    CHECK_FALSE(findNode(tree->shell, "Only for files"));
    CHECK(findNode(tree->shell, "Everywhere"));
    // Windows' own items for the Computer folder (Properties, at least).
    CHECK(tree->systemError.empty());
    CHECK_FALSE(tree->system.empty());
}

TEST_CASE("menu move: Move to and the places offered only use what the menu has")
{
    // Your menus A (with a menu A2 in it and an item X), B (switched off) and C (left out
    // by its where=); Windows' Send to, which a rule sends Share into; Hidden Windows' Edit.
    auto t = parseTree(R"({"ok":true,"context":"file","errors":[],"tree":{
      "shell":[
        {"kind":"menu","title":"A","state":"text","file":"C:\\c\\a.nss","line":1,"column":1,"shows":"yes","items":[
          {"kind":"menu","title":"A2","state":"text","file":"C:\\c\\a.nss","line":2,"column":1,"shows":"yes","items":[]},
          {"kind":"item","title":"X","state":"text","file":"C:\\c\\a.nss","line":3,"column":1,"shows":"yes","items":[]},
          {"kind":"item","title":"Off","state":"text","file":"C:\\c\\a.nss","line":4,"column":1,"shows":"yes","vis":"hidden","items":[]},
          {"kind":"item","title":"Away","state":"text","file":"C:\\c\\a.nss","line":5,"column":1,"shows":"yes","menu":"Nowhere","items":[]},
          {"kind":"item","title":"Left out","state":"text","file":"C:\\c\\a.nss","line":6,"column":1,"shows":"no","shows_why":"where","items":[]},
          {"kind":"item","title":"Y","state":"text","file":"C:\\c\\a.nss","line":7,"column":1,"shows":"yes","items":[]}]},
        {"kind":"menu","title":"B","state":"text","file":"C:\\c\\a.nss","line":8,"column":1,"shows":"yes","vis":"hidden","items":[]},
        {"kind":"menu","title":"C","state":"text","file":"C:\\c\\a.nss","line":9,"column":1,"shows":"no","shows_why":"where","items":[]}],
      "system":[
        {"kind":"item","title":"&Share","name":"Share","moved":"Send to","items":[]},
        {"kind":"menu","title":"Se&nd to","name":"Send to","items":[]}],
      "rules":[],"system_error":"",
      "order":[{"from":"shell","index":0},{"from":"shell","index":1},{"from":"shell","index":2},
               {"from":"system","index":0},{"from":"system","index":1}]}})");
    REQUIRE(t);
    auto list = buildMenuList(*t);
    REQUIRE(list);
    const auto &entries = list->entries;
    auto at = [&](const char *title) {
        for (size_t i = 0; i < entries.size(); ++i)
            if (entries[i].node && entries[i].node->title == title && entries[i].group != 2 && entries[i].group != 4)
                return int(i);
        return -1;
    };
    auto any = [&](const char *title) {
        for (size_t i = 0; i < entries.size(); ++i)
            if (entries[i].node && entries[i].node->title == title)
                return int(i);
        return -1;
    };

    // With Windows' Send to selected: your menu A and its A2, not itself, not B or C.
    const int sendTo = at("Send to");
    REQUIRE(sendTo >= 0);
    CHECK(canMoveInto(entries, at("A"), sendTo));
    CHECK(canMoveInto(entries, at("A2"), sendTo));
    CHECK_FALSE(canMoveInto(entries, sendTo, sendTo));
    CHECK(at("B") < 0); // switched off: not a menu of this menu
    CHECK_FALSE(canMoveInto(entries, any("B"), sendTo));
    CHECK_FALSE(canMoveInto(entries, any("C"), sendTo));
    // With A selected: not A, and not A2, which is inside it; Send to is fine.
    CHECK_FALSE(canMoveInto(entries, at("A"), at("A")));
    CHECK_FALSE(canMoveInto(entries, at("A2"), at("A")));
    CHECK(canMoveInto(entries, sendTo, at("A")));
    // Share was sent to Send to by a rule: it is listed in it.
    CHECK(entries[size_t(at("Share"))].parent == sendTo);

    // The places in A: next to X and Y, which the menu has. Not Off, Away or Left out.
    std::vector<Sibling> siblings;
    size_t index = 0;
    const int a = at("A");
    for (int k : entries[size_t(a)].kids) {
        const auto &e = entries[size_t(k)];
        if (e.node->title == "X")
            index = siblings.size();
        siblings.push_back(siblingOf(*e.node, nullptr, true, "", ""));
    }
    std::vector<std::string> titles;
    for (const auto &k : entries[size_t(a)].kids)
        titles.push_back(entries[size_t(k)].node->title);
    CHECK(titles == std::vector<std::string>{ "A2", "X", "Y" });
    std::vector<std::string> offered;
    for (const auto &p : placesNextTo(siblings, index))
        offered.push_back((p.after ? "after " : "before ") + p.title);
    CHECK(offered == std::vector<std::string>{ "before A2", "after A2", "before Y", "after Y" });
    // Moving up or down steps over what isn't there: X goes past A2, then it can't go further.
    CHECK(planReorder(siblings, index, true).possible);
    CHECK_FALSE(planReorder(siblings, 0, true).possible);
}
