#include "AppChannel.h"

#include "Text.h"

#include <nlohmann/json.hpp>

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <iterator>

#ifndef SETTINGS_VERSION
#define SETTINGS_VERSION "0.0.0"
#endif

namespace fs = std::filesystem;
using text::narrow;
using text::widen;
using updater::Version;

namespace appchannel {

namespace {

constexpr const char *kExeName = "NilesoftShellPro.exe";

std::wstring folderKey(const fs::path &p)
{
    auto s = p.lexically_normal().wstring();
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return wchar_t(std::towlower(c)); });
    std::replace(s.begin(), s.end(), L'/', L'\\');
    while (s.size() > 3 && s.back() == L'\\')
        s.pop_back();
    return s;
}

bool sameFolder(const fs::path &a, const fs::path &b) { return !a.empty() && folderKey(a) == folderKey(b); }

std::wstring env(const wchar_t *name)
{
    wchar_t buf[1024];
    DWORD n = GetEnvironmentVariableW(name, buf, DWORD(std::size(buf)));
    return n > 0 && n < std::size(buf) ? std::wstring(buf, n) : std::wstring{};
}

bool endsWithI(const std::wstring &s, const std::wstring &suffix)
{
    return s.size() >= suffix.size() && _wcsicmp(s.c_str() + s.size() - suffix.size(), suffix.c_str()) == 0;
}

void removeTree(const fs::path &p)
{
    std::error_code ec;
    fs::remove_all(p, ec);
}

} // namespace

fs::path localAppData()
{
    auto e = env(L"LOCALAPPDATA");
    if (!e.empty())
        return fs::path(e);
    PWSTR local = nullptr;
    fs::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)) && local)
        out = local;
    CoTaskMemFree(local);
    return out;
}

std::wstring instanceSuffix()
{
    auto id = env(kInstanceEnv);
    return id.empty() ? std::wstring() : L"." + id;
}

fs::path appRoot()
{
    auto local = localAppData();
    return local.empty() ? fs::path() : local / L"Programs" / L"Nilesoft Shell Pro" / L"app";
}

// --- install.json ----------------------------------------------------------------------

std::string Record::exeHash() const
{
    for (const auto &[path, hash] : files)
        if (_stricmp(path.c_str(), kExeName) == 0)
            return hash;
    return {};
}

std::string recordJson(const Record &r)
{
    nlohmann::json j;
    j["version"] = r.version.str();
    j["base"] = narrow(r.base.wstring());
    j["files"] = nlohmann::json::object();
    for (const auto &[path, hash] : r.files)
        j["files"][path] = hash;
    return j.dump(2);
}

std::optional<Record> parseRecord(const std::string &json)
{
    auto j = nlohmann::json::parse(json, nullptr, false);
    if (!j.is_object())
        return std::nullopt;
    auto str = [&](const char *key) {
        auto it = j.find(key);
        return it != j.end() && it->is_string() ? it->get<std::string>() : std::string{};
    };
    auto v = Version::parse(str("version"));
    auto base = str("base");
    if (!v || base.empty())
        return std::nullopt;
    Record r;
    r.version = *v;
    r.base = fs::path(widen(base));
    if (auto f = j.find("files"); f != j.end() && f->is_object())
        for (auto it = f->begin(); it != f->end(); ++it)
            if (it.value().is_string())
                r.files[it.key()] = it.value().get<std::string>();
    if (r.exeHash().empty())
        return std::nullopt;
    return r;
}

std::optional<Record> readRecord(const fs::path &dir)
{
    std::ifstream in(dir / kRecordName, std::ios::binary);
    if (!in)
        return std::nullopt;
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return parseRecord(s);
}

bool writeRecord(const fs::path &dir, const Record &r)
{
    std::ofstream out(dir / kRecordName, std::ios::binary | std::ios::trunc);
    out << recordJson(r);
    out.close();
    return bool(out);
}

// --- choosing a copy ---------------------------------------------------------------------

std::vector<Candidate> scan(const fs::path &root)
{
    std::vector<Candidate> out;
    std::error_code ec;
    if (root.empty() || !fs::is_directory(root, ec))
        return out;
    for (const auto &e : fs::directory_iterator(root, ec)) {
        if (!e.is_directory(ec) || endsWithI(e.path().filename().wstring(), L".partial"))
            continue;
        Candidate c;
        c.dir = e.path();
        c.record = readRecord(e.path());
        c.failed = fs::exists(e.path() / kFailedName, ec);
        out.push_back(std::move(c));
    }
    return out;
}

std::vector<fs::path> Decision::remove() const
{
    std::vector<fs::path> out;
    for (const auto &[dir, v] : verdicts)
        if (v == Verdict::Older || v == Verdict::Damaged || v == Verdict::Failed)
            out.push_back(dir);
    return out;
}

Decision decide(const Version &running, const fs::path &baseDir, bool elevated, const std::vector<Candidate> &candidates,
                const std::function<std::string(const fs::path &)> &hashOf)
{
    Decision d;
    if (elevated)
        return d; // the per-user folder is not an administrator's to run or to clean
    std::vector<const Candidate *> good;
    for (const auto &c : candidates) {
        auto named = Version::parse(narrow(c.dir.filename().wstring()));
        if (!c.record || !named || !(*named == c.record->version)) {
            d.verdicts.push_back({ c.dir, Verdict::Damaged });
            continue;
        }
        if (!sameFolder(c.record->base, baseDir)) {
            d.verdicts.push_back({ c.dir, Verdict::OtherInstall });
            continue;
        }
        if (c.failed) {
            d.verdicts.push_back({ c.dir, Verdict::Failed });
            continue;
        }
        if (updater::compare(c.record->version, running) <= 0) {
            d.verdicts.push_back({ c.dir, Verdict::Older });
            continue;
        }
        good.push_back(&c);
    }
    std::sort(good.begin(), good.end(), [](const Candidate *a, const Candidate *b) {
        return updater::compare(b->record->version, a->record->version) < 0;
    });
    for (const auto *c : good) {
        if (d.exe) {
            d.verdicts.push_back({ c->dir, Verdict::Older });
            continue;
        }
        const auto exe = c->dir / kExeName;
        const auto hash = hashOf ? hashOf(exe) : std::string();
        if (hash.empty() || _stricmp(hash.c_str(), c->record->exeHash().c_str()) != 0) {
            d.verdicts.push_back({ c->dir, Verdict::Damaged });
            continue;
        }
        d.exe = exe;
        d.version = c->record->version;
        d.verdicts.push_back({ c->dir, Verdict::Use });
    }
    return d;
}

std::optional<fs::path> installedFor(const Version &offered, const Decision &pending)
{
    if (!pending.exe || updater::compare(pending.version, offered) < 0)
        return std::nullopt;
    return pending.exe->parent_path();
}

Decision pendingCopy(const Version &running, const fs::path &baseDir)
{
    return decide(running, baseDir, false, scan(appRoot()), [](const fs::path &p) { return updater::sha256File(p); });
}

namespace {
constexpr const wchar_t *kInstanceTitle = L"Nilesoft Shell Pro background";
}

std::wstring instanceTitle(const Version &v) { return std::wstring(kInstanceTitle) + L" " + widen(v.str()); }

std::optional<Version> instanceVersion(const std::wstring &title)
{
    const std::wstring prefix = std::wstring(kInstanceTitle) + L" ";
    if (title.size() <= prefix.size() || title.compare(0, prefix.size(), prefix) != 0)
        return std::nullopt;
    return Version::parse(narrow(title.substr(prefix.size())));
}

Existing onExisting(const Version &self, const std::optional<Version> &running, bool instanceWindow)
{
    if (!instanceWindow)
        return Existing::Activate;
    // No version in the title: 2.2.0 to 2.2.1, older than any copy that asks.
    if (!running || updater::compare(self, *running) > 0)
        return Existing::Replace;
    return Existing::Activate;
}

std::wstring argumentsOf(const std::wstring &cmd)
{
    size_t i = 0;
    if (!cmd.empty() && cmd[0] == L'"') {
        i = cmd.find(L'"', 1);
        i = i == std::wstring::npos ? cmd.size() : i + 1;
    } else {
        while (i < cmd.size() && cmd[i] != L' ' && cmd[i] != L'\t')
            ++i;
    }
    while (i < cmd.size() && (cmd[i] == L' ' || cmd[i] == L'\t'))
        ++i;
    return cmd.substr(i);
}

// --- this process ------------------------------------------------------------------------

fs::path selfPath()
{
    std::wstring buf(MAX_PATH, L'\0');
    while (true) {
        DWORD n = GetModuleFileNameW(nullptr, buf.data(), DWORD(buf.size()));
        if (n == 0)
            return {};
        if (n < buf.size()) {
            buf.resize(n);
            return fs::path(buf);
        }
        buf.resize(buf.size() * 2);
    }
}

const std::optional<Record> &selfRecord()
{
    static const std::optional<Record> rec = [] {
        auto dir = selfPath().parent_path();
        auto r = readRecord(dir);
        // Only a copy that belongs to another folder: an install.json in the install
        // folder itself means nothing.
        if (r && sameFolder(r->base, dir))
            r.reset();
        return r;
    }();
    return rec;
}

bool isPerUserCopy() { return selfRecord().has_value(); }

fs::path baseDir() { return isPerUserCopy() ? selfRecord()->base : selfPath().parent_path(); }

fs::path launcherPath() { return baseDir() / kExeName; }

std::optional<Version> productVersion(const fs::path &exe)
{
    DWORD handle = 0;
    DWORD size = GetFileVersionInfoSizeW(exe.c_str(), &handle);
    if (size == 0)
        return std::nullopt;
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(exe.c_str(), 0, size, data.data()))
        return std::nullopt;
    struct Lang
    {
        WORD language, codePage;
    } *langs = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void **>(&langs), &len) || len < sizeof(Lang))
        return std::nullopt;
    wchar_t key[64];
    swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\ProductVersion", langs[0].language, langs[0].codePage);
    wchar_t *value = nullptr;
    if (!VerQueryValueW(data.data(), key, reinterpret_cast<void **>(&value), &len) || !value || len == 0)
        return std::nullopt;
    return Version::parse(narrow(std::wstring(value)));
}

std::optional<Version> installedRelease()
{
    if (isPerUserCopy())
        return productVersion(launcherPath());
    return Version::parse(SETTINGS_VERSION);
}

bool isElevated()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    TOKEN_ELEVATION e{};
    DWORD size = 0;
    bool elevated = GetTokenInformation(token, TokenElevation, &e, sizeof e, &size) && e.TokenIsElevated;
    CloseHandle(token);
    return elevated;
}

int handOff(const Version &running)
{
    if (!env(kNoHandoffEnv).empty() || !env(kHandoffEnv).empty() || isPerUserCopy())
        return -1;
    const auto root = appRoot();
    std::error_code ec;
    if (root.empty() || !fs::is_directory(root, ec))
        return -1;
    const auto d = decide(running, selfPath().parent_path(), isElevated(), scan(root),
                          [](const fs::path &p) { return updater::sha256File(p); });
    for (const auto &p : d.remove())
        removeTree(p);
    if (!d.exe)
        return -1;
    const fs::path exe = *d.exe;
    const fs::path dir = exe.parent_path();
    auto markFailed = [&](const std::string &why) {
        std::ofstream(dir / kFailedName, std::ios::binary) << why << "\r\n";
        removeTree(dir); // works once nothing runs from it; else the marker stays
    };

    const std::wstring name = L"Local\\NilesoftShellPro.Handoff." + std::to_wstring(GetCurrentProcessId()) + L"." +
                              std::to_wstring(GetTickCount64());
    HANDLE started = CreateEventW(nullptr, TRUE, FALSE, name.c_str());
    if (!started)
        return -1;
    SetEnvironmentVariableW(kHandoffEnv, name.c_str());
    std::wstring cmd = L"\"" + exe.wstring() + L"\"";
    const auto args = argumentsOf(GetCommandLineW());
    if (!args.empty())
        cmd += L" " + args;
    STARTUPINFOW mine{ sizeof mine };
    GetStartupInfoW(&mine);
    STARTUPINFOW si{ sizeof si };
    si.dwFlags = mine.dwFlags & STARTF_USESHOWWINDOW;
    si.wShowWindow = mine.wShowWindow;
    PROCESS_INFORMATION pi{};
    AllowSetForegroundWindow(ASFW_ANY);
    const bool ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi) != FALSE;
    const DWORD err = ok ? 0 : GetLastError();
    SetEnvironmentVariableW(kHandoffEnv, nullptr);
    if (!ok) {
        CloseHandle(started);
        markFailed("CreateProcess failed: " + std::to_string(err));
        return -1;
    }
    CloseHandle(pi.hThread);
    HANDLE waits[2] = { started, pi.hProcess };
    DWORD w = WaitForMultipleObjects(2, waits, FALSE, 30000);
    if (w == WAIT_OBJECT_0 + 1 && WaitForSingleObject(started, 0) != WAIT_OBJECT_0) {
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        if (code != 0) {
            // It ended before its window was up: run the installed copy, and not that one again.
            CloseHandle(pi.hProcess);
            CloseHandle(started);
            char hex[16];
            sprintf_s(hex, "0x%08lX", code);
            markFailed(std::string("exited with code ") + hex);
            return -1;
        }
    }
    // Signalled, or still starting after 30 seconds (it is running: not a second window).
    CloseHandle(pi.hProcess);
    CloseHandle(started);
    return 0;
}

void signalStarted()
{
    auto name = env(kHandoffEnv);
    if (name.empty())
        return;
    if (HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, name.c_str())) {
        SetEvent(ev);
        CloseHandle(ev);
    }
    // Programs this one starts are not part of the handoff.
    SetEnvironmentVariableW(kHandoffEnv, nullptr);
}

void cleanup(const Version &running)
{
    const auto root = appRoot();
    std::error_code ec;
    if (root.empty() || !fs::is_directory(root, ec))
        return;
    const auto self = selfPath().parent_path();
    const auto base = baseDir();
    const auto installed = installedRelease();
    std::vector<fs::path> gone;
    for (const auto &e : fs::directory_iterator(root, ec)) {
        if (!e.is_directory(ec) || sameFolder(e.path(), self))
            continue;
        if (endsWithI(e.path().filename().wstring(), L".partial")) {
            gone.push_back(e.path()); // an install that never finished (only this app installs)
            continue;
        }
        auto rec = readRecord(e.path());
        if (!rec) {
            gone.push_back(e.path());
            continue;
        }
        if (!sameFolder(rec->base, base))
            continue;
        if (updater::compare(rec->version, running) < 0 || (installed && updater::compare(rec->version, *installed) <= 0))
            gone.push_back(e.path());
    }
    for (const auto &p : gone)
        removeTree(p);
}

} // namespace appchannel
