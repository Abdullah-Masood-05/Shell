// In-app updates (Updater.h): versions, release selection, assets, checksums, the URL
// allow-list and the portable apply step. UPDATER_LIVE_TEST=1 also asks the real GitHub API.

#include "doctest.h"

#include "Text.h"
#include "Updater.h"

#include <windows.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;
using namespace updater;

namespace {

Version V(const char *s)
{
    auto v = Version::parse(s);
    REQUIRE_MESSAGE(v.has_value(), s);
    return *v;
}

std::string readAll(const fs::path &p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void writeAll(const fs::path &p, const std::string &s)
{
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}

std::vector<Release> fixture()
{
    std::string error;
    auto releases = parseReleases(readAll(fs::path(SETTINGS_TEST_DATA) / "releases.json"), error);
    REQUIRE(error.empty());
    return releases;
}

struct TempDir
{
    fs::path path;
    TempDir()
    {
        path = fs::temp_directory_path() / ("nsp-updater-test-" + std::to_string(GetCurrentProcessId()) + "-" +
                                            std::to_string(GetTickCount64()));
        fs::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void setTimes(const fs::path &p, bool edited)
{
    HANDLE h = CreateFileW(p.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    REQUIRE(h != INVALID_HANDLE_VALUE);
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER t{};
    t.LowPart = now.dwLowDateTime;
    t.HighPart = now.dwHighDateTime;
    ULARGE_INTEGER created = t, written = t;
    created.QuadPart -= 3600ull * 10000000ull; // an hour ago
    written.QuadPart -= edited ? 60ull * 10000000ull : 7200ull * 10000000ull;
    FILETIME c{ created.LowPart, created.HighPart }, w{ written.LowPart, written.HighPart };
    SetFileTime(h, &c, nullptr, &w);
    CloseHandle(h);
}

} // namespace

TEST_CASE("updater: semver parsing and ordering")
{
    CHECK(V("v2.0.1").str() == "2.0.1");
    CHECK(V("2.0.1-beta.1").str() == "2.0.1-beta.1");
    CHECK(V("2.0.1+build.7") == V("2.0.1"));
    CHECK(V(" V2.0.0 ") == V("2.0.0"));
    for (const char *bad : { "", "2", "2.0", "2.0.0.0", "2.0.0-", "2.0.0-beta..1", "x.y.z", "2.0.0-be ta", "nightly" })
        CHECK_MESSAGE(!Version::parse(bad).has_value(), bad);

    // Ascending, the semver.org example plus the cases this project has.
    const char *order[] = { "1.0.0-alpha",  "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta",
                            "1.0.0-beta.2", "1.0.0-beta.11", "1.0.0-rc.1",       "1.0.0",
                            "2.0.0-beta.1", "2.0.0-beta.2",  "2.0.0",            "2.0.1-beta.1",
                            "2.0.1",        "2.0.10",        "2.1.0",            "10.0.0" };
    for (size_t i = 0; i + 1 < std::size(order); ++i) {
        CHECK_MESSAGE(compare(V(order[i]), V(order[i + 1])) < 0, order[i], " < ", order[i + 1]);
        CHECK_MESSAGE(compare(V(order[i + 1]), V(order[i])) > 0, order[i + 1], " > ", order[i]);
    }
    CHECK(compare(V("2.0.0-beta.01"), V("2.0.0-beta.1")) == 0);
    CHECK(compare(V("2.0.0-beta.99999999999999999999"), V("2.0.0-beta.100")) > 0);
    CHECK(defaultChannel(V("2.0.1")) == Channel::Stable);
    CHECK(defaultChannel(V("2.0.1-beta.1")) == Channel::Beta);
}

TEST_CASE("updater: release selection by channel, skip and drafts")
{
    auto releases = fixture();
    CHECK(releases.size() == 5); // "nightly" is not a version
    const auto *beta = &releases[0];
    CHECK(beta->prerelease);
    CHECK(beta->publishedAt == "2026-10-20T10:00:00Z");
    CHECK(releases[1].draft);

    // Stable: 2.0.2 is a draft, 2.1.0-beta.1 a pre-release.
    auto u = selectUpdate(releases, V("2.0.0"), Channel::Stable, "");
    REQUIRE(u);
    CHECK(u->version.str() == "2.0.1");
    CHECK(u->htmlUrl == "https://github.com/Abdullah-Masood-05/Shell/releases/tag/v2.0.1");

    u = selectUpdate(releases, V("2.0.0"), Channel::Beta, "");
    REQUIRE(u);
    CHECK(u->version.str() == "2.1.0-beta.1");

    // Skipped versions are left out, a newer one still shows.
    CHECK(!selectUpdate(releases, V("2.0.0"), Channel::Stable, "2.0.1"));
    u = selectUpdate(releases, V("2.0.0"), Channel::Beta, "2.1.0-beta.1");
    REQUIRE(u);
    CHECK(u->version.str() == "2.0.1");
    u = selectUpdate(releases, V("2.0.0"), Channel::Beta, "2.0.1");
    REQUIRE(u);
    CHECK(u->version.str() == "2.1.0-beta.1");

    // Up to date, and a pre-release build on the stable channel gets the release.
    CHECK(!selectUpdate(releases, V("2.0.1"), Channel::Stable, ""));
    CHECK(!selectUpdate(releases, V("2.1.0-beta.1"), Channel::Beta, ""));
    u = selectUpdate(releases, V("2.0.1-beta.1"), Channel::Stable, "");
    REQUIRE(u);
    CHECK(u->version.str() == "2.0.1");

    // The API's error object, and junk.
    std::string error;
    CHECK(parseReleases(R"({"message":"API rate limit exceeded"})", error).empty());
    CHECK(error.find("rate limit") != std::string::npos);
    error.clear();
    CHECK(parseReleases("<html>", error).empty());
    CHECK(!error.empty());
}

TEST_CASE("updater: asset choice by arch and install type")
{
    auto releases = fixture();
    const Release &r = releases[2];
    REQUIRE(r.version.str() == "2.0.1");
    CHECK(chooseAsset(r, InstallType::Msi, Arch::X64)->name == "setup-x64.msi");
    CHECK(chooseAsset(r, InstallType::Msi, Arch::Arm64)->name == "setup-arm64.msi");
    CHECK(chooseAsset(r, InstallType::Msi, Arch::X86)->name == "setup-x86.msi");
    CHECK(chooseAsset(r, InstallType::Portable, Arch::X64)->name == "NilesoftShellPro-2.0.1-x64-portable.zip");
    CHECK(chooseAsset(r, InstallType::Portable, Arch::Arm64)->name == "NilesoftShellPro-2.0.1-x64-portable.zip");
    // 2.0.0 has only the fixed-name zip and no ARM64 MSI.
    const Release &old = releases[3];
    CHECK(chooseAsset(old, InstallType::Portable, Arch::X64)->name == "NilesoftShellPro-x64-portable.zip");
    CHECK(chooseAsset(old, InstallType::Msi, Arch::Arm64) == nullptr);

    const fs::path pf = L"C:\\Program Files";
    CHECK(detectInstallType(L"C:\\Program Files\\Nilesoft Shell\\NilesoftShellPro.exe", pf,
                            "c:\\program files\\nilesoft shell\\shell.dll") == InstallType::Msi);
    CHECK(detectInstallType(L"C:\\Program Files\\Nilesoft Shell\\NilesoftShellPro.exe", pf, "") ==
          InstallType::Portable);
    CHECK(detectInstallType(L"C:\\Program Files\\Nilesoft Shell\\NilesoftShellPro.exe", pf,
                            "C:\\Windows\\nilesoft\\shell.dll") == InstallType::Portable);
    CHECK(detectInstallType(L"D:\\Tools\\Shell\\NilesoftShellPro.exe", pf, "D:\\Tools\\Shell\\shell.dll") ==
          InstallType::Portable);
    CHECK(detectInstallType(L"C:\\Program Files Extra\\Shell\\NilesoftShellPro.exe", pf,
                            "C:\\Program Files Extra\\Shell\\shell.dll") == InstallType::Portable);
    CHECK(nativeArch() != Arch::X86); // this test binary is x64
}

TEST_CASE("updater: SHA256SUMS parse and verify")
{
    auto sums = parseSha256Sums("2CF24DBA5FB0A30E26E83B2AC5B9E29E1B161E5C1FA7425E73043362938B9824  setup-x64.msi\r\n"
                                "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855 *NilesoftShellPro-x64-portable.zip\n"
                                "not a line\n"
                                "abc  short.txt\n"
                                "\n");
    CHECK(sums.size() == 2);
    CHECK(sums["setup-x64.msi"] == "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");
    CHECK(sums["NilesoftShellPro-x64-portable.zip"] == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    TempDir tmp;
    writeAll(tmp.path / "src" / "setup-x64.msi", "hello");
    CHECK(sha256File(tmp.path / "src" / "setup-x64.msi") == "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");
    CHECK(sha256File(tmp.path / "missing") == "");

    // downloadAndVerify with local assets (the NSP_UPDATE_FEED test mode).
    auto release = fixture()[2];
    Asset msi{ "setup-x64.msi", text::narrow((tmp.path / "src" / "setup-x64.msi").wstring()), 5 };
    auto sumsFile = tmp.path / "src" / "SHA256SUMS.txt";
    for (auto &a : release.assets)
        if (a.name == kSumsName)
            a.url = text::narrow(sumsFile.wstring());

    writeAll(sumsFile, "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824  setup-x64.msi\n");
    auto ok = downloadAndVerify(release, msi, tmp.path / "dl", {}, true);
    CHECK_MESSAGE(ok.ok, ok.error);
    CHECK(fs::is_regular_file(tmp.path / "dl" / "setup-x64.msi"));
    // Already there and verified: reused.
    CHECK(downloadAndVerify(release, msi, tmp.path / "dl", {}, true).ok);

    writeAll(sumsFile, "0000000000000000000000000000000000000000000000000000000000000000  setup-x64.msi\n");
    auto bad = downloadAndVerify(release, msi, tmp.path / "dl2", {}, true);
    CHECK(!bad.ok);
    CHECK(bad.error.find("doesn't match") != std::string::npos);
    CHECK(!fs::exists(tmp.path / "dl2" / "setup-x64.msi")); // deleted

    writeAll(sumsFile, "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824  other.msi\n");
    auto missing = downloadAndVerify(release, msi, tmp.path / "dl3", {}, true);
    CHECK(!missing.ok);
    CHECK(missing.error.find("no entry") != std::string::npos);

    // Without allowLocal a local path is not a download address.
    auto remote = downloadAndVerify(release, msi, tmp.path / "dl4", {}, false);
    CHECK(!remote.ok);

    // A name that would leave the folder.
    Asset evil{ "..\\evil.msi", msi.url, 5 };
    CHECK(!downloadAndVerify(release, evil, tmp.path / "dl5", {}, true).ok);
}

TEST_CASE("updater: download URL allow-list")
{
    for (const char *good : {
             "https://github.com/Abdullah-Masood-05/Shell/releases/download/v2.0.1/setup-x64.msi",
             "https://GitHub.com/abdullah-masood-05/shell/releases/download/v2.0.1/SHA256SUMS.txt",
             "https://github.com:443/Abdullah-Masood-05/Shell/releases/download/v2.0.1/setup-x64.msi",
             "https://objects.githubusercontent.com/github-production-release-asset-2e65be/1/2?X-Amz-Algorithm=a",
             "https://release-assets.githubusercontent.com/github-production-release-asset/1/2?sp=r&sv=2018",
         })
        CHECK_MESSAGE(isAllowedDownloadUrl(good), good);
    for (const char *bad : {
             "http://github.com/Abdullah-Masood-05/Shell/releases/download/v2.0.1/setup-x64.msi",
             "https://github.com/someone-else/Shell/releases/download/v2.0.1/setup-x64.msi",
             "https://github.com/Abdullah-Masood-05/Shell/archive/refs/tags/v2.0.1.zip",
             "https://github.com/Abdullah-Masood-05/Shell/releases/download/",
             "https://github.com/Abdullah-Masood-05/Shell/releases/download/../../../evil/x.msi",
             "https://github.com.evil.example/Abdullah-Masood-05/Shell/releases/download/v1/x.msi",
             "https://evil.example/objects.githubusercontent.com/x",
             "https://objects.githubusercontent.com.evil.example/x",
             "https://user@objects.githubusercontent.com/x",
             "https://objects.githubusercontent.com:8443/x",
             "https://api.github.com/repos/Abdullah-Masood-05/Shell/releases",
             "file:///C:/Windows/System32/calc.exe",
             "C:\\Temp\\setup-x64.msi",
             "https://github.com",
             "",
         })
        CHECK_MESSAGE(!isAllowedDownloadUrl(bad), bad);
}

TEST_CASE("updater: portable apply over a folder with shell.dll in use")
{
    TempDir tmp;
    const auto install = tmp.path / "install";
    const auto staging = tmp.path / "staging";
    // A real dll, so it can be mapped as an image below.
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    const std::string oldDll = readAll(fs::path(sys) / L"version.dll");
    REQUIRE(!oldDll.empty());
    writeAll(install / "shell.dll", oldDll);
    writeAll(install / "shell.exe", "old exe");
    writeAll(install / "NilesoftShellPro.exe", "old app");
    writeAll(install / "shell.nss", "old config");
    writeAll(install / "imports" / "theme.nss", "my theme");
    writeAll(install / "imports" / "same.nss", "unchanged");
    setTimes(install / "shell.nss", false);
    setTimes(install / "imports" / "theme.nss", true); // the user edited it
    writeAll(install / "shell.dll.1.update.old", "left from a previous update");

    writeAll(staging / "shell.dll", "new dll");
    writeAll(staging / "shell.exe", "new exe");
    writeAll(staging / "NilesoftShellPro.exe", "new app");
    writeAll(staging / "shell.nss", "new config");
    writeAll(staging / "imports" / "theme.nss", "new theme");
    writeAll(staging / "imports" / "same.nss", "unchanged");
    writeAll(staging / "imports" / "lang" / "xx.nss", "new language");

    // Mapped as an image, like the shell.dll Explorer has loaded: it can be renamed, not
    // overwritten or deleted.
    HMODULE dll = LoadLibraryExW((install / "shell.dll").c_str(), nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    REQUIRE(dll != nullptr);
    CHECK(!DeleteFileW((install / "shell.dll").c_str()));

    auto r = applyPortable(staging, install);
    CHECK_MESSAGE(r.ok, r.error);
    CHECK(readAll(install / "shell.dll") == "new dll");
    CHECK(readAll(install / "shell.exe") == "new exe");
    CHECK(readAll(install / "NilesoftShellPro.exe") == "new app");
    CHECK(readAll(install / "shell.nss") == "new config"); // not edited: replaced
    CHECK(readAll(install / "imports" / "theme.nss") == "my theme");
    CHECK(readAll(install / "imports" / "theme.nss.new") == "new theme");
    CHECK(readAll(install / "imports" / "lang" / "xx.nss") == "new language");
    REQUIRE(r.keptFiles.size() == 1);
    CHECK(r.keptFiles[0] == L"imports\\theme.nss");
    // The dll in use was renamed aside (the name after the stale one) and is still there.
    CHECK(readAll(install / "shell.dll.2.update.old") == oldDll);
    CHECK(!fs::exists(install / "shell.exe.1.update.old")); // nothing held it: deleted

    FreeLibrary(dll);
    cleanupOldFiles(install);
    CHECK(!fs::exists(install / "shell.dll.1.update.old"));
    CHECK(!fs::exists(install / "shell.dll.2.update.old"));
    CHECK(fs::exists(install / "shell.dll"));
}

TEST_CASE("updater: portable apply rolls back when a file can't be replaced")
{
    TempDir tmp;
    const auto install = tmp.path / "install";
    const auto staging = tmp.path / "staging";
    writeAll(install / "NilesoftShellPro.exe", "old app");
    writeAll(install / "shell.dll", "old dll");
    writeAll(install / "shell.exe", "old exe");
    writeAll(staging / "NilesoftShellPro.exe", "new app");
    writeAll(staging / "shell.dll", "new dll");
    writeAll(staging / "shell.exe", "new exe");
    writeAll(staging / "zz-new.txt", "new file");

    // Held without delete sharing: it can't even be renamed.
    HANDLE exe = CreateFileW((install / "shell.exe").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             0, nullptr);
    REQUIRE(exe != INVALID_HANDLE_VALUE);
    auto r = applyPortable(staging, install);
    CloseHandle(exe);
    CHECK(!r.ok);
    CHECK(r.error.find("shell.exe") != std::string::npos);
    CHECK(readAll(install / "NilesoftShellPro.exe") == "old app");
    CHECK(readAll(install / "shell.dll") == "old dll");
    CHECK(readAll(install / "shell.exe") == "old exe");

    // Not a portable build at all.
    fs::remove(staging / "shell.dll");
    CHECK(!applyPortable(staging, install).ok);
}

TEST_CASE("updater: zip extraction with tar.exe")
{
    TempDir tmp;
    writeAll(tmp.path / "content" / "shell.dll", "dll");
    writeAll(tmp.path / "content" / "imports" / "theme.nss", "theme");
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    auto zip = tmp.path / "p.zip";
    std::wstring cmd = L"\"" + (fs::path(sys) / L"tar.exe").wstring() + L"\" -a -cf \"" + zip.wstring() + L"\" -C \"" +
                       (tmp.path / "content").wstring() + L"\" shell.dll imports";
    STARTUPINFOW si{ sizeof si };
    PROCESS_INFORMATION pi{};
    REQUIRE(CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi));
    WaitForSingleObject(pi.hProcess, 60000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    REQUIRE(fs::is_regular_file(zip));

    std::string error;
    CHECK_MESSAGE(extractZip(zip, tmp.path / "out", error), error);
    CHECK(readAll(tmp.path / "out" / "shell.dll") == "dll");
    CHECK(readAll(tmp.path / "out" / "imports" / "theme.nss") == "theme");
    CHECK(!extractZip(zip, tmp.path / "out", error)); // not empty any more
}

TEST_CASE("updater: live GitHub API (UPDATER_LIVE_TEST=1)")
{
    char *live = nullptr;
    size_t len = 0;
    _dupenv_s(&live, &len, "UPDATER_LIVE_TEST");
    bool enabled = live && std::string(live) == "1";
    free(live);
    if (!enabled) {
        MESSAGE("skipped; set UPDATER_LIVE_TEST=1 to ask api.github.com");
        return;
    }
    auto feed = fetchReleases(L"");
    REQUIRE_MESSAGE(feed.ok, feed.error);
    MESSAGE("releases: " << feed.releases.size());
    const Release *v200 = nullptr;
    for (const auto &r : feed.releases)
        if (r.tag == "v2.0.0")
            v200 = &r;
    REQUIRE(v200);
    CHECK(!v200->draft);
    CHECK(!v200->prerelease);
    const Asset *sums = v200->find(kSumsName);
    REQUIRE(sums);
    CHECK(isAllowedDownloadUrl(sums->url));
    auto res = httpGet(sums->url, [](std::string_view u) { return isAllowedDownloadUrl(u); });
    REQUIRE_MESSAGE(res.ok, res.error);
    auto parsed = parseSha256Sums(res.body);
    for (const auto &[name, hash] : parsed)
        MESSAGE(hash << "  " << name);
    CHECK(parsed.count("setup-x64.msi") == 1);
    CHECK(parsed.count("setup-arm64.msi") == 1);
    const Asset *msi = chooseAsset(*v200, InstallType::Msi, Arch::X64);
    REQUIRE(msi);
    CHECK(isAllowedDownloadUrl(msi->url));
    auto stable = selectUpdate(feed.releases, *Version::parse("1.9.9"), Channel::Stable, "");
    REQUIRE(stable);
    MESSAGE("newest stable release: " << stable->tag);
}
