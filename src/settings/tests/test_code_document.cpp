// Tests for code::Document (src/CodeDocument.h), the text model behind the
// code editors: edits reported by the TextInput, commands, undo/redo, the tab
// glyphs, find, and saving the same bytes back.

#include "doctest.h"

#include "CodeDocument.h"
#include "EditorSession.h"
#include "Highlight.h"

#include "nss/Text.h"

#include <windows.h>

#include <chrono>
#include <fstream>
#include <random>

namespace fs = std::filesystem;

namespace {

// What the TextInput does when the user types `s` with the caret at `caret`
// (or over the selection [anchor, caret)); returns the caret afterwards.
struct Typist
{
    code::Document &doc;
    int64_t clock = 1000;
    size_t caret = 0;

    void apply(const code::Result &r, size_t fallback)
    {
        caret = r.select ? r.selection.cursor : fallback;
    }
    void type(const std::string &s)
    {
        for (size_t i = 0; i < s.size();) {
            size_t n = 1;
            while (i + n < s.size() && (static_cast<unsigned char>(s[i + n]) & 0xC0) == 0x80)
                ++n;
            auto shown = doc.display();
            shown.insert(caret, s.substr(i, n));
            auto r = doc.input(shown, caret + n, { caret, caret }, clock += 100);
            apply(r, caret + n);
            i += n;
        }
    }
    // Backspace: the TextInput removes the character before the caret.
    void backspace(int times = 1)
    {
        for (int k = 0; k < times; ++k) {
            auto shown = doc.display();
            if (caret == 0)
                return;
            size_t from = caret - 1;
            while (from > 0 && (static_cast<unsigned char>(shown[from]) & 0xC0) == 0x80)
                --from;
            shown.erase(from, caret - from);
            auto r = doc.input(shown, from, { caret, caret }, clock += 100);
            apply(r, from);
        }
    }
    void del(int times = 1)
    {
        for (int k = 0; k < times; ++k) {
            auto shown = doc.display();
            if (caret >= shown.size())
                return;
            size_t to = caret + 1;
            while (to < shown.size() && (static_cast<unsigned char>(shown[to]) & 0xC0) == 0x80)
                ++to;
            shown.erase(caret, to - caret);
            auto r = doc.input(shown, caret, { caret, caret }, clock += 100);
            apply(r, caret);
        }
    }
};

const std::string L = "\xE2\x80\x87", P = "\xE2\x80\x88"; // the default tab glyphs (U+2007, U+2008)

bool consistent(const code::Document &d)
{
    return d.display() == hl::tabsToDisplay(d.text(), d.glyphs(), d.tabWidth()) &&
           hl::tabsFromDisplay(d.display(), d.glyphs()) == d.text();
}

struct TempFile
{
    fs::path path;
    explicit TempFile(const std::string &bytes)
    {
        path = fs::temp_directory_path() /
               (L"nsp-code-doc-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L".nss");
        std::ofstream(path, std::ios::binary).write(bytes.data(), std::streamsize(bytes.size()));
    }
    ~TempFile()
    {
        std::error_code ec;
        fs::remove(path, ec);
    }
};

} // namespace

TEST_CASE("code document: offsets between the file text and the display")
{
    code::Document d("a\tb\n\t\tc\u00e9\td\n");
    REQUIRE(d.glyphs().valid());
    CHECK(d.display() == "a" + L + P + P + "b\n" + L + P + P + P + L + P + P + P + "c\u00e9" + L + P + "d\n");
    CHECK(consistent(d));
    for (size_t t = 0; t <= d.text().size(); ++t) {
        if (t < d.text().size() && (static_cast<unsigned char>(d.text()[t]) & 0xC0) == 0x80)
            continue;
        CAPTURE(t);
        CHECK(d.toText(d.toDisplay(t)) == t);
    }
    // Inside a tab's glyphs: after the tab.
    CHECK(d.toText(1 + L.size()) == 2);
    CHECK(d.toText(1) == 1);
}

TEST_CASE("code document: typing is one undo step per word, Backspace runs merge")
{
    code::Document d("menu\n");
    Typist t{ d };
    t.caret = 4;
    t.type(" item title");
    CHECK(d.text() == "menu item title\n");
    CHECK(t.caret == 15);

    // A step per word: undo takes back "title", then "item ", then " ".
    auto r = d.undo();
    CHECK(d.text() == "menu item \n");
    CHECK(r.select);
    CHECK(r.selection.cursor == 10);
    d.undo();
    CHECK(d.text() == "menu \n");
    d.undo();
    CHECK(d.text() == "menu\n");
    CHECK(!d.canUndo());
    d.redo();
    d.redo();
    d.redo();
    CHECK(d.text() == "menu item title\n");
    CHECK(!d.canRedo());

    // Backspace x5 is one step; undo puts the caret back after the restored text.
    t.caret = 15;
    t.backspace(5);
    CHECK(d.text() == "menu item \n");
    r = d.undo();
    CHECK(d.text() == "menu item title\n");
    CHECK(r.selection.cursor == 15);

    // Delete forward runs merge too.
    t.caret = 4;
    t.del(5);
    CHECK(d.text() == "menu title\n");
    d.undo();
    CHECK(d.text() == "menu item title\n");

    // A pause of more than two seconds starts a new step.
    t.caret = 15;
    t.type("x");
    t.clock += 5000;
    t.type("y");
    d.undo();
    CHECK(d.text() == "menu item titlex\n");
}

TEST_CASE("code document: the text written back only when the display must change")
{
    code::Document d("\tab\n");
    Typist t{ d };
    t.caret = (L + P + P + P).size() + 1;
    auto shown = d.display();
    shown.insert(t.caret, "x");
    auto r = d.input(shown, t.caret + 1, { t.caret, t.caret }, 1);
    CHECK(r.edited);
    CHECK(!r.display); // typing after the tab: the TextInput already shows it
    CHECK(!r.select);
    CHECK(d.text() == "\taxb\n");

    // Typing before a tab re-pads it: the text is written back with the caret.
    code::Document e("a\tb");
    shown = e.display();
    shown.insert(0, "x");
    r = e.input(shown, 1, { 0, 0 }, 1);
    CHECK(e.text() == "xa\tb");
    CHECK(r.display);
    CHECK(r.select);
    CHECK(r.selection.cursor == 1);
    CHECK(e.display() == "xa" + L + P + "b");
    e.undo();
    CHECK(e.text() == "a\tb");
    CHECK(consistent(e));

    // Backspace on the last pad deletes the whole tab.
    code::Document f("\t\tx");
    Typist g{ f };
    g.caret = f.display().size() - 1;
    g.backspace();
    CHECK(f.text() == "\tx");
    CHECK(g.caret == (L + P + P + P).size());
    // Delete on a lead deletes the whole tab.
    g.caret = 0;
    g.del();
    CHECK(f.text() == "x");
    f.undo();
    f.undo();
    CHECK(f.text() == "\t\tx");
}

TEST_CASE("code document: paste keeps tabs, normalises line ends, is its own undo step")
{
    code::Document d("menu\n{\n}\n");
    Typist t{ d };
    t.caret = 7; // after "{\n"
    t.type("// ");
    auto r = d.paste({ t.caret, t.caret }, "item(title='a'\tcmd='b')\r\n\tsep\r\nlast\rend");
    CHECK(d.text() == "menu\n{\n// item(title='a'\tcmd='b')\n\tsep\nlast\nend}\n");
    CHECK(r.display);
    CHECK(r.select);
    CHECK(d.display().find('\t') == std::string::npos);
    CHECK(d.display().find('\r') == std::string::npos);
    CHECK(consistent(d));
    CHECK(d.toText(r.selection.cursor) == d.text().find("end") + 3);
    d.undo();
    CHECK(d.text() == "menu\n{\n// }\n");
    d.undo();
    CHECK(d.text() == "menu\n{\n}\n");

    // Pasting over a selection replaces it; cut removes it.
    code::Document e("abc def");
    e.paste({ 4, 7 }, "xyz");
    CHECK(e.text() == "abc xyz");
    CHECK(e.selectedText({ 0, 3 }) == "abc");
    e.cut({ 0, 4 });
    CHECK(e.text() == "xyz");
    e.undo();
    e.undo();
    CHECK(e.text() == "abc def");

    // Text with the glyph characters themselves gets another pair of glyphs.
    code::Document f("\tx");
    f.paste({ f.display().size(), f.display().size() }, " fig" + L + "space");
    CHECK(f.text() == "\tx fig" + L + "space");
    CHECK(f.glyphs().lead != U'\u2007');
    CHECK(consistent(f));
    CHECK(f.selectedText({ 0, f.display().size() }) == f.text());
}

TEST_CASE("code document: Enter keeps the indentation")
{
    code::Document d("menu\n{\n\titem(title='a')\n}\n");
    size_t end = d.toDisplay(d.text().find("')") + 2);
    auto r = d.newline({ end, end });
    CHECK(d.text() == "menu\n{\n\titem(title='a')\n\t\n}\n");
    CHECK(d.toText(r.selection.cursor) == d.text().find("\t\n}") + 1);

    // After "{": one level deeper; "{|}" puts the brace on its own line.
    code::Document e("menu{}");
    r = e.newline({ 5, 5 });
    CHECK(e.text() == "menu{\n\t\n}");
    CHECK(e.toText(r.selection.cursor) == 7);
    e.undo();
    CHECK(e.text() == "menu{}");

    // Spaces stay spaces.
    code::Document f("menu\n{\n    item\n}\n");
    CHECK(f.indentUnit() == "    ");
    size_t p = f.text().find("item") + 4;
    f.newline({ p, p });
    CHECK(f.text() == "menu\n{\n    item\n    \n}\n");
}

TEST_CASE("code document: Tab and Shift+Tab")
{
    code::Document d("a\nb\n\nc\n");
    CHECK(d.indentUnit() == "\t");
    // No selection: a tab at the caret.
    auto r = d.tab({ 0, 0 });
    CHECK(d.text() == "\ta\nb\n\nc\n");
    d.undo();
    // Several lines: each non-empty line indented; the selection follows.
    r = d.tab({ 0, d.toDisplay(6) }); // "a\nb\n\nc" selected
    CHECK(d.text() == "\ta\n\tb\n\n\tc\n");
    CHECK(r.selection.anchor == 0);
    CHECK(d.toText(r.selection.cursor) == d.text().find('c') + 1);
    // Shift+Tab takes one level off the same lines.
    r = d.outdent(r.selection);
    CHECK(d.text() == "a\nb\n\nc\n");
    // A selection ending at the start of a line leaves that line alone.
    d.tab({ 0, d.toDisplay(2) });
    CHECK(d.text() == "\ta\nb\n\nc\n");
    // Shift+Tab with no selection works on the caret's line; nothing to take off: no step.
    d.outdent({ 0, 0 });
    CHECK(d.text() == "a\nb\n\nc\n");
    CHECK(!d.outdent({ 0, 0 }).edited);
    d.undo();
    d.undo();
    CHECK(d.text() == "a\nb\n\nc\n");

    // Four-space files indent with spaces to the next stop.
    code::Document s("x {\n    y\n    z\n}\n");
    CHECK(s.indentUnit() == "    ");
    s.tab({ 1, 1 });
    CHECK(s.text() == "x    {\n    y\n    z\n}\n");
    s.undo();
    s.outdent({ s.toDisplay(5), s.toDisplay(15) });
    CHECK(s.text() == "x {\ny\nz\n}\n");
    // Two-space files
    code::Document two("a\n  b\n    c\n  d\n");
    CHECK(two.indentUnit() == "  ");
}

TEST_CASE("code document: find")
{
    code::Document d("item\n\titem(Title='item')\nITEM\n");
    auto m = d.find("item", { 0, 0 }, false, true);
    REQUIRE(m);
    CHECK(m->count == 4);
    CHECK(m->index == 1);
    CHECK(m->selection.anchor == 0);
    m = d.find("item", m->selection, false);
    REQUIRE(m);
    CHECK(m->index == 2);
    CHECK(d.selectedText(m->selection) == "item");
    m = d.find("item", m->selection, true);
    CHECK(m->index == 1);
    m = d.find("item", m->selection, true); // wraps to the last
    CHECK(m->index == 4);
    CHECK(d.selectedText(m->selection) == "ITEM");
    CHECK(!d.find("nothing", { 0, 0 }, false));
    CHECK(!d.find("", { 0, 0 }, false));
}

TEST_CASE("code document: edits and undo give the file's bytes back unchanged")
{
    // UTF-8 with a BOM, CRLF, tabs inside lines and in indentation, non-ASCII.
    const std::string bytes = "\xEF\xBB\xBFmenu(title='Caf\xC3\xA9')\r\n{\r\n\titem(title='a'\tcmd='b')\r\n\t\t// nested\r\n}\r\n";
    TempFile file(bytes);
    auto buf = editor::loadBuffer(file.path);
    REQUIRE(buf);
    code::Document d(buf->text);
    Typist t{ d };
    t.caret = d.toDisplay(d.text().find("'b')") + 4);
    auto r = d.newline({ t.caret, t.caret });
    t.caret = r.selection.cursor;
    t.type("sep");
    t.backspace(2);
    d.paste({ t.caret, t.caret }, "ep\r\n\tseparator");
    d.tab({ 0, d.display().size() });
    d.outdent({ 0, d.display().size() });
    CHECK(consistent(d));
    buf->text = d.text();
    CHECK(buf->dirty());
    auto edited = nss::encode(editor::fromEditorText(buf->text, buf->format.eol), buf->format);
    CHECK(edited == "\xEF\xBB\xBFmenu(title='Caf\xC3\xA9')\r\n{\r\n\titem(title='a'\tcmd='b')\r\n\tsep\r\n\tseparator\r\n\t\t// nested\r\n}\r\n");
    CHECK(edited.find("\xE2\x80") == std::string::npos);

    while (d.canUndo())
        d.undo();
    buf->text = d.text();
    CHECK(!buf->dirty());
    CHECK(nss::encode(editor::fromEditorText(buf->text, buf->format.eol), buf->format) == bytes);
    while (d.canRedo())
        d.redo();
    CHECK(nss::encode(editor::fromEditorText(d.text(), buf->format.eol), buf->format) == edited);
}

TEST_CASE("code document: random edits stay consistent and undo to the start")
{
    const std::string start = "menu(title='x')\n{\n\titem(title='a'\tcmd='b')\n\t\t// c\u00e9\n  \tz\n}\n";
    code::Document d(start);
    std::mt19937 rng(7);
    const std::string pieces[] = { "a", "\t", "\n", "\u00e9", " ", "{", "x\ty", "\r\n" };
    int64_t clock = 0;
    for (int step = 0; step < 400; ++step) {
        const auto &shown0 = d.display();
        size_t pos = rng() % (shown0.size() + 1);
        while (pos > 0 && pos < shown0.size() && (static_cast<unsigned char>(shown0[pos]) & 0xC0) == 0x80)
            --pos;
        pos = d.toDisplay(d.toText(pos));
        int op = int(rng() % 7);
        if (op <= 2) {
            auto s = hl::tabsToDisplay(pieces[rng() % 6], d.glyphs());
            auto shown = shown0;
            shown.insert(pos, s);
            d.input(shown, pos + s.size(), { pos, pos }, clock += 50);
        } else if (op == 3 && pos > 0) {
            auto shown = shown0;
            size_t from = pos - 1;
            while (from > 0 && (static_cast<unsigned char>(shown[from]) & 0xC0) == 0x80)
                --from;
            shown.erase(from, pos - from);
            d.input(shown, from, { pos, pos }, clock += 50);
        } else if (op == 4) {
            d.paste({ pos, pos }, pieces[rng() % 8]);
        } else if (op == 5) {
            d.tab({ 0, pos });
        } else {
            d.outdent({ pos, pos });
        }
        REQUIRE(consistent(d));
    }
    while (d.canUndo())
        d.undo();
    CHECK(d.text() == start);
    CHECK(consistent(d));
}

TEST_CASE("code document: typing in a 2,000-line file")
{
    std::string text;
    for (int i = 0; i < 2000; ++i)
        text += "\titem(title='Item " + std::to_string(i) + "' cmd='notepad.exe' args='\"@sel.path\"' image=\\uE0B3)\n";
    code::Document d(text);
    Typist t{ d };
    t.caret = d.toDisplay(text.size() / 2);
    auto begin = std::chrono::steady_clock::now();
    const int keys = 50;
    for (int k = 0; k < keys; ++k) {
        t.type("x");
        auto lines = hl::highlight(d.text(), d.tabWidth(), &d.glyphs()); // what the controller does per key
        REQUIRE(lines.size() == 2001);
    }
    auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count() / keys;
    MESSAGE("per keystroke (model + full highlight), " << text.size() / 1024 << " KB: " << ms << " ms");
    CHECK(ms < 40.0);
}

TEST_CASE("code document: typing over a selection undoes back to the selection")
{
    code::Document d("menu item");
    auto shown = d.display();
    shown.replace(5, 4, "x"); // "item" selected, "x" typed
    auto r = d.input(shown, 6, { 5, 9 }, 1000);
    CHECK(d.text() == "menu x");
    CHECK(!r.display);
    r = d.undo();
    CHECK(d.text() == "menu item");
    CHECK(r.selection == code::Selection{ 5, 9 });
}

TEST_CASE("code document: the selection by rows, for drawing it")
{
    const std::string_view s = "ab\ncde\n\nf";
    CHECK(code::rowSpan(s, 1, 1).first == -1);
    auto r = code::rowSpan(s, 1, 2);
    CHECK(r.first == 0);
    CHECK(r.last == 0);
    CHECK(r.startPrefix == "a");
    CHECK(r.endPrefix == "ab");
    r = code::rowSpan(s, 5, 1); // backwards: cursor before anchor
    CHECK(r.first == 0);
    CHECK(r.last == 1);
    CHECK(r.startPrefix == "a");
    CHECK(r.endPrefix == "cd");
    r = code::rowSpan(s, 0, s.size()); // Ctrl+A
    CHECK(r.first == 0);
    CHECK(r.last == 3);
    CHECK(r.startPrefix == "");
    CHECK(r.endPrefix == "f");
    r = code::rowSpan(s, 3, 8); // to the start of a line: that line is in, empty
    CHECK(r.first == 1);
    CHECK(r.last == 3);
    CHECK(r.endPrefix == "");
    CHECK(code::rowSpan(s, 0, 999).last == 3);
}
