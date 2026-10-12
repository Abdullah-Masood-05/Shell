// Tests for the Config Editor and Backups logic (no UI): Diff, Highlight,
// FileWatcher, EditorSession and BackupsModel. The end-to-end cases run the
// real shell.exe from src/bin against a per-user config seeded with
// `shell.exe -silent -config:init` under a temporary %APPDATA%.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "BackupsModel.h"
#include "Diff.h"
#include "EditorSession.h"
#include "FileWatcher.h"
#include "Highlight.h"

#include "nss/ConfigStore.h"
#include "nss/Text.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <random>
#include <thread>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

fs::path binDir() { return fs::path(SETTINGS_SOURCE_BIN).lexically_normal(); }
fs::path shellExe() { return binDir() / L"shell.exe"; }

std::string readAll(const fs::path &p)
{
    std::string s;
    nss::readFile(p, s);
    return s;
}

void writeAll(const fs::path &p, std::string_view bytes)
{
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), std::streamsize(bytes.size()));
}

struct TempDir
{
    fs::path path;
    TempDir()
    {
        static std::atomic<int> n{ 0 };
        path = fs::temp_directory_path() /
               (L"settings-tests-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++n));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

// Seeds a per-user config like a real user would get it: `shell.exe -silent
// -config:init` with %APPDATA% pointing at a temp folder. Returns the root.
fs::path seedConfig(const fs::path &appData)
{
    SetEnvironmentVariableW(L"APPDATA", appData.c_str());
    std::wstring cmd = L"\"" + shellExe().wstring() + L"\" -silent -config:init";
    STARTUPINFOW si{ sizeof si };
    PROCESS_INFORMATION pi{};
    REQUIRE(CreateProcessW(shellExe().c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                           nullptr, &si, &pi));
    WaitForSingleObject(pi.hProcess, 30000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    auto root = appData / L"Nilesoft" / L"Shell" / L"shell.nss";
    REQUIRE(fs::is_regular_file(root));
    return root;
}

std::string applyEdits(const std::vector<std::string> &a, const std::vector<std::string> &b,
                       const std::vector<diff::Edit> &edits)
{
    std::string out;
    size_t ai = 0;
    for (const auto &e : edits) {
        if (e.op == diff::Op::Same) {
            REQUIRE(e.oldLine == int(ai) + 1);
            REQUIRE(a[ai] == b[size_t(e.newLine - 1)]);
            out += a[ai++] + "\n";
        } else if (e.op == diff::Op::Removed) {
            REQUIRE(e.oldLine == int(ai) + 1);
            ++ai;
        } else {
            out += b[size_t(e.newLine - 1)] + "\n";
        }
    }
    REQUIRE(ai == a.size());
    return out;
}

} // namespace

TEST_CASE("diff: split lines")
{
    CHECK(diff::splitLines("").empty());
    CHECK(diff::splitLines("a\r\nb\nc\rd") == std::vector<std::string>{ "a", "b", "c", "d" });
    CHECK(diff::splitLines("a\n") == std::vector<std::string>{ "a" });
    CHECK(diff::splitLines("a\n\n") == std::vector<std::string>{ "a", "" });
}

TEST_CASE("diff: minimal scripts")
{
    std::vector<std::string> a{ "a", "b", "c", "a", "b", "b", "a" };
    std::vector<std::string> b{ "c", "b", "a", "b", "a", "c" };
    auto edits = diff::diffLines(a, b);
    int changes = 0;
    for (auto &e : edits)
        changes += e.op != diff::Op::Same;
    CHECK(changes == 5); // the classic Myers example: D = 5
    std::string expect;
    for (auto &s : b)
        expect += s + "\n";
    CHECK(applyEdits(a, b, edits) == expect);

    auto u = diff::unified("one\ntwo\nthree\n", "one\n2\nthree\nfour\n");
    CHECK(u.added == 2);
    CHECK(u.removed == 1);
    REQUIRE(!u.rows.empty());
    CHECK(u.rows[0].kind == diff::Row::Hunk);
    CHECK(u.rows[0].text == "@@ -1,3 +1,4 @@");

    CHECK(diff::unified("same\n", "same\r\n").rows.empty());
}

TEST_CASE("diff: random scripts apply")
{
    std::mt19937 rng(12345);
    for (int round = 0; round < 300; ++round) {
        auto make = [&](int n) {
            std::vector<std::string> v;
            for (int i = 0; i < n; ++i)
                v.push_back(std::string(1, char('a' + rng() % 4)));
            return v;
        };
        auto a = make(int(rng() % 40));
        auto b = make(int(rng() % 40));
        auto edits = diff::diffLines(a, b, round % 3 == 0 ? 3 : 4000);
        std::string expect;
        for (auto &s : b)
            expect += s + "\n";
        CHECK(applyEdits(a, b, edits) == expect);
    }
}

TEST_CASE("highlight: token kinds")
{
    std::string src = "// note\nmenu(title=\"Copy\" image=#ff0000 pos=2)\n$x = 'a\nb' /* c */ app.dir";
    auto lines = hl::highlight(src);
    REQUIRE(lines.size() == 4);
    REQUIRE(lines[0].spans.size() == 1);
    CHECK(lines[0].spans[0].kind == hl::Comment);
    auto kindOf = [&](size_t line, std::string_view text) {
        for (auto &s : lines[line].spans)
            if (s.text == text)
                return s.kind;
        return -1;
    };
    CHECK(kindOf(1, "menu") == hl::Keyword);
    CHECK(kindOf(1, "title") == hl::Property);
    CHECK(kindOf(1, "\"Copy\"") == hl::String);
    CHECK(kindOf(1, "#ff0000") == hl::Color);
    CHECK(kindOf(1, "2") == hl::Number);
    CHECK(kindOf(2, "$x") == hl::Variable);
    CHECK(kindOf(2, "'a") == hl::String); // a multi-line string is split per line
    CHECK(kindOf(3, "b'") == hl::String);
    CHECK(kindOf(3, "/* c */") == hl::Comment);
    CHECK(kindOf(3, "app.dir") == hl::Ident);

    auto tabs = hl::highlight("\tx");
    REQUIRE(!tabs[0].spans.empty());
    CHECK(tabs[0].spans[0].text == "    ");
}

TEST_CASE("editor: text helpers")
{
    CHECK(editor::toEditorText("a\r\nb\rc\n") == "a\nb\nc\n");
    CHECK(editor::fromEditorText("a\nb", nss::Eol::CRLF) == "a\r\nb");
    auto [b, e] = editor::lineOffsets("ab\ncd\nef", 2);
    CHECK(b == 3);
    CHECK(e == 5);
    CHECK(editor::isManaged("C:\\cfg", "C:\\cfg\\imports\\settings-app.nss"));
    CHECK(!editor::isManaged("C:\\cfg", "C:\\cfg\\imports\\theme.nss"));
    CHECK(editor::isProtectedPath("C:\\Program Files\\Nilesoft Shell\\shell.nss"));
    CHECK(!editor::isProtectedPath("C:\\Users\\x\\AppData\\Roaming\\Nilesoft\\Shell\\shell.nss"));

    auto check = editor::parseCheck(
        R"({"ok": false, "root": "C:\\t\\shell.nss", "errors": [{"file": "C:\\t\\imports\\a.nss", "line": 4, "column": 2, "message": "Unexpected"}], "warnings": []})",
        "C:\\t", "D:\\real");
    CHECK(check.ran);
    CHECK(!check.ok);
    REQUIRE(check.issues.size() == 1);
    CHECK(nss::pathKey(check.issues[0].file) == nss::pathKey("D:\\real\\imports\\a.nss"));
    CHECK(check.issues[0].line == 4);
}

TEST_CASE("file watcher: external changes, own writes ignored")
{
    TempDir dir;
    auto file = dir.path / L"a.nss";
    writeAll(file, "one");

    std::mutex m;
    std::condition_variable cv;
    std::vector<fs::path> seen;
    FileWatcher w;
    REQUIRE(w.start(
        dir.path,
        [&](std::vector<fs::path> changed) {
            std::lock_guard lock(m);
            seen.insert(seen.end(), changed.begin(), changed.end());
            cv.notify_all();
        },
        [](const fs::path &p) { return p.extension() == L".nss"; }, true, 100ms));

    auto waitFor = [&](std::chrono::milliseconds t) {
        std::unique_lock lock(m);
        return cv.wait_for(lock, t, [&] { return !seen.empty(); });
    };

    writeAll(file, "two");
    REQUIRE(waitFor(3s));
    {
        std::lock_guard lock(m);
        CHECK(nss::pathKey(seen.front()) == nss::pathKey(file));
        seen.clear();
    }

    // Our own write: the same bytes are noted first, so nothing is reported.
    w.noteOwnWrite(file, "three");
    writeAll(file, "three");
    writeAll(dir.path / L"ignored.txt", "x");
    CHECK(!waitFor(800ms));

    // A later external write with different content is reported again.
    writeAll(file, "four");
    CHECK(waitFor(3s));
    w.stop();
}

TEST_CASE("editor + backups: edit, save, check, restore (real shell.exe)")
{
    if (!fs::is_regular_file(shellExe()) || !fs::is_regular_file(binDir() / L"shell.dll")) {
        MESSAGE("shell.exe/shell.dll not in src/bin; skipped");
        return;
    }
    TempDir app;
    auto root = seedConfig(app.path);
    auto rootDir = root.parent_path();
    nss::ConfigStore store(root);

    // The import tree: root first, its imports under it.
    auto tree = editor::loadTree(root, binDir());
    REQUIRE(!tree.empty());
    CHECK(nss::pathKey(tree[0].path) == nss::pathKey(root));
    CHECK(tree[0].depth == 0);
    bool hasTheme = false;
    for (auto &e : tree)
        if (e.label == "imports/theme.nss" && e.depth == 1 && !e.missing)
            hasTheme = true;
    CHECK(hasTheme);

    // The seeded config passes the check.
    auto clean = editor::checkTree(shellExe(), root);
    CHECK(clean.ran);
    CHECK(clean.ok);

    // 1. Edit theme.nss and save: a snapshot is taken, the file has the new
    //    text with its original BOM and CRLF line endings.
    auto themePath = rootDir / L"imports" / L"theme.nss";
    auto beforeBytes = readAll(themePath);
    auto buf = editor::loadBuffer(themePath);
    REQUIRE(buf);
    auto origFormat = buf->format;
    std::string edited = buf->text;
    auto pos = edited.find("image.align=2");
    REQUIRE(pos != std::string::npos);
    edited.replace(pos, std::string("image.align=2").size(), "image.align=1\n\t// edited by the test");
    auto snapsBefore = store.snapshots().size();
    auto saved = editor::save(shellExe(), root, { { themePath, edited, buf->format } }, false);
    REQUIRE_MESSAGE(saved.ok, saved.error);
    CHECK(!saved.snapshotId.empty());
    CHECK(store.snapshots().size() == snapsBefore + 1);
    auto snap = store.findSnapshot(saved.snapshotId);
    REQUIRE(snap);
    CHECK(snap->reason == "Config Editor: imports/theme.nss");
    auto afterBytes = readAll(themePath);
    auto dec = nss::decode(afterBytes);
    CHECK(dec.format == origFormat);
    CHECK(dec.text == editor::fromEditorText(edited, origFormat.eol));
    CHECK(fs::is_regular_file(fs::path(themePath.wstring() + L".bak")));
    CHECK(readAll(snap->dir / L"imports" / L"theme.nss") == beforeBytes);

    // The diff of that snapshot against the tree shows exactly that change.
    auto d = backups::diffSnapshot(store, *snap);
    CHECK(d.changed == 1);
    REQUIRE(!d.files.empty());
    CHECK(d.files[0].path == "imports/theme.nss");
    CHECK(d.files[0].status == backups::FileDiff::Modified);
    CHECK(d.files[0].diff.added == 2);
    CHECK(d.files[0].diff.removed == 1);

    // 2. A syntax error: the live check (on a temp copy) reports it on the
    //    real path and line, and a normal save is refused without writing.
    auto broken = edited + "\nmenu(title='x'\n";
    auto overlay = editor::checkTree(shellExe(), root, { { themePath, broken, buf->format } });
    CHECK(overlay.ran);
    CHECK(!overlay.ok);
    REQUIRE(!overlay.issues.empty());
    CHECK(overlay.issues[0].error);
    CHECK(nss::pathKey(overlay.issues[0].file) == nss::pathKey(themePath));
    CHECK(overlay.issues[0].line > 0);
    CHECK(readAll(themePath) == afterBytes); // the real file is untouched by the check

    auto count = store.snapshots().size();
    auto refused = editor::save(shellExe(), root, { { themePath, broken, buf->format } }, false);
    CHECK(!refused.ok);
    CHECK(!refused.check.ok);
    CHECK(readAll(themePath) == afterBytes);
    CHECK(store.snapshots().size() == count);

    // "Save anyway" writes it (and snapshots first).
    auto forced = editor::save(shellExe(), root, { { themePath, broken, buf->format } }, true);
    CHECK(forced.ok);
    CHECK(!forced.snapshotId.empty());
    CHECK(!editor::checkTree(shellExe(), root).ok);

    // 3. Restore the first snapshot: the files match it again.
    std::string error;
    REQUIRE_MESSAGE(store.restore(snap->id, &error), error);
    CHECK(readAll(themePath) == beforeBytes);
    CHECK(backups::matchesTree(store, *snap));
    CHECK(editor::checkTree(shellExe(), root).ok);
    // Restore took a "before restore" snapshot of the broken state.
    auto list = store.snapshots();
    CHECK(list.front().reason == "before restore " + snap->id);

    // 4. Pin, delete rules.
    REQUIRE(store.pin(snap->id, true));
    CHECK(!backups::deleteSnapshot(store, snap->id, &error));
    REQUIRE(store.pin(snap->id, false));
    CHECK(backups::deleteSnapshot(store, snap->id, &error));
    CHECK(!store.findSnapshot(snap->id));
}

TEST_CASE("backups: display time")
{
    nss::SnapshotInfo s;
    s.id = "20261012-203107";
    s.created = "2026-10-12T20:31:07";
    CHECK(backups::displayTime(s) == "Oct 12, 2026, 20:31:07");
}

// Seeds a demo history for screenshots and manual checks in a per-user config
// under %SETTINGS_DEMO_APPDATA% (never the real %APPDATA%):
//   settings_tests -tc="demo*" --no-skip
TEST_CASE("demo: seed snapshots and a syntax error" * doctest::skip())
{
    wchar_t buf[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"SETTINGS_DEMO_APPDATA", buf, MAX_PATH)) {
        MESSAGE("SETTINGS_DEMO_APPDATA is not set");
        return;
    }
    fs::path app(buf);
    REQUIRE(nss::toLower(nss::narrow(app.filename().wstring())) != "roaming");
    fs::remove_all(app / L"Nilesoft");
    auto root = seedConfig(app);
    auto dir = root.parent_path();
    nss::ConfigStore store(root);

    auto edit = [&](const fs::path &file, const std::string &from, const std::string &to, bool force) {
        auto b = editor::loadBuffer(file);
        REQUIRE(b);
        auto text = b->text;
        auto pos = text.find(from);
        REQUIRE(pos != std::string::npos);
        text.replace(pos, from.size(), to);
        auto r = editor::save(shellExe(), root, { { file, text, b->format } }, force);
        REQUIRE_MESSAGE(r.ok, r.error);
        return r.snapshotId;
    };
    auto theme = dir / L"imports" / L"theme.nss";
    edit(theme, "\t\topacity=auto", "\t\topacity=85", false);
    Sleep(1100);
    auto manual = store.snapshot("Manual snapshot");
    REQUIRE(manual);
    REQUIRE(store.pin(manual->id, true));
    Sleep(1100);
    edit(dir / L"imports" / L"terminal.nss", "menu(", "// Terminal tools\nmenu(", false);
    Sleep(1100);
    edit(theme, "\timage.align=2", "\timage.align=2\n\tfont.size=14\n\tborder.radius = \n", true);
    CHECK(!editor::checkTree(shellExe(), root).ok);
    MESSAGE("seeded " << nss::narrow(root.wstring()) << " with " << store.snapshots().size() << " snapshots");
}
