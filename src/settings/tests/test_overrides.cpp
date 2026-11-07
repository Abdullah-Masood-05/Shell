// ShellOverrides.h: the Menu map's changes to Shell's own items in the files that came
// with Shell, kept as override(...) lines in imports/settings-app-map.nss, and what
// the real shell.exe makes of them.

#include "doctest.h"

#include "MapRules.h"
#include "MenuMap.h"
#include "ShellOverrides.h"

#include "nss/Literal.h"
#include "nss/NssDocument.h"
#include "nss/Text.h"

#include "../../shared/ConfigUpgrade.h"

#include <filesystem>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;

namespace {

fs::path binDir() { return fs::path(SETTINGS_SOURCE_BIN).lexically_normal(); }
bool haveShell()
{
    return fs::is_regular_file(binDir() / L"shell.exe") && fs::is_regular_file(binDir() / L"shell.dll") &&
           fs::is_regular_file(binDir() / L"shell.nss");
}

struct Scratch
{
    fs::path path;
    Scratch()
    {
        static int n = 0;
        path = fs::temp_directory_path() /
               (L"settings-overrides-tests-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++n));
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
    std::string read(const fs::path &rel) const
    {
        std::ifstream in(path / rel, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
};

// Runs shell.exe -silent with `args`; the JSON it writes to -out.
std::string runShell(const std::wstring &args, const fs::path &out)
{
    std::wstring cmd = L"\"" + (binDir() / L"shell.exe").wstring() + L"\" -silent " + args + L" \"-out:" + out.wstring() + L"\"";
    STARTUPINFOW si{ sizeof si };
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return {};
    WaitForSingleObject(pi.hProcess, 60000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    std::ifstream in(out, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

const menumap::Node *findNode(const std::vector<menumap::Node> &list, const std::string &title)
{
    for (const auto &n : list) {
        if (n.title == title)
            return &n;
        if (auto *c = findNode(n.children, title))
            return c;
    }
    return nullptr;
}

const char *kGoto = "menu(type='*' title=title.go_to image=\\uE14A)\r\n"
                    "{\r\n"
                    "\tmenu(title=loc.personalization image=inherit)\r\n"
                    "\t{\r\n"
                    "\t\titem(title=loc.personalization cmd='ms-settings:personalization')\r\n"
                    "\t\titem(title = 'Lock  screen' // a comment\r\n"
                    "\t\t\tcmd='ms-settings:lockscreen')\r\n"
                    "\t}\r\n"
                    "\tseparator\r\n"
                    "\titem(image=icon.copy cmd='x')\r\n"
                    "}\r\n";

} // namespace

TEST_CASE("overrides: sourceKey compares titles as written")
{
    using overrides::sourceKey;
    CHECK(sourceKey("loc.personalization") == "loc.personalization");
    CHECK(sourceKey("  loc.x  // note\r\n  ") == "loc.x");
    CHECK(sourceKey("'Lock  screen'") == "'Lock  screen'");               // blanks in a string count
    CHECK(sourceKey("str.replace(a,\r\n\t  'b')") == "str.replace(a, 'b')"); // outside they don't
    CHECK(sourceKey("\"a \\\" b\" /* c */ + d") == "\"a \\\" b\" + d");
}

TEST_CASE("overrides: a line, read and written")
{
    auto o = overrides::parse("override(file='imports/goto.nss' item=loc.personalization index=2 vis=vis.remove title='Mine')");
    REQUIRE(o);
    CHECK(o->key.file == "imports/goto.nss");
    CHECK(o->key.item == "loc.personalization");
    CHECK(o->key.index == 2);
    REQUIRE(o->find("vis"));
    CHECK(*o->find("vis") == "vis.remove");
    CHECK(*o->find("title") == "'Mine'");
    CHECK(overrides::format(*o) ==
          "override(file='imports/goto.nss' item=loc.personalization index=2 vis=vis.remove title='Mine')");

    // Aliases read as their canonical names; another rule, a comment, or a line turned
    // off is not one.
    auto alias = overrides::parse("override(file='imports\\x.nss' item='A' visibility=vis.remove icon=icon.copy)");
    REQUIRE(alias);
    CHECK(alias->key.file == "imports/x.nss");
    CHECK(alias->find("vis"));
    CHECK(alias->find("image"));
    CHECK_FALSE(overrides::parse("// override(file='imports/x.nss' item='A' vis=vis.remove)"));
    CHECK_FALSE(overrides::parse("remove(find='\"A\"')"));
    CHECK_FALSE(overrides::parse("override(item='A' vis=vis.remove)"));
    // NssDocument reads it as a declaration without complaint (the map file's check).
    auto doc = nss::NssDocument::fromText("override(file='imports/x.nss' item='A' vis=vis.remove)\r\n");
    CHECK(doc.diagnostics().empty());
}

TEST_CASE("overrides: an item is named by its file, its title as written and which one it is")
{
    auto doc = nss::NssDocument::fromText(kGoto);
    REQUIRE(doc.diagnostics().empty());
    auto decls = overrides::declarations(doc);
    REQUIRE(decls.size() == 5); // the separator isn't one

    auto menuKey = overrides::keyOf(doc, "imports\\goto.nss", *decls[1]);
    auto itemKey = overrides::keyOf(doc, "imports/goto.nss", *decls[2]);
    REQUIRE(menuKey);
    REQUIRE(itemKey);
    CHECK(menuKey->file == "imports/goto.nss");
    CHECK(menuKey->item == "loc.personalization");
    CHECK(menuKey->index == 1); // a menu before the items in it, as shell.dll reads them
    CHECK(itemKey->index == 2);
    CHECK(overrides::find(doc, *itemKey) == decls[2]);

    auto lock = overrides::keyOf(doc, "imports/goto.nss", *decls[3]);
    REQUIRE(lock);
    CHECK(lock->item == "'Lock  screen'");
    CHECK_FALSE(overrides::keyOf(doc, "imports/goto.nss", *decls[4])); // no title=

    // By line, as the Menu map has it.
    const auto *at = menumap::findDeclaration(doc, 5, 0);
    REQUIRE(at);
    CHECK(overrides::keyOf(doc, "imports/goto.nss", *at)->index == 2);
}

TEST_CASE("overrides: set() keeps one line per item")
{
    overrides::Key key{ "imports/terminal.nss", "title.command_prompt", 1 };
    maprules::Rules lines{ "remove(find='\"Cast to Device\"')", "// a note of the user's" };

    CHECK(overrides::set(lines, key, "vis", std::string("vis.remove")));
    REQUIRE(lines.size() == 3);
    CHECK(lines[2] == "override(file='imports/terminal.nss' item=title.command_prompt vis=vis.remove)");
    CHECK(overrides::get(lines, key, "vis") == std::optional<std::string>("vis.remove"));

    CHECK(overrides::set(lines, key, "title", std::string("'Cmd'")));
    REQUIRE(lines.size() == 3);
    CHECK(lines[2] == "override(file='imports/terminal.nss' item=title.command_prompt vis=vis.remove title='Cmd')");
    CHECK_FALSE(overrides::set(lines, key, "title", std::string("'Cmd'"))); // no change

    // Another item, and the same file named with a backslash and in another case.
    overrides::Key other{ "IMPORTS\\Terminal.nss", "title.command_prompt", 1 };
    CHECK(other == key);
    CHECK_FALSE(overrides::Key{ "imports/terminal.nss", "title.command_prompt", 2 } == key);

    CHECK(overrides::set(lines, key, "vis", std::nullopt));
    CHECK(lines[2] == "override(file='imports/terminal.nss' item=title.command_prompt title='Cmd')");
    CHECK(overrides::set(lines, key, "title", std::nullopt));
    REQUIRE(lines.size() == 2); // no property left: the line goes
    CHECK(lines[0] == "remove(find='\"Cast to Device\"')");
    CHECK_FALSE(overrides::has(lines, key));

    // A second line for the item (written by hand) loses the property the first gets.
    lines = { "override(file='imports/terminal.nss' item=title.command_prompt   vis=vis.remove)",
              "override(file='imports/terminal.nss' item=title.command_prompt vis=vis.remove tip='x')" };
    CHECK(overrides::set(lines, key, "image", std::string("null")));
    REQUIRE(lines.size() == 2);
    CHECK(lines[0] == "override(file='imports/terminal.nss' item=title.command_prompt vis=vis.remove image=null)");
    CHECK(lines[1] == "override(file='imports/terminal.nss' item=title.command_prompt vis=vis.remove tip='x')");
    CHECK(overrides::set(lines, key, "vis", std::string("vis.disable")));
    CHECK(lines[1] == "override(file='imports/terminal.nss' item=title.command_prompt tip='x')");
}

TEST_CASE("overrides: which files came with Shell")
{
    CHECK(overrides::isShipped("imports/terminal.nss"));
    CHECK(overrides::isShipped("imports\\goto.nss"));
    CHECK(overrides::isShipped("shell.nss"));
    CHECK_FALSE(overrides::isShipped("imports/kaser.nss"));
    CHECK_FALSE(overrides::isShipped("imports/settings-app-map.nss"));
    CHECK_FALSE(overrides::isShipped("imports/snippets/copy-path.nss"));
}

TEST_CASE("overrides: planMapWrite writes the map file and the root's import of it")
{
    Scratch s;
    s.write("shell.nss", "settings { priority=1 }\r\nimport 'imports/terminal.nss'\r\n");
    s.write("imports/terminal.nss", "item(title='A' cmd='a')\r\n");
    overrides::Key key{ "imports/terminal.nss", "'A'", 1 };
    auto plan = overrides::planMapWrite(s.path / L"shell.nss", [&](maprules::Rules &lines, std::string &) {
        overrides::set(lines, key, "vis", std::string("vis.remove"));
        return true;
    });
    REQUIRE(plan.ok);
    REQUIRE(plan.writes.size() == 2);
    CHECK(plan.writes[0].path.filename() == L"settings-app-map.nss");
    CHECK(plan.writes[0].text.find("override(file='imports/terminal.nss' item='A' vis=vis.remove)") != std::string::npos);
    CHECK(plan.writes[1].path.filename() == L"shell.nss");
    CHECK(plan.writes[1].text.find("import 'imports/settings-app-map.nss'\r\nimport 'imports/terminal.nss'") != std::string::npos);

    // Nothing to change: nothing written.
    auto none = overrides::planMapWrite(s.path / L"shell.nss", [](maprules::Rules &, std::string &) { return true; });
    CHECK(none.ok);
    CHECK(none.writes.empty());
}

TEST_CASE("overrides: shell.exe applies them, and the shipped files stay as shipped (real shell.exe)")
{
    if (!haveShell()) {
        MESSAGE("shell.exe/shell.dll/shell.nss not in src/bin; skipped");
        return;
    }
    Scratch s;
    fs::copy_file(binDir() / L"shell.nss", s.path / L"shell.nss");
    fs::copy(binDir() / L"imports", s.path / L"imports", fs::copy_options::recursive);
    const auto terminalBefore = s.read("imports/terminal.nss");
    const auto gotoBefore = s.read("imports/goto.nss");
    const auto root = s.path / L"shell.nss";

    // As the Menu map does it: the key from the item's declaration.
    auto keyFor = [&](const char *rel, const char *title) {
        auto doc = nss::NssDocument::load(s.path / fs::path(rel));
        REQUIRE(doc);
        for (const auto *d : overrides::declarations(*doc))
            if (overrides::titleText(*doc, *d) == title)
                return *overrides::keyOf(*doc, rel, *d);
        FAIL("no declaration titled ", title);
        return overrides::Key{};
    };
    auto prompt = keyFor("imports/terminal.nss", "title.command_prompt");
    auto terminal = keyFor("imports/terminal.nss", "title.terminal");
    auto windows = keyFor("imports/goto.nss", "loc.goto_windows");
    auto plan = overrides::planMapWrite(root, [&](maprules::Rules &lines, std::string &) {
        overrides::set(lines, prompt, "vis", std::string("vis.remove"));
        overrides::set(lines, terminal, "title", nss::quoteString("My terminal"));
        overrides::set(lines, terminal, "image", std::string("null"));
        overrides::set(lines, windows, "menu", std::string("title.go_to"));
        overrides::set(lines, windows, "pos", nss::quoteString("top"));
        return true;
    });
    REQUIRE(plan.ok);
    for (const auto &w : plan.writes) {
        auto bytes = nss::encode(w.text, w.format.value_or(nss::defaultFormat()));
        std::ofstream(w.path, std::ios::binary | std::ios::trunc).write(bytes.data(), std::streamsize(bytes.size()));
    }

    auto check = runShell(L"-config:check \"" + root.wstring() + L"\"", s.path / L"check.json");
    CHECK(check.find("\"ok\": true") != std::string::npos);
    CHECK(check.find("override: no item") == std::string::npos);

    auto tree = menumap::parseTree(runShell(L"-config:tree \"" + root.wstring() + L"\" -context:back", s.path / L"tree.json"));
    REQUIRE(tree);
    REQUIRE(tree->ok);
    const auto *renamed = findNode(tree->shell, "My terminal");
    REQUIRE(renamed);
    CHECK(renamed->file.filename() == L"terminal.nss"); // still its own declaration
    const auto *cmd = findNode(tree->shell, "Command Prompt");
    REQUIRE(cmd);
    CHECK(cmd->vis == "hidden");
    const auto *goTo = findNode(tree->shell, "Go To");
    REQUIRE(goTo);
    const menumap::Node *movedWindows = nullptr;
    for (const auto &c : goTo->children)
        if (c.title == "Windows")
            movedWindows = &c;
    REQUIRE(movedWindows);
    CHECK(movedWindows->relocated);

    // The files that came with Shell are byte for byte as shipped: updates replace them.
    CHECK(s.read("imports/terminal.nss") == terminalBefore);
    CHECK(s.read("imports/goto.nss") == gotoBefore);
    auto shipped = config_upgrade::run(binDir().wstring(), s.path.wstring(), { L"shell.nss", {}, true, true });
    CHECK(shipped.ok);
    CHECK(shipped.modified_kept.empty());

    // An override that names nothing: a warning, the config still loads.
    s.write("imports/settings-app-map.nss", "override(file='imports/terminal.nss' item=loc.nothing vis=vis.remove)\r\n");
    check = runShell(L"-config:check \"" + root.wstring() + L"\"", s.path / L"check.json");
    CHECK(check.find("\"ok\": true") != std::string::npos);
    CHECK(check.find("override: no item") != std::string::npos);
}
