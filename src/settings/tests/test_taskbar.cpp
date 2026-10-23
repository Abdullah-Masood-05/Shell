// TaskbarApps: the Apps submenu of the taskbar's menu (imports/settings-app-taskbar.nss),
// and the Context Menus page's own modules (ConfigRules: addModule / removeModule).
// The last cases run the real shell.exe from src/bin against a per-user config seeded
// with `shell.exe -silent -config:init` under a temporary %APPDATA%.

#include "doctest.h"

#include "ConfigRules.h"
#include "TaskbarApps.h"

#include "nss/ConfigStore.h"
#include "nss/NssDocument.h"
#include "nss/Text.h"

#include <windows.h>

#include <atomic>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using namespace taskbarapps;

namespace {

fs::path binDir() { return fs::path(SETTINGS_SOURCE_BIN).lexically_normal(); }
fs::path shellExe() { return binDir() / L"shell.exe"; }
bool haveShell() { return fs::is_regular_file(shellExe()) && fs::is_regular_file(binDir() / L"shell.dll"); }

// "@sys.prog\x" <-> "C:\PF\x", "@sys.prog32\x" <-> "C:\PF86\x".
Paths fakePaths()
{
    Paths p;
    p.expand = [](std::string_view s) {
        std::string t(s);
        if (t.rfind("@sys.prog32\\", 0) == 0)
            return "C:\\PF86" + t.substr(11);
        if (t.rfind("@sys.prog\\", 0) == 0)
            return "C:\\PF" + t.substr(9);
        return t;
    };
    p.contract = [](std::string_view s) {
        std::string t(s);
        if (t.rfind("C:\\PF86\\", 0) == 0)
            return "@sys.prog32" + t.substr(7);
        if (t.rfind("C:\\PF\\", 0) == 0)
            return "@sys.prog" + t.substr(5);
        return t;
    };
    return p;
}

struct TempDir
{
    fs::path path;
    TempDir()
    {
        static std::atomic<int> n{ 0 };
        path = fs::temp_directory_path() /
               (L"settings-taskbar-tests-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++n));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

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

// `shell.exe -config:tree <root> -context:taskbar`: the JSON it writes.
std::string taskbarTree(const fs::path &root)
{
    auto out = root.parent_path() / L"tree.json";
    std::wstring cmd = L"\"" + shellExe().wstring() + L"\" -silent -config:tree \"" + root.wstring() +
                       L"\" -context:taskbar \"-out:" + out.wstring() + L"\"";
    STARTUPINFOW si{ sizeof si };
    PROCESS_INFORMATION pi{};
    REQUIRE(CreateProcessW(shellExe().c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                           nullptr, &si, &pi));
    WaitForSingleObject(pi.hProcess, 30000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    std::string bytes;
    nss::readFile(out, bytes);
    fs::remove(out);
    return nss::decode(bytes).text;
}

const char *kHandWritten =
    "// my taskbar apps\r\n"
    "\r\n"
    "// the editor I use\r\n"
    "item(title='Code' image cmd='@sys.prog\\VS Code\\Code.exe' keys='C')\r\n"
    "// item(title=\"Old tool\" image cmd='C:\\tools\\old.exe' args='-x')\r\n"
    "sep\r\n"
    "menu(title='More' image=\\uE0F3)\r\n"
    "{\r\n"
    "\titem(title='Inner' cmd='inner.exe')\r\n"
    "}\r\n"
    "$x = 1\r\n"
    "item(title=loc.paint image=\\uE116 cmd='mspaint')\r\n"
    "item(title='Dynamic' cmd=sys.dir + '\\x.exe')\r\n"
    "\r\n"
    "// the end\r\n";

} // namespace

TEST_CASE("taskbar apps: the shipped four")
{
    auto list = defaults(fakePaths());
    CHECK(!list.exists);
    REQUIRE(list.entries.size() == 4);
    CHECK(builtinOf(list.entries[0]) == "paint");
    CHECK(builtinOf(list.entries[1]) == "edge");
    CHECK(builtinOf(list.entries[2]) == "calculator");
    CHECK(builtinOf(list.entries[3]) == "regedit");
    for (const auto &e : list.entries) {
        CHECK(e.kind == Kind::App);
        CHECK(e.enabled);
        CHECK(!e.edited());
        CHECK(!e.titleExpr.empty());
    }
    CHECK(list.entries[1].program == "C:\\PF86\\Microsoft\\Edge\\Application\\msedge.exe");
    CHECK(displayTitle(list.entries[0], { { "paint", "Paint (loc)" } }) == "Paint (loc)");
    CHECK(displayTitle(list.entries[2]) == "Calculator");
    CHECK(displayTitle(list.entries[3]) == "Registry Editor");

    // Saved untouched: the header, then the four exactly as taskbar.nss has them.
    auto text = generate(list, fakePaths());
    CHECK(text.rfind(std::string(kHeader[0]) + "\r\n" + std::string(kHeader[1]) + "\r\n\r\n", 0) == 0);
    CHECK(text.find("item(title=loc.paint image=\\uE116 cmd='mspaint')\r\n") != std::string::npos);
    CHECK(text.find("item(where=path.exists(sys.prog32 + '\\Microsoft\\Edge\\Application\\msedge.exe') title=loc.edge image "
                    "cmd='@sys.prog32\\Microsoft\\Edge\\Application\\msedge.exe')\r\n") != std::string::npos);
    CHECK(!dirty(list, defaults(fakePaths()), fakePaths()));

    // ... and it reads back as the same list.
    auto back = parse(text, fakePaths());
    CHECK(back.exists);
    REQUIRE(back.entries.size() == 4);
    CHECK(generate(back, fakePaths()) == text);
    CHECK(back.head.size() == 3); // the header and its blank line
}

TEST_CASE("taskbar apps: a hand-written file reads and writes back unchanged")
{
    auto list = parse(kHandWritten, fakePaths());
    REQUIRE(list.error.empty());
    CHECK(generate(list, fakePaths()) == kHandWritten);
    REQUIRE(list.entries.size() == 7);
    CHECK(list.head == std::vector<std::string>{ "// my taskbar apps", "" });

    const auto &code = list.entries[0];
    CHECK(code.kind == Kind::App);
    CHECK(code.title == "Code");
    CHECK(code.program == "C:\\PF\\VS Code\\Code.exe");
    CHECK(code.lead == std::vector<std::string>{ "// the editor I use" });
    CHECK(code.line == 4);

    const auto &old = list.entries[1];
    CHECK(old.kind == Kind::App);
    CHECK(!old.enabled);
    CHECK(old.title == "Old tool");
    CHECK(old.args == "-x");

    CHECK(list.entries[2].kind == Kind::Separator);
    CHECK(list.entries[3].kind == Kind::Other); // the submenu
    CHECK(list.entries[3].text.size() == 4);
    CHECK(list.entries[4].kind == Kind::Other); // $x = 1
    CHECK(list.entries[5].kind == Kind::App);
    CHECK(list.entries[5].titleExpr == "loc.paint");
    CHECK(list.entries[6].kind == Kind::Other); // cmd is an expression
    CHECK(list.tail == std::vector<std::string>{ "", "// the end" });
}

TEST_CASE("taskbar apps: edits change only what was edited")
{
    auto paths = fakePaths();
    auto list = parse(kHandWritten, paths);
    REQUIRE(list.error.empty());

    // Rename: the title's value is replaced, keys= stays.
    list.entries[0].title = "VS Code";
    auto text = generate(list, paths);
    CHECK(text.find("item(title=\"VS Code\" image cmd='@sys.prog\\VS Code\\Code.exe' keys='C')\r\n") != std::string::npos);

    // Switch one off and on again: commented out, then back as it was.
    list.entries[5].enabled = false;
    text = generate(list, paths);
    CHECK(text.find("// item(title=loc.paint image=\\uE116 cmd='mspaint')\r\n") != std::string::npos);
    auto reread = parse(text, paths);
    REQUIRE(reread.entries.size() == 7);
    CHECK(!reread.entries[5].enabled);
    reread.entries[5].enabled = true;
    CHECK(generate(reread, paths).find("\r\nitem(title=loc.paint image=\\uE116 cmd='mspaint')\r\n") != std::string::npos);

    // The switched-off one, switched on with new arguments.
    list = parse(kHandWritten, paths);
    list.entries[1].enabled = true;
    list.entries[1].args = "";
    list.entries[1].admin = true;
    text = generate(list, paths);
    CHECK(text.find("\r\nitem(title=\"Old tool\" image cmd='C:\\tools\\old.exe' admin)\r\n") != std::string::npos);
    list.entries[1].args = "--all files";
    list.entries[1].program = "C:\\PF86\\Tools\\new.exe";
    text = generate(list, paths);
    CHECK(text.find("item(title=\"Old tool\" image cmd='@sys.prog32\\Tools\\new.exe' args='--all files' admin)") !=
          std::string::npos);

    // Moving an entry takes its comments; the file's own header stays on top.
    list = parse(kHandWritten, paths);
    std::swap(list.entries[0], list.entries[2]);
    text = generate(list, paths);
    CHECK(text.rfind("// my taskbar apps\r\n\r\nsep\r\n", 0) == 0);
    CHECK(text.find("// the editor I use\r\nitem(title='Code'") != std::string::npos);

    // Removing one removes its comments too; the rest stays.
    list = parse(kHandWritten, paths);
    list.entries.erase(list.entries.begin());
    text = generate(list, paths);
    CHECK(text.find("the editor I use") == std::string::npos);
    CHECK(text.find("menu(title='More'") != std::string::npos);
    CHECK(text.find("$x = 1") != std::string::npos);
}

TEST_CASE("taskbar apps: new apps")
{
    auto paths = fakePaths();
    auto list = defaults(paths);
    list.entries.erase(list.entries.begin() + 1); // no Edge
    list.entries.push_back(makeSeparator());
    list.entries.push_back(makeApp("Notepad++", "C:\\PF\\Notepad++\\notepad++.exe", "-multiInst"));
    list.entries.push_back(makeApp("It's \"mine\"", "C:\\x\\it's.lnk", "", "\"C:\\\\x\\\\app.exe\""));
    list.entries.back().admin = true;
    auto text = generate(list, paths);
    CHECK(text.find("item(title=loc.paint image=\\uE116 cmd='mspaint')\r\n"
                    "item(title=loc.calculator image=\\ue1e7 cmd='calc.exe')\r\n"
                    "item(title=str.res('regedit.exe,-16') image cmd='regedit.exe')\r\n"
                    "sep\r\n"
                    "item(title=\"Notepad++\" image cmd='@sys.prog\\Notepad++\\notepad++.exe' args='-multiInst')\r\n"
                    "item(title=\"It's \\\"mine\\\"\" image=\"C:\\\\x\\\\app.exe\" cmd=`C:\\x\\it's.lnk` admin)\r\n") !=
          std::string::npos);
    CHECK(dirty(list, defaults(paths), paths));

    auto back = parse(text, paths);
    REQUIRE(back.entries.size() == 6);
    CHECK(back.entries[4].title == "Notepad++");
    CHECK(back.entries[4].program == "C:\\PF\\Notepad++\\notepad++.exe");
    CHECK(back.entries[4].args == "-multiInst");
    CHECK(back.entries[5].title == "It's \"mine\"");
    CHECK(back.entries[5].program == "C:\\x\\it's.lnk");
    CHECK(back.entries[5].admin);
    CHECK(generate(back, paths) == text);

    // An empty list is just the header.
    List none;
    CHECK(generate(none, paths) == std::string(kHeader[0]) + "\r\n" + std::string(kHeader[1]) + "\r\n\r\n");
    CHECK(parse(generate(none, paths), paths).entries.empty());
}

TEST_CASE("taskbar apps: a file that doesn't parse is reported, not read")
{
    auto list = parse("item(title='x' cmd='a.exe'\r\n", fakePaths());
    CHECK(!list.error.empty());
    CHECK(list.errorLine == 1);
    CHECK(list.entries.empty());
}

TEST_CASE("taskbar apps: hand edit survives an app save")
{
    auto paths = fakePaths();
    // The app wrote the file...
    auto list = defaults(paths);
    list.entries.push_back(makeApp("Notepad++", "C:\\PF\\Notepad++\\notepad++.exe"));
    auto saved = generate(list, paths);

    // ... the user edited it by hand: a comment, a property the page doesn't know,
    // a new title for Calculator, and a submenu of their own.
    auto hand = saved;
    auto replace = [&](std::string_view from, std::string_view to) {
        auto at = hand.find(from);
        REQUIRE(at != std::string::npos);
        hand.replace(at, from.size(), to);
    };
    replace("item(title=loc.calculator image=\\ue1e7 cmd='calc.exe')",
            "// sums\r\nitem(title='Sums' image=\\ue1e7 cmd='calc.exe' tip='Calculator')");
    replace("item(title=\"Notepad++\"", "item(keys='N' title=\"Notepad++\"");
    hand += "menu(title='Mine') { item(title='a' cmd='a.exe') }\r\n";

    // The page reads the edit back...
    auto read = parse(hand, paths);
    REQUIRE(read.error.empty());
    REQUIRE(read.entries.size() == 6);
    CHECK(read.entries[2].title == "Sums");
    CHECK(read.entries[2].lead == std::vector<std::string>{ "// sums" });

    // ... and its next save (Paint renamed, Edge switched off) keeps it.
    read.entries[0].title = "Paint it"; // as the page does: a typed name replaces loc.paint
    read.entries[0].titleExpr.clear();
    read.entries[1].enabled = false;
    auto next = generate(read, paths);
    CHECK(next.find("// sums\r\nitem(title='Sums' image=\\ue1e7 cmd='calc.exe' tip='Calculator')\r\n") != std::string::npos);
    CHECK(next.find("item(keys='N' title=\"Notepad++\" image cmd='@sys.prog\\Notepad++\\notepad++.exe')") != std::string::npos);
    CHECK(next.find("menu(title='Mine') { item(title='a' cmd='a.exe') }\r\n") != std::string::npos);
    CHECK(next.find("item(title=\"Paint it\" image=\\uE116 cmd='mspaint')") != std::string::npos);
    CHECK(next.find("// item(where=path.exists(") != std::string::npos);
}

TEST_CASE("modules: adding and removing your own files")
{
    auto root = nss::NssDocument::fromText("import 'imports/theme.nss'\r\nimport 'imports/goto.nss'\r\n"
                                           "import 'imports/settings-app-menus.nss'\r\nimport 'imports/settings-app.nss'\r\n");
    CHECK(rules::addModule(root, "imports/mine.nss"));
    // Before the app's own files, so settings-app.nss stays last.
    CHECK(root.text() == "import 'imports/theme.nss'\r\nimport 'imports/goto.nss'\r\nimport 'imports/mine.nss'\r\n"
                         "import 'imports/settings-app-menus.nss'\r\nimport 'imports/settings-app.nss'\r\n");
    CHECK(!rules::addModule(root, "imports/MINE.nss")); // already there
    CHECK(rules::hasModule(root, "imports\\mine.nss"));

    auto mods = rules::listModules(root);
    REQUIRE(mods.size() == 4);
    CHECK(!mods[0].own);
    CHECK(mods[2].own);
    CHECK(mods[2].title == "Mine");
    CHECK(!mods[3].own); // the Menu Builder's file
    CHECK(rules::removeModule(root, mods[2].importIndex));
    CHECK(root.text() == "import 'imports/theme.nss'\r\nimport 'imports/goto.nss'\r\n"
                         "import 'imports/settings-app-menus.nss'\r\nimport 'imports/settings-app.nss'\r\n");

    // A root without the app's files: at the end.
    auto plain = nss::NssDocument::fromText("settings { priority = 1 }\r\nimport 'imports/theme.nss'");
    CHECK(rules::addModule(plain, "extra/x y.nss"));
    CHECK(plain.text() == "settings { priority = 1 }\r\nimport 'imports/theme.nss'\r\nimport 'extra/x y.nss'");

    // A turned-off one can go too.
    auto off = nss::NssDocument::fromText("import 'imports/theme.nss'\r\n// import 'imports/mine.nss'\r\n");
    auto list = rules::listModules(off);
    REQUIRE(list.size() == 2);
    CHECK(list[1].own);
    CHECK(rules::removeModule(off, list[1].importIndex));
    CHECK(off.text() == "import 'imports/theme.nss'\r\n");
}

TEST_CASE("taskbar apps: the taskbar menu shows the list (real shell.exe)")
{
    if (!haveShell()) {
        MESSAGE("shell.exe/shell.dll not in src/bin; skipped");
        return;
    }
    TempDir app;
    auto root = seedConfig(app.path);
    auto rootDir = root.parent_path();
    auto check = nss::makeShellCheckValidator(shellExe());
    REQUIRE(check(root).ok);

    // Without the file: the four shipped apps, and no warning about it.
    auto tree = taskbarTree(root);
    CHECK(tree.find("\"ok\": true") != std::string::npos);
    CHECK(tree.find("settings-app-taskbar") == std::string::npos);
    CHECK(tree.find("\"source\":\"loc.paint\"") != std::string::npos);

    // The page's save: through ConfigStore with the validator and a snapshot.
    auto paths = realPaths();
    auto list = defaults(paths);
    list.entries.erase(list.entries.begin()); // no Paint
    list.entries.push_back(makeSeparator());
    list.entries.push_back(makeApp("My Notepad", "notepad.exe", "C:\\notes.txt"));
    auto file = rootDir / L"imports" / L"settings-app-taskbar.nss";
    nss::ConfigStore store(root);
    nss::SaveOptions options;
    options.reason = "Context Menus: taskbar apps test";
    options.validator = check;
    auto saved = store.save({ { file, generate(list, paths), nss::defaultFormat() } }, options);
    REQUIRE_MESSAGE(saved.ok, (saved.error + " " + saved.check.message));
    CHECK(!saved.snapshotId.empty());

    tree = taskbarTree(root);
    CHECK(tree.find("\"ok\": true") != std::string::npos);
    CHECK(tree.find("\"title\":\"My Notepad\"") != std::string::npos);
    CHECK(tree.find("settings-app-taskbar.nss") != std::string::npos);

    // The file as written reads back as the same list.
    std::string bytes;
    nss::readFile(file, bytes);
    auto back = parse(nss::decode(bytes).text, paths);
    REQUIRE(back.entries.size() == 5);
    CHECK(back.entries[4].title == "My Notepad");
    CHECK(back.entries[4].args == "C:\\notes.txt");

    // A broken file is refused by the check and nothing is written.
    auto refused = store.save({ { file, "item(title='x' cmd=)\r\n", nss::defaultFormat() } }, options);
    CHECK(!refused.ok);
    nss::readFile(file, bytes);
    CHECK(parse(nss::decode(bytes).text, paths).entries.size() == 5);
}
