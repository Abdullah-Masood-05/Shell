// CarryOver.h and config_update::carryOver(): files that came with Shell which the
// Menu map changed in place before 2.3.0-beta.6 (kaser4's case) are brought to the new
// version with those changes kept as override lines in imports/settings-app-map.nss.

#include "doctest.h"

#include "CarryOver.h"
#include "ConfigUpdate.h"
#include "MapRules.h"
#include "MenuMap.h"
#include "ShellOverrides.h"

#include "nss/ConfigStore.h"
#include "nss/NssDocument.h"
#include "nss/Text.h"

#include "../../shared/ConfigUpgrade.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;

namespace {

fs::path binDir() { return fs::path(SETTINGS_SOURCE_BIN).lexically_normal(); }
bool haveShipped() { return fs::is_regular_file(binDir() / L"shell.nss") && fs::is_directory(binDir() / L"imports"); }
bool haveShell() { return haveShipped() && fs::is_regular_file(binDir() / L"shell.exe") && fs::is_regular_file(binDir() / L"shell.dll"); }

std::string readBytes(const fs::path &p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void writeBytes(const fs::path &p, std::string_view bytes)
{
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary | std::ios::trunc).write(bytes.data(), std::streamsize(bytes.size()));
}

struct Scratch
{
    fs::path path;
    Scratch()
    {
        static int n = 0;
        path = fs::temp_directory_path() /
               (L"settings-carryover-tests-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++n));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~Scratch()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

// The shipped file with the declaration titled `title` changed by `edit`, as the Menu
// map of 2.3.0-beta.5 changed it, in the file's own encoding.
std::string editedShipped(const std::string &rel, const std::string &title,
                          const std::function<bool(nss::NssDocument &, size_t, size_t)> &edit, std::string bytes = {})
{
    if (bytes.empty())
        bytes = readBytes(binDir() / fs::path(rel));
    auto doc = nss::NssDocument::fromBytes(bytes);
    REQUIRE(doc);
    const nss::Node *target = nullptr;
    for (const auto *d : overrides::declarations(*doc))
        if (overrides::titleText(*doc, *d) == title) {
            target = d;
            break;
        }
    REQUIRE_MESSAGE(target, "no declaration titled ", title, " in ", rel);
    REQUIRE(edit(*doc, target->line, 0));
    return doc->serialize();
}

auto hide = [](nss::NssDocument &doc, size_t line, size_t column) {
    return menumap::setShellItemHidden(doc, line, column, true);
};

const overrides::Override *overrideFor(const carryover::Result &r, const std::string &item)
{
    for (const auto &o : r.overrides)
        if (o.key.item == item)
            return &o;
    return nullptr;
}

} // namespace

TEST_CASE("carry over: a hidden item in a shipped file becomes an override line")
{
    if (!haveShipped()) {
        MESSAGE("src/bin has no shipped config; skipped");
        return;
    }
    const auto shipped = readBytes(binDir() / L"imports" / L"terminal.nss");
    const auto user = editedShipped("imports/terminal.nss", "title.command_prompt", hide);
    REQUIRE(user != shipped);

    auto r = carryover::carry("imports/terminal.nss", user, shipped, {});
    CHECK(r.complete);
    CHECK(r.carried == 1);
    CHECK(r.text == nss::decode(shipped).text); // the new version as it is
    REQUIRE(r.overrides.size() == 1);
    CHECK(overrides::format(r.overrides[0]) == "override(file='imports/terminal.nss' item=title.command_prompt vis=vis.remove)");
}

TEST_CASE("carry over: renamed, new icon, moved and placed")
{
    if (!haveShipped()) {
        MESSAGE("src/bin has no shipped config; skipped");
        return;
    }
    const auto shipped = readBytes(binDir() / L"imports" / L"goto.nss");
    auto user = editedShipped("imports/goto.nss", "loc.goto_windows", [](nss::NssDocument &doc, size_t line, size_t column) {
        return menumap::setShellItemTitle(doc, line, column, "Win folder") &&
               menumap::setShellItemProp(doc, line, column, "image", "icon.copy") &&
               menumap::setShellItemProp(doc, line, column, "menu", "title.go_to") &&
               menumap::setShellItemProp(doc, line, column, "pos", "'top'");
    });
    // The second Personalization (the item inside the menu of that name) hidden too.
    user = editedShipped("imports/goto.nss", "loc.personalization", hide, user);

    auto r = carryover::carry("imports/goto.nss", user, shipped, {});
    CHECK(r.complete);
    CHECK(r.carried == 5);
    const auto *win = overrideFor(r, "loc.goto_windows");
    REQUIRE(win);
    CHECK(*win->find("title") == "\"Win folder\"");
    CHECK(*win->find("image") == "icon.copy");
    CHECK(*win->find("menu") == "title.go_to");
    CHECK(*win->find("pos") == "'top'");
    const auto *pers = overrideFor(r, "loc.personalization");
    REQUIRE(pers);
    CHECK(pers->key.index == 1); // the menu: editedShipped() picks the first one
    CHECK(*pers->find("vis") == "vis.remove");

    // An icon taken away (image= removed from the line): image=null.
    auto noIcon = editedShipped("imports/goto.nss", "title.control_panel", [](nss::NssDocument &doc, size_t line, size_t column) {
        return menumap::setShellItemProp(doc, line, column, "image", "");
    });
    auto r2 = carryover::carry("imports/goto.nss", noIcon, shipped, {});
    CHECK(r2.complete);
    REQUIRE(overrideFor(r2, "title.control_panel"));
    CHECK(*overrideFor(r2, "title.control_panel")->find("image") == "null");
}

TEST_CASE("carry over: other edits can't be carried, but Use the new version keeps the Menu map's")
{
    if (!haveShipped()) {
        MESSAGE("src/bin has no shipped config; skipped");
        return;
    }
    const auto shipped = readBytes(binDir() / L"imports" / L"terminal.nss");
    auto user = editedShipped("imports/terminal.nss", "title.command_prompt", hide);
    auto text = nss::decode(user).text;
    // A command changed by hand.
    auto at = text.find("cmd=\"wt.exe\"");
    REQUIRE(at != std::string::npos);
    text.replace(at, 12, "cmd=\"wtx.exe\"");
    auto r = carryover::carry("imports/terminal.nss", text, shipped, {});
    CHECK_FALSE(r.complete);
    CHECK(r.overrides.empty());

    auto partial = carryover::carry("imports/terminal.nss", text, shipped, {}, true);
    CHECK_FALSE(partial.complete);
    CHECK(partial.text == nss::decode(shipped).text);
    REQUIRE(overrideFor(partial, "title.command_prompt"));

    // A new comment is the user's too: kept as it is, not carried.
    auto commented = "// mine\r\n" + nss::decode(user).text;
    CHECK_FALSE(carryover::carry("imports/terminal.nss", commented, shipped, {}).complete);
}

TEST_CASE("carry over: shell.nss keeps the user's import lines and the app's")
{
    if (!haveShipped()) {
        MESSAGE("src/bin has no shipped config; skipped");
        return;
    }
    const auto shipped = readBytes(binDir() / L"shell.nss");
    auto text = nss::decode(editedShipped("shell.nss", "loc.pin_unpin", hide)).text;
    // goto.nss turned off, the user's own file imported last, the app's files imported.
    auto at = text.find("import 'imports/goto.nss'");
    REQUIRE(at != std::string::npos);
    text.insert(at, "// ");
    text += "import 'imports/kaser.nss'\r\nimport 'imports/settings-app.nss'\r\n";
    auto first = text.find("import 'imports/theme.nss'");
    text.insert(first, "import 'imports/settings-app-map.nss'\r\n");

    auto r = carryover::carry("shell.nss", text, shipped, {});
    CHECK(r.complete);
    CHECK(r.carried == 3); // the hidden item, goto.nss off, kaser.nss on
    CHECK(r.text.find("// import 'imports/goto.nss'") != std::string::npos);
    CHECK(r.text.find("import 'imports/kaser.nss'") != std::string::npos);
    CHECK(r.text.find("import 'imports/settings-app.nss'") != std::string::npos);
    // The map file before the first config import, as the app writes it.
    CHECK(r.text.find("import 'imports/settings-app-map.nss'") < r.text.find("import 'imports/theme.nss'"));
    REQUIRE(overrideFor(r, "loc.pin_unpin"));
    CHECK(overrideFor(r, "loc.pin_unpin")->key.file == "shell.nss");
    // The rest is the new version: the same file once the user's lines are taken out again.
    // His import right before the app's settings-app.nss, as in his copy.
    CHECK(r.text.find("import 'imports/kaser.nss'\r\nimport 'imports/settings-app.nss'") != std::string::npos);
    auto back = r.text;
    back.erase(back.find("import 'imports/kaser.nss'\r\n"), 28);
    back.erase(back.find("// import 'imports/goto.nss'"), 3);
    CHECK(config_upgrade::fingerprint(back) == config_upgrade::fingerprint(shipped));
}

TEST_CASE("carry over: a copy that started from an earlier release")
{
    if (!haveShipped()) {
        MESSAGE("src/bin has no shipped config; skipped");
        return;
    }
    auto bases = carryover::shippedBases("imports/file-manage.nss");
    if (bases.empty()) {
        MESSAGE("file-manage.nss is the same in every release since 2.3.0-beta.1; skipped");
        return;
    }
    const auto shipped = readBytes(binDir() / L"imports" / L"file-manage.nss");
    const std::string base(bases.front());
    // An item in both versions, hidden in the old one.
    auto oldDoc = nss::NssDocument::fromText(base);
    auto newDoc = nss::NssDocument::fromText(nss::decode(shipped).text);
    std::string title;
    for (const auto *d : overrides::declarations(oldDoc)) {
        auto key = overrides::keyOf(oldDoc, "imports/file-manage.nss", *d);
        if (key && overrides::find(newDoc, *key) && !menumap::shellItemHidden(oldDoc, d->line, 0)) {
            title = key->item;
            break;
        }
    }
    REQUIRE_FALSE(title.empty());
    auto user = editedShipped("imports/file-manage.nss", title, hide, base);
    // Not known (an edited copy), so the update kept it.
    CHECK_FALSE(config_upgrade::is_known("imports/file-manage.nss", config_upgrade::fingerprint(user)));

    // Without the earlier release it is a copy with other changes; with it, the hide carries.
    CHECK_FALSE(carryover::carry("imports/file-manage.nss", user, shipped, {}).complete);
    auto r = carryover::carry("imports/file-manage.nss", user, shipped, bases);
    CHECK(r.complete);
    CHECK(r.text == nss::decode(shipped).text);
    REQUIRE(overrideFor(r, title));
    CHECK(*overrideFor(r, title)->find("vis") == "vis.remove");
}

TEST_CASE("carry over: an item the new version doesn't have can't be carried")
{
    std::string base = "item(title='Old' cmd='a')\r\nitem(title='Kept' cmd='b')\r\n";
    std::string user = "item(vis=vis.remove title='Old' cmd='a')\r\nitem(title='Kept' cmd='b')\r\n";
    std::string next = "item(title='New' cmd='a')\r\nitem(title='Kept' cmd='b')\r\n";
    auto r = carryover::carry("imports/x.nss", user, next, { base });
    CHECK_FALSE(r.complete);
    CHECK(r.why.find("'Old'") != std::string::npos);
    // Kept's hide would carry: the item is in both.
    user = "item(title='Old' cmd='a')\r\nitem(vis=vis.remove title='Kept' cmd='b')\r\n";
    r = carryover::carry("imports/x.nss", user, next, { base });
    CHECK(r.complete);
    REQUIRE(overrideFor(r, "'Kept'"));
}

TEST_CASE("carry over: after an update, kaser4's files become the new version and keep his hides")
{
    if (!haveShipped()) {
        MESSAGE("src/bin has no shipped config; skipped");
        return;
    }
    Scratch s;
    const auto config = s.path / L"config";
    fs::create_directories(config);
    fs::copy_file(binDir() / L"shell.nss", config / L"shell.nss");
    fs::copy(binDir() / L"imports", config / L"imports", fs::copy_options::recursive);
    // His own file and icons, and his import of it.
    writeBytes(config / L"imports" / L"kaser.nss", "item(title='Block Access' image='imports\\images\\padlock.png' cmd='x')\r\n");
    writeBytes(config / L"imports" / L"images" / L"padlock.png", std::string("\x89PNG\r\n\x1a\n\0\0", 10));
    {
        auto root = nss::decode(readBytes(config / L"shell.nss")).text + "import 'imports/kaser.nss'\r\n";
        writeBytes(config / L"shell.nss", nss::encode(root, nss::defaultFormat()));
    }
    // Hidden on the Menu map of 2.3.0-beta.5: Command Prompt and Go To's Windows folder.
    writeBytes(config / L"imports" / L"terminal.nss", editedShipped("imports/terminal.nss", "title.command_prompt", hide));
    writeBytes(config / L"imports" / L"goto.nss", editedShipped("imports/goto.nss", "loc.goto_windows", hide));
    // And a file edited by hand: left for Compare / Use the new version.
    {
        auto dev = nss::decode(readBytes(config / L"imports" / L"develop.nss")).text;
        dev = "// my notes\r\n" + dev;
        writeBytes(config / L"imports" / L"develop.nss", nss::encode(dev, nss::defaultFormat()));
    }

    auto upgrade = config_upgrade::run(binDir().wstring(), config.wstring());
    REQUIRE(upgrade.ok);
    std::sort(upgrade.modified_kept.begin(), upgrade.modified_kept.end());
    CHECK(upgrade.modified_kept == std::vector<std::string>{ "imports/develop.nss", "imports/goto.nss", "imports/terminal.nss", "shell.nss" });

    nss::Validator validator;
    if (haveShell())
        validator = nss::makeShellCheckValidator(binDir() / L"shell.exe");
    auto carried = config_update::carryOver(config / L"shell.nss", binDir(), upgrade.modified_kept, false, validator);
    REQUIRE(carried.ok);
    auto files = carried.files;
    std::sort(files.begin(), files.end());
    CHECK(files == std::vector<std::string>{ "imports/goto.nss", "imports/terminal.nss", "shell.nss" });
    CHECK(carried.kept == 3); // two hides and the kaser.nss import
    CHECK_FALSE(carried.snapshot.empty());

    // The shipped files are the new version now, byte for byte; develop.nss is his.
    CHECK(readBytes(config / L"imports" / L"terminal.nss") == readBytes(binDir() / L"imports" / L"terminal.nss"));
    CHECK(readBytes(config / L"imports" / L"goto.nss") == readBytes(binDir() / L"imports" / L"goto.nss"));
    CHECK(nss::decode(readBytes(config / L"imports" / L"develop.nss")).text.rfind("// my notes", 0) == 0);
    auto root = nss::decode(readBytes(config / L"shell.nss")).text;
    CHECK(root.find("import 'imports/kaser.nss'") != std::string::npos);
    CHECK(root.find("import 'imports/settings-app-map.nss'") != std::string::npos);
    auto map = nss::decode(readBytes(config / L"imports" / L"settings-app-map.nss")).text;
    CHECK(map.find("override(file='imports/terminal.nss' item=title.command_prompt vis=vis.remove)") != std::string::npos);
    CHECK(map.find("override(file='imports/goto.nss' item=loc.goto_windows vis=vis.remove)") != std::string::npos);

    // The snapshot has everything as it was, his icon too.
    nss::ConfigStore store(config / L"shell.nss");
    auto snap = store.findSnapshot(carried.snapshot);
    REQUIRE(snap);
    CHECK(snap->reason == config_update::reasonCarry);
    CHECK(std::any_of(snap->files.begin(), snap->files.end(), [](const nss::SnapshotFile &f) { return f.path == "imports/images/padlock.png"; }));

    // The next update finds them unchanged; only develop.nss is still his.
    // The next update finds the shipped files unchanged. develop.nss is still his, and
    // so is shell.nss (his import line): carried over again, as now.
    auto again = config_upgrade::run(binDir().wstring(), config.wstring(), { L"shell.nss", {}, true, true });
    std::sort(again.modified_kept.begin(), again.modified_kept.end());
    CHECK(again.modified_kept == std::vector<std::string>{ "imports/develop.nss", "shell.nss" });
    auto rootAgain = config_update::carryOver(config / L"shell.nss", binDir(), { "shell.nss" }, false, validator);
    CHECK(rootAgain.files == std::vector<std::string>{ "shell.nss" });
    CHECK(rootAgain.kept == 1);
    CHECK(nss::decode(readBytes(config / L"shell.nss")).text == root); // the same file again

    // Use the new version on develop.nss: replaced (his comment goes; the copy is in Backups).
    auto taken = config_update::carryOver(config / L"shell.nss", binDir(), { "imports/develop.nss" }, true, validator);
    REQUIRE(taken.ok);
    CHECK(taken.files == std::vector<std::string>{ "imports/develop.nss" });
    CHECK(readBytes(config / L"imports" / L"develop.nss") == readBytes(binDir() / L"imports" / L"develop.nss"));
}

TEST_CASE("carry over: a value over several lines is not carried")
{
    std::string base = "item(title='Kept' cmd='b')\r\n";
    std::string user = "item(title=('Two' +\r\n'lines') cmd='b')\r\n";
    auto r = carryover::carry("imports/x.nss", user, base, { base });
    CHECK_FALSE(r.complete);
    CHECK(r.overrides.empty());
    CHECK_MESSAGE(r.why.find("several lines") != std::string::npos, r.why);
}

TEST_CASE("carry over: without a way to check the config, override lines are not written")
{
    if (!haveShipped()) {
        MESSAGE("src/bin has no shipped config; skipped");
        return;
    }
    Scratch s;
    const auto config = s.path / L"config";
    fs::create_directories(config);
    fs::copy_file(binDir() / L"shell.nss", config / L"shell.nss");
    fs::copy(binDir() / L"imports", config / L"imports", fs::copy_options::recursive);
    const auto edited = editedShipped("imports/terminal.nss", "title.command_prompt", hide);
    writeBytes(config / L"imports" / L"terminal.nss", edited);

    // No shell.exe found: an older engine would reject override(...), so the file stays
    // as it is (for the banner) and no map file is made.
    for (bool partial : { false, true }) {
        auto carried = config_update::carryOver(config / L"shell.nss", binDir(), { "imports/terminal.nss" }, partial, {});
        CHECK(carried.ok);
        CHECK(carried.files.empty());
        CHECK(readBytes(config / L"imports" / L"terminal.nss") == edited);
        CHECK_FALSE(fs::exists(config / L"imports" / L"settings-app-map.nss"));
    }
}
