// App updates without administrator rights (AppChannel.h, Updater.h): update.json, the
// choice between the app-only and the installer path, the per-user copies the installed
// exe hands off to, their cleanup, and the config upgrade a per-user copy does itself.

#include "doctest.h"

#include "AppChannel.h"
#include "ConfigUpdate.h"
#include "Text.h"
#include "Updater.h"

#include <windows.h>

#include <algorithm>
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

void writeAll(const fs::path &p, const std::string &s)
{
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary | std::ios::trunc) << s;
}

std::string readAll(const fs::path &p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

struct TempDir
{
    fs::path path;
    TempDir()
    {
        path = fs::temp_directory_path() / ("nsp-appchannel-test-" + std::to_string(GetCurrentProcessId()) + "-" +
                                            std::to_string(GetTickCount64()));
        fs::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

// Points %LOCALAPPDATA% somewhere else for one test.
struct LocalAppData
{
    std::wstring old;
    bool had = false;
    explicit LocalAppData(const fs::path &p)
    {
        wchar_t buf[1024];
        DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, 1024);
        had = n > 0 && n < 1024;
        if (had)
            old.assign(buf, n);
        SetEnvironmentVariableW(L"LOCALAPPDATA", p.c_str());
    }
    ~LocalAppData() { SetEnvironmentVariableW(L"LOCALAPPDATA", had ? old.c_str() : nullptr); }
};

const std::string H1(64, '1'), H2(64, '2'), H3(64, '3');

std::string manifestJson(const std::string &version, const std::string &coreSince, const std::string &exeHash = H1)
{
    return R"({ "schema": 1, "version": ")" + version + R"(",
      "app": { "package": "NilesoftShellPro-)" + version + R"(-x64-app.zip", "sha256": ")" + H3 + R"(",
               "minCore": ")" + coreSince + R"(",
               "files": { "NilesoftShellPro.exe": ")" + exeHash + R"(", "imports/theme.nss": ")" + H2 + R"(" } },
      "core": { "since": ")" + coreSince + R"(", "changed": false },
      "config": { "since": ")" + version + R"(", "changed": true } })";
}

Release release(const std::string &version, bool withPackage = true)
{
    Release r;
    r.tag = "v" + version;
    r.version = V(version.c_str());
    auto add = [&](const std::string &name) {
        r.assets.push_back({ name, "https://github.com/Abdullah-Masood-05/Shell/releases/download/v" + version + "/" + name, 1000 });
    };
    add("setup-x64.msi");
    add("setup-arm64.msi");
    add("NilesoftShellPro-" + version + "-x64-portable.zip");
    add("SHA256SUMS.txt");
    add("update.json");
    if (withPackage)
        add("NilesoftShellPro-" + version + "-x64-app.zip");
    return r;
}

// A per-user copy folder with an install.json and an exe whose content is `exe`.
fs::path makeCopy(const fs::path &root, const std::string &version, const fs::path &base, const std::string &exe = "exe")
{
    auto dir = root / text::widen(version);
    writeAll(dir / "NilesoftShellPro.exe", exe + version);
    appchannel::Record r;
    r.version = V(version.c_str());
    r.base = base;
    r.files["NilesoftShellPro.exe"] = sha256File(dir / "NilesoftShellPro.exe");
    REQUIRE(appchannel::writeRecord(dir, r));
    return dir;
}

std::string hashOf(const fs::path &p)
{
    std::error_code ec;
    return fs::is_regular_file(p, ec) ? sha256File(p) : std::string();
}

} // namespace

TEST_CASE("update.json: parsing")
{
    std::string error;
    auto m = parseManifest(manifestJson("2.2.0", "2.1.1"), error);
    REQUIRE_MESSAGE(m.has_value(), error);
    CHECK(m->version.str() == "2.2.0");
    CHECK(m->hasApp);
    CHECK(m->appPackage == "NilesoftShellPro-2.2.0-x64-app.zip");
    CHECK(m->appSha256 == H3);
    CHECK(m->minCore.str() == "2.1.1");
    CHECK(m->coreSince.str() == "2.1.1");
    CHECK(!m->coreChanged);
    CHECK(m->configSince.str() == "2.2.0");
    CHECK(m->appFiles.size() == 2);
    CHECK(m->appFiles.at("imports/theme.nss") == H2);

    // minCore defaults to core.since; no app entry is a manifest without a package.
    m = parseManifest(R"({"schema":1,"version":"2.2.0","core":{"since":"2.2.0","changed":true}})", error);
    REQUIRE(m.has_value());
    CHECK(!m->hasApp);
    CHECK(m->coreChanged);

    // Unusable ones.
    CHECK(!parseManifest("not json", error));
    CHECK(!parseManifest(R"({"schema":2,"version":"2.2.0","core":{"since":"2.2.0"}})", error));
    CHECK(!parseManifest(R"({"schema":1,"version":"2.2.0"})", error)); // no core entry
    CHECK(!parseManifest(R"({"schema":1,"version":"2.2.0","core":{"since":"2.3.0"}})", error)); // since > version
    auto withFile = [&](const std::string &path) {
        return R"({"schema":1,"version":"2.2.0","core":{"since":"2.2.0"},"app":{"package":"a.zip","sha256":")" + H3 +
               R"(","files":{"NilesoftShellPro.exe":")" + H1 + R"(",")" + path + R"(":")" + H2 + R"("}}})";
    };
    CHECK(parseManifest(withFile("imports/lang/en.nss"), error).has_value());
    CHECK(!parseManifest(withFile("../evil.exe"), error));
    CHECK(!parseManifest(withFile("imports/../../evil.exe"), error));
    CHECK(!parseManifest(withFile("/evil.exe"), error));
    CHECK(!parseManifest(withFile("C:/evil.exe"), error));
    CHECK(!parseManifest(withFile("imports\\\\evil.nss"), error));
    // The package needs the app itself.
    CHECK(!parseManifest(R"({"schema":1,"version":"2.2.0","core":{"since":"2.2.0"},"app":{"package":"a.zip","sha256":")" + H3 +
                             R"(","files":{"imports/a.nss":")" + H2 + R"("}}})",
                         error));
    // A package name that could leave the download folder.
    CHECK(!parseManifest(R"({"schema":1,"version":"2.2.0","core":{"since":"2.2.0"},"app":{"package":"..\\a.zip","sha256":")" + H3 +
                             R"(","files":{"NilesoftShellPro.exe":")" + H1 + R"("}}})",
                         error));
}

TEST_CASE("update path: app only unless the core changed, and only the installer needs elevation")
{
    std::string error;
    auto rel = release("2.2.0");
    auto manifest = parseManifest(manifestJson("2.2.0", "2.1.1"), error);
    REQUIRE(manifest);

    SUBCASE("portable: replaced in place, never elevated")
    {
        for (auto *m : { &manifest, static_cast<std::optional<Manifest> *>(nullptr) }) {
            auto p = choosePlan(rel, InstallType::Portable, Arch::X64, m ? *m : std::nullopt, std::nullopt);
            CHECK(p.path == UpdatePath::Portable);
            CHECK(!p.needsElevation);
            REQUIRE(p.asset);
            CHECK(p.asset->name == "NilesoftShellPro-2.2.0-x64-portable.zip");
        }
    }
    SUBCASE("MSI with the core unchanged since the installed release: app only, no elevation")
    {
        for (const char *installed : { "2.1.1", "2.1.2", "2.2.0-beta.1" }) {
            auto p = choosePlan(rel, InstallType::Msi, Arch::X64, manifest, V(installed));
            CHECK(p.path == UpdatePath::AppOnly);
            CHECK(!p.needsElevation);
            CHECK(p.reason == ElevationReason::None);
            REQUIRE(p.asset);
            CHECK(p.asset->name == "NilesoftShellPro-2.2.0-x64-app.zip");
        }
        // ARM64 runs the x64 app too.
        CHECK(choosePlan(rel, InstallType::Msi, Arch::Arm64, manifest, V("2.1.1")).path == UpdatePath::AppOnly);
    }
    SUBCASE("MSI with an older core: the installer, opt-in elevation")
    {
        for (const char *installed : { "2.1.0", "2.1.1-beta.3", "1.0.0" }) {
            auto p = choosePlan(rel, InstallType::Msi, Arch::X64, manifest, V(installed));
            CHECK(p.path == UpdatePath::Installer);
            CHECK(p.needsElevation);
            CHECK(p.reason == ElevationReason::CoreChanged);
            REQUIRE(p.asset);
            CHECK(p.asset->name == "setup-x64.msi");
        }
        auto arm = choosePlan(rel, InstallType::Msi, Arch::Arm64, manifest, V("2.0.0"));
        REQUIRE(arm.asset);
        CHECK(arm.asset->name == "setup-arm64.msi");
    }
    SUBCASE("MSI without a usable manifest or package: the installer")
    {
        auto none = choosePlan(rel, InstallType::Msi, Arch::X64, std::nullopt, V("2.1.1"));
        CHECK(none.path == UpdatePath::Installer);
        CHECK(none.needsElevation);
        CHECK(none.reason == ElevationReason::NoAppPackage);

        auto noPackage = choosePlan(release("2.2.0", false), InstallType::Msi, Arch::X64, manifest, V("2.1.1"));
        CHECK(noPackage.path == UpdatePath::Installer);
        CHECK(noPackage.reason == ElevationReason::NoAppPackage);

        auto unknown = choosePlan(rel, InstallType::Msi, Arch::X64, manifest, std::nullopt);
        CHECK(unknown.path == UpdatePath::Installer);
        CHECK(unknown.reason == ElevationReason::UnknownCore);

        // An x86 install has no app to update.
        CHECK(choosePlan(rel, InstallType::Msi, Arch::X86, manifest, V("2.1.1")).path == UpdatePath::Installer);
    }
    SUBCASE("only the installer path ever needs elevation")
    {
        for (auto type : { InstallType::Msi, InstallType::Portable })
            for (auto arch : { Arch::X64, Arch::Arm64, Arch::X86 })
                for (const char *installed : { "1.0.0", "2.1.1", "2.2.0" }) {
                    auto p = choosePlan(rel, type, arch, manifest, V(installed));
                    CHECK(p.needsElevation == (p.path == UpdatePath::Installer));
                }
    }
}

TEST_CASE("update.json: fetched and checked against SHA256SUMS.txt (local feed)")
{
    TempDir t;
    const std::string body = manifestJson("2.2.0", "2.1.1");
    writeAll(t.path / "update.json", body);
    writeAll(t.path / "SHA256SUMS.txt", sha256Bytes(body) + "  update.json\n");
    Release rel;
    rel.version = V("2.2.0");
    rel.assets.push_back({ "update.json", text::narrow((t.path / "update.json").wstring()), 0 });
    rel.assets.push_back({ "SHA256SUMS.txt", text::narrow((t.path / "SHA256SUMS.txt").wstring()), 0 });

    auto ok = fetchManifest(rel, true);
    REQUIRE_MESSAGE(ok.ok, ok.error);
    CHECK(ok.manifest->appPackage == "NilesoftShellPro-2.2.0-x64-app.zip");

    // Local paths only with a local feed.
    CHECK(!fetchManifest(rel, false).ok);

    // Changed after the sums were made.
    writeAll(t.path / "update.json", body + " ");
    auto bad = fetchManifest(rel, true);
    CHECK(!bad.ok);
    CHECK(!bad.missing);

    // For another version.
    const std::string other = manifestJson("2.3.0", "2.1.1");
    writeAll(t.path / "update.json", other);
    writeAll(t.path / "SHA256SUMS.txt", sha256Bytes(other) + "  update.json\n");
    CHECK(!fetchManifest(rel, true).ok);

    // Older releases have none: not an error.
    Release old;
    old.version = V("2.1.0");
    auto missing = fetchManifest(old, true);
    CHECK(!missing.ok);
    CHECK(missing.missing);
}

TEST_CASE("app package: installed per user only when every file matches")
{
    TempDir t;
    const fs::path staging = t.path / "staging", root = t.path / "app", base = t.path / "Program Files" / "Nilesoft Shell";
    writeAll(staging / "NilesoftShellPro.exe", "new app");
    writeAll(staging / "imports" / "theme.nss", "theme");
    std::string error;
    auto m = parseManifest(manifestJson("2.2.0", "2.1.1", sha256File(staging / "NilesoftShellPro.exe")), error);
    REQUIRE(m);
    m->appFiles["imports/theme.nss"] = sha256File(staging / "imports" / "theme.nss");

    fs::path installed;
    auto r = installAppPackage(staging, *m, root, base, &installed);
    REQUIRE_MESSAGE(r.ok, r.error);
    CHECK(installed == root / "2.2.0");
    CHECK(readAll(installed / "NilesoftShellPro.exe") == "new app");
    CHECK(readAll(installed / "imports" / "theme.nss") == "theme");
    CHECK(!fs::exists(root / "2.2.0.partial"));
    auto rec = appchannel::readRecord(installed);
    REQUIRE(rec);
    CHECK(rec->version.str() == "2.2.0");
    CHECK(rec->base == base);
    CHECK(rec->exeHash() == sha256File(installed / "NilesoftShellPro.exe"));

    // Installing the same version again replaces it.
    CHECK(installAppPackage(staging, *m, root, base).ok);

    SUBCASE("a file that doesn't match")
    {
        writeAll(staging / "imports" / "theme.nss", "tampered");
        fs::remove_all(root);
        CHECK(!installAppPackage(staging, *m, root, base).ok);
        CHECK(!fs::exists(root / "2.2.0"));
        CHECK(!fs::exists(root / "2.2.0.partial"));
    }
    SUBCASE("a file update.json doesn't list")
    {
        writeAll(staging / "extra.dll", "x");
        fs::remove_all(root);
        CHECK(!installAppPackage(staging, *m, root, base).ok);
        CHECK(!fs::exists(root / "2.2.0"));
    }
    SUBCASE("a missing file")
    {
        fs::remove(staging / "imports" / "theme.nss");
        fs::remove_all(root);
        CHECK(!installAppPackage(staging, *m, root, base).ok);
    }
}

TEST_CASE("handoff: which per-user copy the installed exe starts")
{
    TempDir t;
    const fs::path root = t.path / "app", base = t.path / "Program Files" / "Nilesoft Shell";
    const auto running = V("2.1.1");

    auto verdictOf = [](const appchannel::Decision &d, const fs::path &dir) {
        for (const auto &[p, v] : d.verdicts)
            if (p == dir)
                return v;
        FAIL("no verdict for " << dir.string());
        return appchannel::Verdict::OtherInstall;
    };

    SUBCASE("none: the installed copy runs")
    {
        auto d = appchannel::decide(running, base, false, appchannel::scan(root), hashOf);
        CHECK(!d.exe);
        CHECK(d.verdicts.empty());
    }
    SUBCASE("the newest newer copy; older ones go")
    {
        auto older = makeCopy(root, "2.1.0", base);
        auto same = makeCopy(root, "2.1.1", base);
        auto newer = makeCopy(root, "2.2.0", base);
        auto newest = makeCopy(root, "2.3.0-beta.1", base);
        auto d = appchannel::decide(running, base, false, appchannel::scan(root), hashOf);
        REQUIRE(d.exe);
        CHECK(*d.exe == newest / "NilesoftShellPro.exe");
        CHECK(d.version.str() == "2.3.0-beta.1");
        CHECK(verdictOf(d, older) == appchannel::Verdict::Older);
        CHECK(verdictOf(d, same) == appchannel::Verdict::Older);
        CHECK(verdictOf(d, newer) == appchannel::Verdict::Older);
        CHECK(verdictOf(d, newest) == appchannel::Verdict::Use);
        CHECK(d.remove().size() == 3);
    }
    SUBCASE("a newer MSI was installed since: every per-user copy is older and goes")
    {
        auto a = makeCopy(root, "2.2.0", base);
        auto d = appchannel::decide(V("2.3.0"), base, false, appchannel::scan(root), hashOf);
        CHECK(!d.exe);
        CHECK(verdictOf(d, a) == appchannel::Verdict::Older);
    }
    SUBCASE("a damaged copy is skipped for the next good one")
    {
        auto good = makeCopy(root, "2.2.0", base);
        auto damaged = makeCopy(root, "2.3.0", base);
        writeAll(damaged / "NilesoftShellPro.exe", "changed");
        auto missingExe = makeCopy(root, "2.4.0", base);
        fs::remove(missingExe / "NilesoftShellPro.exe");
        auto noRecord = root / "2.5.0";
        writeAll(noRecord / "NilesoftShellPro.exe", "x");
        auto misnamed = makeCopy(root, "2.6.0", base);
        fs::rename(misnamed, root / "2.7.0");
        auto d = appchannel::decide(running, base, false, appchannel::scan(root), hashOf);
        REQUIRE(d.exe);
        CHECK(*d.exe == good / "NilesoftShellPro.exe");
        CHECK(verdictOf(d, damaged) == appchannel::Verdict::Damaged);
        CHECK(verdictOf(d, missingExe) == appchannel::Verdict::Damaged);
        CHECK(verdictOf(d, noRecord) == appchannel::Verdict::Damaged);
        CHECK(verdictOf(d, root / "2.7.0") == appchannel::Verdict::Damaged);
    }
    SUBCASE("a copy that failed to start is not used again")
    {
        auto good = makeCopy(root, "2.2.0", base);
        auto failed = makeCopy(root, "2.3.0", base);
        writeAll(failed / appchannel::kFailedName, "exited with code 0xC0000005");
        auto d = appchannel::decide(running, base, false, appchannel::scan(root), hashOf);
        REQUIRE(d.exe);
        CHECK(*d.exe == good / "NilesoftShellPro.exe");
        CHECK(verdictOf(d, failed) == appchannel::Verdict::Failed);
    }
    SUBCASE("copies of another install are left alone")
    {
        auto other = makeCopy(root, "2.2.0", t.path / "portable");
        auto d = appchannel::decide(running, base, false, appchannel::scan(root), hashOf);
        CHECK(!d.exe);
        CHECK(verdictOf(d, other) == appchannel::Verdict::OtherInstall);
        CHECK(d.remove().empty());
    }
    SUBCASE("the install folder compares without case or a trailing separator")
    {
        auto mine = makeCopy(root, "2.2.0", base);
        auto upper = base.wstring();
        std::transform(upper.begin(), upper.end(), upper.begin(), ::towupper);
        auto d = appchannel::decide(running, fs::path(upper + L"\\"), false, appchannel::scan(root), hashOf);
        REQUIRE(d.exe);
        CHECK(*d.exe == mine / "NilesoftShellPro.exe");
    }
    SUBCASE("elevated: nothing is run or removed")
    {
        makeCopy(root, "2.2.0", base);
        makeCopy(root, "2.0.0", base);
        auto d = appchannel::decide(running, base, true, appchannel::scan(root), hashOf);
        CHECK(!d.exe);
        CHECK(d.remove().empty());
    }
    SUBCASE("a .partial folder is not a candidate")
    {
        writeAll(root / "2.2.0.partial" / "NilesoftShellPro.exe", "half");
        CHECK(appchannel::scan(root).empty());
    }
}

TEST_CASE("handoff: the arguments are passed on unchanged")
{
    // Not raw strings inside CHECK(): MSVC's preprocessor mangles them in macro arguments.
    const std::wstring quoted = L"\"C:\\Program Files\\Nilesoft Shell\\NilesoftShellPro.exe\" --page home";
    const std::wstring alone = L"\"C:\\x y\\a.exe\"";
    const std::wstring spaced = L"a.exe   --lang \"zh-CN\" --po \"C:\\a b\\x.po\"";
    const std::wstring spacedArgs = L"--lang \"zh-CN\" --po \"C:\\a b\\x.po\"";
    CHECK(appchannel::argumentsOf(quoted) == L"--page home");
    CHECK(appchannel::argumentsOf(alone) == L"");
    CHECK(appchannel::argumentsOf(spaced) == spacedArgs);
    CHECK(appchannel::argumentsOf(L"a.exe") == L"");
    CHECK(appchannel::argumentsOf(L"") == L"");
}

TEST_CASE("install.json round trip")
{
    appchannel::Record r;
    r.version = V("2.2.0-beta.1");
    r.base = LR"(C:\Program Files\Nilesoft Shell)";
    r.files["NilesoftShellPro.exe"] = H1;
    r.files["imports/theme.nss"] = H2;
    auto back = appchannel::parseRecord(appchannel::recordJson(r));
    REQUIRE(back);
    CHECK(back->version.str() == "2.2.0-beta.1");
    CHECK(back->base == r.base);
    CHECK(back->exeHash() == H1);
    CHECK(!appchannel::parseRecord(R"({"version":"2.2.0","base":"C:\\x","files":{}})")); // no exe
    CHECK(!appchannel::parseRecord(R"({"version":"x","base":"C:\\x","files":{"NilesoftShellPro.exe":"a"}})"));
    CHECK(!appchannel::parseRecord("[]"));
}

TEST_CASE("cleanup: older per-user copies, leftovers, not other installs")
{
    TempDir t;
    LocalAppData redirect(t.path);
    const fs::path root = appchannel::appRoot();
    REQUIRE(root == t.path / "Programs" / "Nilesoft Shell Pro" / "app");
    // This test exe is not a per-user copy: its own folder is the install folder.
    const fs::path base = appchannel::baseDir();
    REQUIRE(!appchannel::isPerUserCopy());

    auto older = makeCopy(root, "1.0.0", base);
    auto newer = makeCopy(root, "9.0.0", base);
    auto other = makeCopy(root, "0.5.0", t.path / "elsewhere");
    writeAll(root / "9.1.0.partial" / "NilesoftShellPro.exe", "half");
    writeAll(root / "8.0.0" / "NilesoftShellPro.exe", "no install.json");

    appchannel::cleanup(V("2.0.0"));
    CHECK(!fs::exists(older));
    CHECK(fs::exists(newer));
    CHECK(fs::exists(other));
    CHECK(!fs::exists(root / "9.1.0.partial"));
    CHECK(!fs::exists(root / "8.0.0"));

    // The updates folder follows %LOCALAPPDATA% too.
    CHECK(updatesRoot() == t.path / "Nilesoft" / "Shell" / "updates");
}

TEST_CASE("config upgrade from a per-user copy's own shipped files")
{
    TempDir t;
    const fs::path shipped = t.path / "app" / "2.2.0", config = t.path / "Roaming" / "Nilesoft" / "Shell";
    writeAll(shipped / "shell.nss", "// shipped 2.2.0\nimport 'imports/theme.nss'\n");
    writeAll(shipped / "imports" / "theme.nss", "theme { }\n");
    writeAll(config / "shell.nss", "// mine\nimport 'imports/theme.nss'\n");

    auto r = config_update::runLocal(shipped.wstring(), (config / "shell.nss").wstring());
    REQUIRE_MESSAGE(r.ok, r.error);
    CHECK(text::widen(r.installDir) == shipped.wstring());
    // The import the user's config refers to arrives; the edited root stays.
    CHECK(std::find(r.added.begin(), r.added.end(), "imports/theme.nss") != r.added.end());
    CHECK(std::find(r.modifiedKept.begin(), r.modifiedKept.end(), "shell.nss") != r.modifiedKept.end());
    CHECK(readAll(config / "imports" / "theme.nss") == "theme { }\n");
    CHECK(readAll(config / "shell.nss") == "// mine\nimport 'imports/theme.nss'\n");

    // "Use the new version" for the kept file.
    auto take = config_update::runLocal(shipped.wstring(), (config / "shell.nss").wstring(), { "shell.nss" });
    REQUIRE(take.ok);
    CHECK(std::find(take.updated.begin(), take.updated.end(), "shell.nss") != take.updated.end());

    // The shared config (the shipped folder itself) is never upgraded.
    auto none = config_update::runLocal(shipped.wstring(), (shipped / "shell.nss").wstring());
    CHECK(none.configDir.empty());
}
// --- a start that finds the app running already --------------------------------------------

TEST_CASE("instance window title: carries the running version")
{
    using namespace appchannel;
    CHECK(instanceTitle(V("2.2.2-beta.1")) == L"Nilesoft Shell Pro background 2.2.2-beta.1");
    CHECK(instanceVersion(instanceTitle(V("2.2.2"))).value().str() == "2.2.2");
    CHECK(instanceVersion(instanceTitle(V("2.3.0-rc.2"))).value().str() == "2.3.0-rc.2");
    // 2.2.0 to 2.2.1: no version in the title
    CHECK_FALSE(instanceVersion(L"Nilesoft Shell Pro background").has_value());
    CHECK_FALSE(instanceVersion(L"Nilesoft Shell Pro background ").has_value());
    CHECK_FALSE(instanceVersion(L"Nilesoft Shell Pro background x.y").has_value());
    CHECK_FALSE(instanceVersion(L"").has_value());
}

TEST_CASE("single instance: a newer start replaces an older running copy, never the other way")
{
    using appchannel::Existing;
    using appchannel::onExisting;
    // the user's case: 2.2.1-beta.1 kept in the tray, 2.2.1 (and later) started from the Start menu
    CHECK(onExisting(V("2.2.2"), V("2.2.1-beta.1"), true) == Existing::Replace);
    CHECK(onExisting(V("2.2.2"), V("2.2.2-beta.1"), true) == Existing::Replace); // pre-release is older
    CHECK(onExisting(V("2.3.0"), V("2.2.9"), true) == Existing::Replace);
    // the same version, or a newer one running: show that one, as before
    CHECK(onExisting(V("2.2.2"), V("2.2.2"), true) == Existing::Activate);
    CHECK(onExisting(V("2.2.2-beta.1"), V("2.2.2"), true) == Existing::Activate);
    CHECK(onExisting(V("2.2.1"), V("2.2.2"), true) == Existing::Activate);
    // an instance window without a version: 2.2.0 to 2.2.1, older than any build that asks
    CHECK(onExisting(V("2.2.2-beta.1"), std::nullopt, true) == Existing::Replace);
    // no instance window: a version from before the tray (or still starting): leave it
    CHECK(onExisting(V("2.2.2"), std::nullopt, false) == Existing::Activate);
}

TEST_CASE("an app update installed for the user and waiting for a restart is not offered again")
{
    TempDir tmp;
    const fs::path base = tmp.path / "Program Files" / "Nilesoft Shell";
    auto copy = [&](const char *version) {
        appchannel::Candidate c;
        c.dir = tmp.path / "app" / version;
        appchannel::Record r;
        r.version = V(version);
        r.base = base;
        r.files["NilesoftShellPro.exe"] = std::string("hash-") + version;
        c.record = r;
        return c;
    };
    auto hashOf = [](const fs::path &exe) { return "hash-" + text::narrow(exe.parent_path().filename().wstring()); };
    auto pending = [&](const char *running, std::vector<appchannel::Candidate> cs) {
        return appchannel::decide(V(running), base, false, cs, hashOf);
    };

    // 2.2.1-beta.1 runs (in the tray), 2.2.1 is installed for the user: Restart, no download
    {
        auto d = pending("2.2.1-beta.1", { copy("2.2.1") });
        auto dir = appchannel::installedFor(V("2.2.1"), d);
        REQUIRE(dir.has_value());
        CHECK(dir->filename() == "2.2.1");
    }
    // a newer copy than the offer counts too (the restart runs the newest)
    CHECK(appchannel::installedFor(V("2.2.1"), pending("2.2.0", { copy("2.2.2") })).has_value());
    // an older copy than the offer: download the offer
    CHECK_FALSE(appchannel::installedFor(V("2.2.2"), pending("2.2.0", { copy("2.2.1") })).has_value());
    // nothing installed for the user
    CHECK_FALSE(appchannel::installedFor(V("2.2.1"), pending("2.2.1-beta.1", {})).has_value());
    // the running version itself (this is the per-user copy): nothing pending
    CHECK_FALSE(appchannel::installedFor(V("2.2.1"), pending("2.2.1", { copy("2.2.1") })).has_value());
    // a damaged copy (the exe's hash differs) or one that failed to start: download again
    {
        auto c = copy("2.2.1");
        c.record->files["NilesoftShellPro.exe"] = "other";
        CHECK_FALSE(appchannel::installedFor(V("2.2.1"), pending("2.2.1-beta.1", { c })).has_value());
        auto f = copy("2.2.1");
        f.failed = true;
        CHECK_FALSE(appchannel::installedFor(V("2.2.1"), pending("2.2.1-beta.1", { f })).has_value());
    }
    // a copy that belongs to another install folder
    {
        auto c = copy("2.2.1");
        c.record->base = tmp.path / "elsewhere";
        CHECK_FALSE(appchannel::installedFor(V("2.2.1"), pending("2.2.1-beta.1", { c })).has_value());
    }
}
