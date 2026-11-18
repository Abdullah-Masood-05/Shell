// Snapshots keep the user's own files under the config folder too (icons in
// imports\images\, their own .nss files), and the Backups page compares and restores
// them: nss::ConfigStore (the app) and config_upgrade::snapshot (shell.exe).

#include "doctest.h"

#include "BackupsModel.h"

#include "nss/ConfigStore.h"
#include "nss/Text.h"

#include "../../shared/ConfigUpgrade.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;

namespace {

struct Scratch
{
    fs::path path;
    Scratch()
    {
        static int n = 0;
        path = fs::temp_directory_path() /
               (L"settings-backup-files-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++n));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~Scratch()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    void write(const fs::path &rel, std::string_view bytes) const
    {
        fs::create_directories((path / rel).parent_path());
        std::ofstream(path / rel, std::ios::binary | std::ios::trunc).write(bytes.data(), std::streamsize(bytes.size()));
    }
    std::string read(const fs::path &rel) const
    {
        std::ifstream in(path / rel, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
};

// The start of a PNG: not text (NUL bytes in it).
constexpr char kPngBytes[] = "\x89PNG\r\n\x1a\n\0\0\0\rIHDR\0\0\0\x10\0\0\0\x10\x08\x06\0\0\0";
const std::string kPng(kPngBytes, sizeof kPngBytes - 1);

// kaser4's layout: his own kaser.nss and icons in imports\images\.
void makeConfig(const Scratch &s)
{
    s.write("shell.nss", "settings { priority=1 }\r\nimport 'imports/kaser.nss'\r\n");
    s.write("imports/kaser.nss", "item(title='Block Access' image='imports\\images\\padlock-closed.png' cmd='x')\r\n");
    s.write("imports/images/padlock-closed.png", kPng + "closed");
    s.write("imports/images/sub/file explorer.png", kPng + "explorer");
    s.write("imports/images/notes.txt", "where these icons come from\r\n");
    // Not kept: copies, temp files and logs; the backups and backup-<time> folders.
    s.write("shell.nss.bak", "old");
    s.write("imports/.kaser.nss.123.1.tmp", "half");
    s.write("shell.log", "log");
    s.write("backup-20260101-000000/shell.nss", "reset copy");
}

std::vector<std::string> paths(const nss::SnapshotInfo &snap)
{
    std::vector<std::string> out;
    for (const auto &f : snap.files)
        out.push_back(f.path);
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace

TEST_CASE("backups: a snapshot keeps the user's icons and own files")
{
    Scratch s;
    makeConfig(s);
    nss::ConfigStore store(s.path / L"shell.nss");
    std::string error;
    auto snap = store.snapshot("test", &error);
    REQUIRE(snap);
    CHECK(snap->allFiles);
    CHECK(paths(*snap) == std::vector<std::string>{ "imports/images/notes.txt", "imports/images/padlock-closed.png",
                                                    "imports/images/sub/file explorer.png", "imports/kaser.nss", "shell.nss" });
    // The copy is byte for byte, and the manifest says it kept every file.
    std::string copy;
    REQUIRE(nss::readFile(snap->dir / L"imports" / L"images" / L"padlock-closed.png", copy));
    CHECK(copy == kPng + "closed");
    auto again = store.findSnapshot(snap->id);
    REQUIRE(again);
    CHECK(again->allFiles);

    // Unchanged: nothing to show.
    auto same = backups::diffSnapshot(store, *snap);
    CHECK(same.changed == 0);

    // An icon changed, one removed, one added: listed without line diffs; text still diffs.
    s.write("imports/images/padlock-closed.png", kPng + "closed, edited");
    fs::remove(s.path / L"imports" / L"images" / L"sub" / L"file explorer.png");
    s.write("imports/images/new.png", kPng + "new");
    s.write("imports/kaser.nss", "item(title='Block' cmd='x')\r\n");
    auto d = backups::diffSnapshot(store, *snap);
    CHECK(d.changed == 4);
    auto file = [&](const std::string &p) -> const backups::FileDiff & {
        auto it = std::find_if(d.files.begin(), d.files.end(), [&](const backups::FileDiff &f) { return f.path == p; });
        REQUIRE(it != d.files.end());
        return *it;
    };
    CHECK(file("imports/images/padlock-closed.png").status == backups::FileDiff::Modified);
    CHECK(file("imports/images/padlock-closed.png").binary);
    CHECK(file("imports/images/padlock-closed.png").diff.rows.empty());
    CHECK(file("imports/images/padlock-closed.png").newSize > file("imports/images/padlock-closed.png").oldSize);
    CHECK(file("imports/images/sub/file explorer.png").status == backups::FileDiff::Removed);
    CHECK(file("imports/images/new.png").status == backups::FileDiff::Added);
    CHECK(file("imports/images/new.png").binary);
    CHECK_FALSE(file("imports/kaser.nss").binary);
    CHECK(file("imports/kaser.nss").diff.added > 0);

    // Restore puts the icons back byte for byte; one added since stays.
    REQUIRE(store.restore(snap->id, &error));
    CHECK(s.read("imports/images/padlock-closed.png") == kPng + "closed");
    CHECK(s.read("imports/images/sub/file explorer.png") == kPng + "explorer");
    CHECK(fs::exists(s.path / L"imports" / L"images" / L"new.png"));
    CHECK(s.read("imports/kaser.nss").find("Block Access") != std::string::npos);
    // With removeExtra, the icon added since goes too.
    REQUIRE(store.restore(snap->id, &error, true));
    CHECK_FALSE(fs::exists(s.path / L"imports" / L"images" / L"new.png"));
}

TEST_CASE("backups: an older snapshot (.nss only) is compared and restored as such")
{
    Scratch s;
    makeConfig(s);
    nss::ConfigStore store(s.path / L"shell.nss");
    // As earlier versions wrote it: no "scope", only the .nss files.
    auto bytes = s.read("shell.nss");
    s.write("backups/20260101-120000/shell.nss", bytes);
    s.write("backups/20260101-120000/manifest.json",
            "{ \"version\": 1, \"id\": \"20260101-120000\", \"created\": \"2026-01-01T12:00:00\", \"reason\": \"old\", "
            "\"root\": \"shell.nss\", \"pinned\": false, \"files\": [ { \"path\": \"shell.nss\", \"sha256\": \"" +
                nss::ConfigStore::sha256(bytes) + "\", \"size\": " + std::to_string(bytes.size()) + " } ] }");
    auto old = store.findSnapshot("20260101-120000");
    REQUIRE(old);
    CHECK_FALSE(old->allFiles);
    auto d = backups::diffSnapshot(store, *old);
    // kaser.nss is "added" (a .nss file), the icons aren't listed at all.
    for (const auto &f : d.files)
        CHECK(f.path.find("images/") == std::string::npos);
    // Restoring it with removeExtra takes out .nss files added since, never the icons.
    std::string error;
    REQUIRE(store.restore("20260101-120000", &error, true));
    CHECK(fs::exists(s.path / L"imports" / L"images" / L"padlock-closed.png"));
    CHECK_FALSE(fs::exists(s.path / L"imports" / L"kaser.nss"));
}

TEST_CASE("backups: very big files are left out of a snapshot")
{
    Scratch s;
    makeConfig(s);
    s.write("imports/images/huge.bin", std::string(size_t(nss::ConfigStore::maxSnapshotFile + 1), 'x'));
    nss::ConfigStore store(s.path / L"shell.nss");
    auto files = store.snapshotFiles();
    for (const auto &f : files)
        CHECK(f.filename() != L"huge.bin");
    CHECK(std::any_of(files.begin(), files.end(), [](const fs::path &f) { return f.filename() == L"padlock-closed.png"; }));
}

TEST_CASE("backups: shell.exe's snapshot before an update keeps the icons too")
{
    Scratch s;
    makeConfig(s);
    config_upgrade::Snapshot snap;
    std::string error;
    REQUIRE(config_upgrade::snapshot(s.path.wstring(), L"shell.nss", config_upgrade::reason_upgrade, snap, error));
    nss::ConfigStore store(s.path / L"shell.nss");
    auto info = store.findSnapshot(snap.id);
    REQUIRE(info);
    CHECK(info->allFiles);
    CHECK(paths(*info) == std::vector<std::string>{ "imports/images/notes.txt", "imports/images/padlock-closed.png",
                                                    "imports/images/sub/file explorer.png", "imports/kaser.nss", "shell.nss" });
    // The app reads it like its own: nothing changed since.
    CHECK(backups::diffSnapshot(store, *info).changed == 0);
    std::string copy;
    REQUIRE(nss::readFile(info->dir / L"imports" / L"images" / L"sub" / L"file explorer.png", copy));
    CHECK(copy == kPng + "explorer");
}

TEST_CASE("backups: a snapshot keeps to .nss files and imports\, not the rest of the folder")
{
    Scratch s;
    makeConfig(s);
    // A config root in Documents: other files there are not the config's.
    s.write("report.docx", "word");
    s.write("photos/holiday.jpg", kPng);
    s.write("Backup-20260102-000000/imports/images/old.png", kPng);
    s.write("imports/Backup-1/x.png", kPng);
    nss::ConfigStore store(s.path / L"shell.nss");
    const std::vector<std::string> want{ "imports/images/notes.txt", "imports/images/padlock-closed.png",
                                         "imports/images/sub/file explorer.png", "imports/kaser.nss", "shell.nss" };
    auto snap = store.snapshot("test");
    REQUIRE(snap);
    CHECK(paths(*snap) == want);
    CHECK(snap->skipped.empty());

    config_upgrade::Snapshot shellSnap;
    std::string error;
    REQUIRE(config_upgrade::snapshot(s.path.wstring(), L"shell.nss", config_upgrade::reason_upgrade, shellSnap, error));
    auto info = store.findSnapshot(shellSnap.id);
    REQUIRE(info);
    CHECK(paths(*info) == want);
}
