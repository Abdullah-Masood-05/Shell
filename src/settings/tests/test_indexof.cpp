// The core's pos=indexof(pattern, offset, default) placement (src/dll/src/Include/IndexOf.h),
// as ContextMenu.cpp uses it once Explorer's items are in the menu: the anchor
// is the first item a pattern matches (the patterns tried in order), and items
// with the same indexof() keep their order in the file.

#include "doctest.h"

#include "../../dll/src/Include/IndexOf.h"

#include <cctype>
#include <string>
#include <vector>

namespace {

using Menu = std::vector<std::string>;
namespace IndexOf = Nilesoft::Shell::IndexOf;

// Shell's matching for a quoted pattern: the whole title, any case, '&' ignored.
std::string norm(const std::string &s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '&' && !(i + 1 < s.size() && s[i + 1] == '&'))
            continue;
        out += char(std::tolower(static_cast<unsigned char>(s[i])));
    }
    return out;
}

std::ptrdiff_t find(const Menu &menu, const std::vector<std::string> &patterns)
{
    for (const auto &p : patterns)
        for (size_t i = 0; i < menu.size(); ++i)
            if (norm(menu[i]) == norm(p))
                return std::ptrdiff_t(i);
    return -1;
}

// Places `items` in order as ContextMenu.cpp's add_custom_items does for one
// indexof('"WinRAR"|"Open in new window"', offset, def).
Menu place(Menu menu, const std::vector<std::string> &items, int offset = 1, int def = IndexOf::Top)
{
    const std::vector<std::string> anchors = { "WinRAR", "Open in new window" };
    std::string last;
    bool first = true;
    for (const auto &item : items) {
        auto anchor = first ? find(menu, anchors) : -1;
        IndexOf::insert(menu, item, first ? nullptr : &last, anchor, offset, def);
        last = item;
        first = false;
    }
    return menu;
}

const std::vector<std::string> kOurs = { "Open in VS Code", "Open in Zed", "Open in Cursor" };

} // namespace

TEST_CASE("indexof: right under WinRAR, in the file's order")
{
    Menu menu = { "Open", "Open in new tab", "Open in new window", "Win&RAR", "Add to VLC", "Pin to Quick access" };
    CHECK(place(menu, kOurs) == Menu{ "Open", "Open in new tab", "Open in new window", "Win&RAR", "Open in VS Code",
                                      "Open in Zed", "Open in Cursor", "Add to VLC", "Pin to Quick access" });
}

TEST_CASE("indexof: no WinRAR, so under Open in new window")
{
    Menu menu = { "Open", "Open in new tab", "Open in new window", "Pin to Quick access" };
    CHECK(place(menu, kOurs) == Menu{ "Open", "Open in new tab", "Open in new window", "Open in VS Code", "Open in Zed",
                                      "Open in Cursor", "Pin to Quick access" });
    // WinRAR wins even when it comes first in the menu.
    Menu early = { "WinRAR", "Open", "Open in new window" };
    CHECK(place(early, { "A", "B" }) == Menu{ "WinRAR", "A", "B", "Open", "Open in new window" });
    // Whole titles only: "Add to WinRAR archive" is no anchor.
    Menu similar = { "Open", "Add to WinRAR archive", "Open in new window" };
    CHECK(place(similar, { "A" }) == Menu{ "Open", "Add to WinRAR archive", "Open in new window", "A" });
}

TEST_CASE("indexof: neither (a file, or Windows in another language), so the top")
{
    Menu german = { "Öffnen", "In neuem Fenster öffnen", "Kopieren" };
    CHECK(place(german, kOurs) ==
          Menu{ "Open in VS Code", "Open in Zed", "Open in Cursor", "Öffnen", "In neuem Fenster öffnen", "Kopieren" });
    CHECK(place({}, kOurs) == kOurs);
    // Other defaults.
    CHECK(place({ "a", "b", "c", "d" }, { "X", "Y" }, 1, IndexOf::Middle) == Menu{ "a", "b", "X", "Y", "c", "d" });
    CHECK(place({ "a", "b" }, { "X", "Y" }, 1, -1) == Menu{ "a", "b", "X", "Y" });
}

TEST_CASE("indexof: the offset, kept inside the menu")
{
    Menu menu = { "Open", "WinRAR", "Copy" };
    CHECK(place(menu, { "A", "B" }, 0) == Menu{ "Open", "A", "B", "WinRAR", "Copy" }); // before it
    CHECK(place(menu, { "A", "B" }, 2) == Menu{ "Open", "WinRAR", "Copy", "A", "B" });
    CHECK(place(menu, { "A" }, 40) == Menu{ "Open", "WinRAR", "Copy", "A" });  // past the end: the end
    CHECK(place(menu, { "A" }, -40) == Menu{ "A", "Open", "WinRAR", "Copy" }); // before the start: the start
}

TEST_CASE("indexof: the previous item of the group is gone")
{
    // Not in the menu (e.g. removed by a rule): the next one is placed on its own.
    Menu menu = { "Open", "WinRAR" };
    std::string ghost = "ghost";
    IndexOf::insert(menu, std::string("B"), &ghost, 1, 1, IndexOf::Top);
    CHECK(menu == Menu{ "Open", "WinRAR", "B" });
}

TEST_CASE("indexof: a number as the default position")
{
    // No match: the item goes at that index, kept inside the menu.
    CHECK(place({ "a", "b", "c" }, { "X" }, 1, 0) == Menu{ "X", "a", "b", "c" });
    CHECK(place({ "a", "b", "c" }, { "X", "Y" }, 1, 2) == Menu{ "a", "b", "X", "Y", "c" });
    CHECK(place({ "a", "b" }, { "X" }, 1, 40) == Menu{ "a", "b", "X" });
    // A match still wins over the number.
    CHECK(place({ "Open", "WinRAR", "Copy" }, { "X" }, 1, 0) == Menu{ "Open", "WinRAR", "X", "Copy" });
}
