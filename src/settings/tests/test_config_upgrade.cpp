// src/shared/ConfigUpgrade.h, which `shell.exe -config:upgrade` and `-config:init` use to
// bring a per-user config up to date: the normalization and fingerprints, the list of
// shipped versions, the decision for each file, and a whole run on temporary folders
// (the snapshot it takes is read back with the app's own nss::ConfigStore).
// The app itself only runs shell.exe; this is the one place it includes src/shared.

#include "doctest.h"

#include "../../shared/ConfigUpgrade.h"

#include "ConfigUpdate.h"

#include "nss/ConfigStore.h"
#include "nss/Text.h"

#include <atomic>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
namespace cu = config_upgrade;

namespace {

fs::path binDir() { return fs::path(SETTINGS_SOURCE_BIN).lexically_normal(); }

std::string readBytes(const fs::path &p)
{
    std::string bytes;
    nss::readFile(p, bytes);
    return bytes;
}

void writeBytes(const fs::path &p, std::string_view bytes)
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
               (L"config-upgrade-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++n));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

bool contains(const std::vector<std::string> &list, std::string_view item)
{
    for (const auto &s : list)
        if (s == item)
            return true;
    return false;
}

// The block `shell -config:init` (2.0.5) adds to a shell.nss without a language import.
const char *kInitBlock = "// Added by shell -config:init; the file as it was is shell.nss.old\n"
                         "// localization\n"
                         "$loc_path=app.dir + '\\imports\\lang\\'\n"
                         "import lang loc_path + \"en.nss\"\n"
                         "\n"
                         "// or import lang 'imports/lang/en.nss'\n";

} // namespace

TEST_CASE("config upgrade: line endings, a BOM and trailing blanks don't count")
{
    const std::string lf = "settings\n{\n\tx = 1\n}\nimport 'imports/theme.nss'\n";
    CHECK(cu::normalize(lf) == "settings\n{\n\tx = 1\n}\nimport 'imports/theme.nss'");
    CHECK(cu::fingerprint("\xEF\xBB\xBFsettings\r\n{\r\n\tx = 1\r\n}\r\nimport 'imports/theme.nss'\r\n") == cu::fingerprint(lf));
    CHECK(cu::fingerprint("settings\r{\r\tx = 1\r}\rimport 'imports/theme.nss'") == cu::fingerprint(lf));
    CHECK(cu::fingerprint("settings  \n{\t\n\tx = 1 \n}\nimport 'imports/theme.nss'\n\n\n") == cu::fingerprint(lf));

    // Anything else does.
    CHECK(cu::fingerprint("settings\n{\n\tx = 2\n}\nimport 'imports/theme.nss'\n") != cu::fingerprint(lf));
    CHECK(cu::fingerprint("settings\n{\n  x = 1\n}\nimport 'imports/theme.nss'\n") != cu::fingerprint(lf));
    CHECK(cu::fingerprint("\nsettings\n{\n\tx = 1\n}\nimport 'imports/theme.nss'\n") != cu::fingerprint(lf));
    // A UTF-16 file is never the same as a UTF-8 one.
    CHECK(cu::fingerprint(std::string("\xFF\xFEs\0", 4)) != cu::fingerprint("s"));
}

TEST_CASE("config upgrade: the app's import lines don't count")
{
    const std::string base = "import 'imports/theme.nss'\nimport 'imports/modify.nss'\n";
    for (const char *line : { "import 'imports/settings-app.nss'", "import \"imports/settings-app.nss\"",
                              "IMPORT 'Imports\\Settings-App-Menus.nss'", "  import  'imports/settings-app-openwith.nss'  " }) {
        CAPTURE(line);
        CHECK(cu::is_app_import_line(line));
        CHECK(cu::fingerprint(base + line + "\n") == cu::fingerprint(base));
    }
    for (const char *line : { "import 'imports/theme.nss'", "import lang 'imports/settings-app.nss'",
                              "import 'imports/settings-app.nss' // mine", "import 'imports/settings-app.txt'",
                              "importx 'imports/settings-app.nss'", "import 'my/imports/settings-app.nss'" }) {
        CAPTURE(line);
        CHECK_FALSE(cu::is_app_import_line(line));
    }

    auto lines = cu::app_import_lines("a\r\nimport 'imports/settings-app.nss'\r\nb\nimport 'imports/settings-app-menus.nss'");
    REQUIRE(lines.size() == 2);
    CHECK(lines[0] == "import 'imports/settings-app.nss'");
    CHECK(lines[1] == "import 'imports/settings-app-menus.nss'");
    CHECK(cu::with_app_imports("x\r\ny", lines) ==
          "x\r\ny\r\nimport 'imports/settings-app.nss'\r\nimport 'imports/settings-app-menus.nss'\r\n");
    CHECK(cu::with_app_imports("x\n", { "import 'imports/settings-app.nss'" }) == "x\nimport 'imports/settings-app.nss'\n");
    CHECK(cu::with_app_imports("x", {}) == "x");

    // The Menu map's rules go before the root's other config imports (MapRules.h).
    const std::string shipped = "settings {}\r\nimport lang 'imports/lang/en.nss'\r\n  import 'nested.nss'\r\n"
                                "import 'imports/theme.nss'\r\nimport 'imports/modify.nss'\r\n";
    CHECK(cu::with_app_imports(shipped, { "import 'imports/settings-app-map.nss'", "import 'imports/settings-app.nss'",
                                          "import 'imports/settings-app-snippets.nss'" }) ==
          "settings {}\r\nimport lang 'imports/lang/en.nss'\r\n  import 'nested.nss'\r\n"
          "import 'imports/settings-app-map.nss'\r\nimport 'imports/theme.nss'\r\nimport 'imports/modify.nss'\r\n"
          "import 'imports/settings-app.nss'\r\nimport 'imports/settings-app-snippets.nss'\r\n");
    // No config import to go before: at the end, like the others.
    CHECK(cu::with_app_imports("x", { "import \"Imports\\Settings-App-Map.nss\"" }) ==
          "x\nimport \"Imports\\Settings-App-Map.nss\"\n");
}

TEST_CASE("config upgrade: the localization block -config:init adds doesn't count")
{
    const std::string settings = "settings\n{\n\tx = 1\n}\n";
    const std::string imports = "import 'imports/theme.nss'\nimport 'imports/modify.nss'\n";
    const std::string original = settings + "\n" + imports;
    const auto expected = cu::fingerprint(original);

    // Where add_localization() puts it: after the settings block (a blank line before it),
    // before the first import (a blank line after it), or at the end (a blank line before).
    CHECK(cu::fingerprint(settings + "\n" + kInitBlock + "\n" + imports) == expected);
    const std::string noSettings = "// mine\n" + imports;
    CHECK(cu::fingerprint("// mine\n" + std::string(kInitBlock) + "\n" + imports) == cu::fingerprint(noSettings));
    CHECK(cu::fingerprint("// mine\n\n" + std::string(kInitBlock)) == cu::fingerprint("// mine\n"));

    // An unfinished block is the user's text.
    CHECK(cu::fingerprint(settings + "\n// Added by shell -config:init\n// localization\n\n" + imports) != expected);
}

TEST_CASE("config upgrade: what is done with each file")
{
    using cu::Action;
    cu::FileState f;
    f.shipped_hash = 2;

    SUBCASE("identical to the shipped file")
    {
        f.user_exists = true;
        f.user_hash = 2;
        CHECK(cu::decide(f) == Action::unchanged);
        f.take = true;
        CHECK(cu::decide(f) == Action::unchanged);
    }
    SUBCASE("an older shipped version")
    {
        f.user_exists = true;
        f.user_hash = 1;
        f.known = true;
        CHECK(cu::decide(f) == Action::update);
    }
    SUBCASE("edited by the user")
    {
        f.user_exists = true;
        f.user_hash = 3;
        CHECK(cu::decide(f) == Action::keep_modified);
        f.take = true; // "Use the new version"
        CHECK(cu::decide(f) == Action::update);
    }
    SUBCASE("missing")
    {
        CHECK(cu::decide(f) == Action::skip);
        f.referenced = true;
        CHECK(cu::decide(f) == Action::add);
    }
}

TEST_CASE("config upgrade: known versions, paths and references")
{
    // Every file in src/bin is in the list (scripts/update-shipped-hashes.ps1 was run).
    std::vector<std::string> shipped{ "shell.nss" };
    cu::list_nss((binDir() / L"imports").wstring(), "imports/", shipped);
    CHECK(shipped.size() > 10);
    for (const auto &rel : shipped) {
        CAPTURE(rel);
        CHECK(cu::is_known(rel, cu::fingerprint(readBytes(binDir() / fs::path(nss::widen(rel))))));
    }
    CHECK_FALSE(cu::is_known("imports/modify.nss", cu::fingerprint("// mine")));
    // A fingerprint counts for its own path only.
    CHECK_FALSE(cu::is_known("imports/theme.nss", cu::fingerprint(readBytes(binDir() / L"imports" / L"modify.nss"))));

    CHECK(cu::is_app_owned("imports/settings-app.nss"));
    CHECK(cu::is_app_owned("Imports\\Settings-App-Menus.nss"));
    CHECK(cu::is_app_owned("imports\\snippets\\copy-file-hash.nss"));
    CHECK_FALSE(cu::is_app_owned("imports/theme.nss"));
    CHECK_FALSE(cu::is_app_owned("imports/snippets.nss"));
    CHECK(cu::same_rel("imports/lang/zh-CN.nss", "Imports\\Lang\\zh-cn.nss"));
    CHECK_FALSE(cu::same_rel("imports/lang/zh-CN.nss", "imports/lang/zh-TW.nss"));

    std::vector<std::string> texts{ "import 'imports/goto.nss'\n$loc_path=app.dir + '\\imports\\lang\\'" };
    CHECK(cu::is_referenced("imports/goto.nss", texts));
    CHECK(cu::is_referenced("imports/lang/de-DE.nss", texts));
    CHECK_FALSE(cu::is_referenced("imports/taskbar.nss", texts));
    CHECK_FALSE(cu::is_referenced("shell.nss", texts));
    CHECK_FALSE(cu::is_referenced("imports/goto.nss", { "import 'imports/mygoto.nss'" }));
    CHECK_FALSE(cu::is_referenced("imports/lang/en.nss", { "import 'imports/theme.nss'" }));
}

TEST_CASE("config upgrade: a whole run on temporary folders")
{
    // The install folder is a newer release: its modify.nss and shell.nss differ from the
    // ones in src/bin, which are known shipped versions.
    TempDir tmp;
    const auto install = tmp.path / L"install";
    const auto user = tmp.path / L"user";
    const auto srcModify = readBytes(binDir() / L"imports" / L"modify.nss");
    const auto srcTheme = readBytes(binDir() / L"imports" / L"theme.nss");
    const auto srcTerminal = readBytes(binDir() / L"imports" / L"terminal.nss");
    const auto srcShell = readBytes(binDir() / L"shell.nss");
    REQUIRE_FALSE(srcModify.empty());
    REQUIRE_FALSE(srcShell.empty());

    const std::string newModify = srcModify + "\r\n// new in this version\r\n";
    const std::string newShell = srcShell + "\r\nimport 'imports/goto.nss'\r\n";
    writeBytes(install / L"shell.nss", newShell);
    writeBytes(install / L"imports" / L"modify.nss", newModify);
    writeBytes(install / L"imports" / L"theme.nss", srcTheme + "// new\r\n");
    writeBytes(install / L"imports" / L"terminal.nss", srcTerminal);
    writeBytes(install / L"imports" / L"goto.nss", "// goto\r\n");
    writeBytes(install / L"imports" / L"extra.nss", "// extra\r\n");
    writeBytes(install / L"imports" / L"settings-app.nss", "// never shipped, never copied\r\n");

    // The user: the old shell.nss (LF, with the app's import), the old modify.nss, an
    // edited theme.nss, the current terminal.nss, no goto.nss or extra.nss.
    std::string lf;
    for (char c : srcShell)
        if (c != '\r')
            lf += c;
    lf += "import 'imports/settings-app.nss'\n";
    writeBytes(user / L"shell.nss", lf);
    writeBytes(user / L"imports" / L"modify.nss", srcModify);
    writeBytes(user / L"imports" / L"theme.nss", srcTheme + "// mine\r\n");
    writeBytes(user / L"imports" / L"terminal.nss", srcTerminal);
    writeBytes(user / L"imports" / L"settings-app.nss", "settings { }\r\n");
    writeBytes(user / L"imports" / L"mine.nss", "// mine\r\n");

    SUBCASE("dry run")
    {
        cu::Options options;
        options.dry_run = true;
        auto r = cu::run(install.wstring(), user.wstring(), options);
        CHECK(r.ok);
        CHECK(r.backup_id.empty());
        CHECK(readBytes(user / L"imports" / L"modify.nss") == srcModify);
        CHECK(contains(r.updated, "imports/modify.nss"));
    }

    auto r = cu::run(install.wstring(), user.wstring());
    CHECK(r.ok);
    CHECK(r.updated == std::vector<std::string>{ "imports/modify.nss", "shell.nss" });
    // goto.nss: the new shell.nss imports it. extra.nss: nothing does.
    CHECK(r.added == std::vector<std::string>{ "imports/goto.nss" });
    CHECK(r.modified_kept == std::vector<std::string>{ "imports/theme.nss" });
    CHECK(r.unchanged == std::vector<std::string>{ "imports/terminal.nss" });
    CHECK(r.failed.empty());

    CHECK(readBytes(user / L"imports" / L"modify.nss") == newModify);
    CHECK(readBytes(user / L"shell.nss") == newShell + "import 'imports/settings-app.nss'\r\n");
    CHECK(readBytes(user / L"imports" / L"theme.nss") == srcTheme + "// mine\r\n");
    CHECK(readBytes(user / L"imports" / L"settings-app.nss") == "settings { }\r\n");
    CHECK(readBytes(user / L"imports" / L"mine.nss") == "// mine\r\n");
    CHECK_FALSE(fs::exists(user / L"imports" / L"extra.nss"));

    // The snapshot is one the Backups page reads, with the files as they were.
    REQUIRE_FALSE(r.backup_id.empty());
    nss::ConfigStore store(user / L"shell.nss");
    auto snap = store.findSnapshot(r.backup_id);
    REQUIRE(snap.has_value());
    CHECK(snap->reason == cu::reason_upgrade);
    CHECK(snap->root == "shell.nss");
    CHECK(snap->files.size() == 6);
    for (const auto &f : snap->files) {
        CAPTURE(f.path);
        CHECK(nss::ConfigStore::sha256(readBytes(snap->dir / fs::path(nss::widen(f.path)))) == f.sha256);
    }
    CHECK(readBytes(snap->dir / L"imports" / L"modify.nss") == srcModify);

    // Again: nothing to do, no new snapshot.
    auto again = cu::run(install.wstring(), user.wstring());
    CHECK(again.ok);
    CHECK(again.updated.empty());
    CHECK(again.added.empty());
    CHECK(again.backup_id.empty());
    CHECK(again.modified_kept == std::vector<std::string>{ "imports/theme.nss" });

    // "Use the new version" for the edited file; the app's own file is refused.
    cu::Options take;
    take.take = { "Imports\\Theme.nss", "imports/settings-app.nss" };
    auto taken = cu::run(install.wstring(), user.wstring(), take);
    CHECK_FALSE(taken.ok);
    CHECK(taken.updated == std::vector<std::string>{ "imports/theme.nss" });
    CHECK(taken.failed == std::vector<std::string>{ "imports/settings-app.nss" });
    CHECK(readBytes(user / L"imports" / L"theme.nss") == srcTheme + "// new\r\n");
    CHECK(readBytes(user / L"imports" / L"settings-app.nss") == "settings { }\r\n");
    auto takeSnap = store.findSnapshot(taken.backup_id);
    REQUIRE(takeSnap.has_value());
    CHECK(takeSnap->reason == cu::reason_take);
    CHECK(readBytes(takeSnap->dir / L"imports" / L"theme.nss") == srcTheme + "// mine\r\n");

    // The JSON shell.exe prints.
    auto json = cu::to_json(r, install.wstring(), user.wstring());
    CHECK(json.find("\"updated\": [\"imports/modify.nss\", \"shell.nss\"]") != std::string::npos);
    CHECK(json.find("\"modified_kept\": [\"imports/theme.nss\"]") != std::string::npos);
    CHECK(json.find("\"error\": null") != std::string::npos);

    // The install folder itself is never a target.
    auto self = cu::run(install.wstring(), install.wstring());
    CHECK_FALSE(self.ok);
}

TEST_CASE("config upgrade: when the app runs it, and reading what shell.exe printed")
{
    CHECK(config_update::shouldRun("", "2.0.6", false));      // never ran (or before this existed)
    CHECK(config_update::shouldRun("2.0.5", "2.0.6", false)); // an update
    CHECK(config_update::shouldRun("2.0.6", "2.0.5", false)); // a downgrade
    CHECK_FALSE(config_update::shouldRun("2.0.6", "2.0.6", false));
    CHECK(config_update::shouldRun("2.0.6", "2.0.6", true));  // the update helper reinstalled it

    CHECK(config_update::isPersonal("appdata", true));
    CHECK(config_update::isPersonal("registry", true));
    CHECK_FALSE(config_update::isPersonal("install", true));
    CHECK_FALSE(config_update::isPersonal("appdata", false));
    CHECK_FALSE(config_update::isPersonal("", false));

    cu::Result result;
    result.updated = { "imports/modify.nss" };
    result.added = { "imports/goto.nss" };
    result.modified_kept = { "imports/theme.nss", "shell.nss" };
    result.backup_id = "20261018-120000";
    auto report = config_update::parseReport(
        cu::to_json(result, L"C:\\Program Files\\Nilesoft Shell", L"C:\\Users\\me\\AppData\\Roaming\\Nilesoft\\Shell"));
    REQUIRE(report.has_value());
    CHECK(report->ok);
    CHECK(report->changed() == 2);
    CHECK(report->modifiedKept == std::vector<std::string>{ "imports/theme.nss", "shell.nss" });
    CHECK(report->backup == "20261018-120000");
    CHECK(report->installDir == "C:\\Program Files\\Nilesoft Shell");
    CHECK(report->error.empty());

    // No personal config: a null configDir, nothing done.
    auto none = config_update::parseReport(cu::to_json(cu::Result{}, L"C:\\Shell", L""));
    REQUIRE(none.has_value());
    CHECK(none->configDir.empty());
    CHECK(none->changed() == 0);

    // An older shell.exe prints its usage text instead.
    CHECK_FALSE(config_update::parseReport("usage: shell -config:init [-force] | ...").has_value());
}
