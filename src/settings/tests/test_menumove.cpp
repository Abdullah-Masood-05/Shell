// Moving and ordering Shell's own items on the Menu map page (MenuMap.h): menu=
// and pos= written onto an item's declaration and taken out again, a declaration
// moved up or down in its file, and the order pos= gives a menu
// (src/dll/src/Include/MenuPlacement.h, as `shell -config:tree` and the menu use it).

#include "doctest.h"

#include "MenuMap.h"

#include "nss/NssDocument.h"

#include "../../dll/src/Include/MenuPlacement.h"

#include <string>
#include <vector>

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
