#include "Background.h"

#include <cwctype>
#include <vector>

namespace startup {

namespace {

constexpr const wchar_t *kSettingsKey = L"Software\\Nilesoft\\Shell\\Settings";

bool equalNoCase(const std::wstring &a, const std::wstring &b)
{
    return a.size() == b.size() && _wcsicmp(a.c_str(), b.c_str()) == 0;
}

std::wstring trim(const std::wstring &s)
{
    size_t b = 0, e = s.size();
    while (b < e && std::iswspace(s[b]))
        ++b;
    while (e > b && std::iswspace(s[e - 1]))
        --e;
    return s.substr(b, e - b);
}

} // namespace

// --- the real registry -------------------------------------------------------------------

std::optional<DWORD> RegistryStore::pref(const wchar_t *name)
{
    DWORD value = 0, size = sizeof value;
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        return std::nullopt;
    return value;
}

bool RegistryStore::setPref(const wchar_t *name, DWORD value)
{
    return RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, name, REG_DWORD, &value, sizeof value) == ERROR_SUCCESS;
}

std::optional<std::wstring> RegistryStore::runValue()
{
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND,
                     nullptr, nullptr, &size) != ERROR_SUCCESS)
        return std::nullopt;
    std::wstring s(size / sizeof(wchar_t) + 1, L'\0');
    size = DWORD(s.size() * sizeof(wchar_t));
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND,
                     nullptr, s.data(), &size) != ERROR_SUCCESS)
        return std::nullopt;
    s.resize(wcsnlen(s.c_str(), s.size()));
    return s;
}

bool RegistryStore::setRunValue(const std::wstring &command)
{
    return RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, REG_SZ, command.c_str(),
                           DWORD((command.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

bool RegistryStore::deleteRunValue()
{
    LSTATUS rc = RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue);
    return rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND;
}

// StartupApproved\Run holds a 12-byte binary value per entry; an odd first byte (03)
// means switched off, 02 on. No value: on.
bool RegistryStore::disabledInWindows()
{
    BYTE data[16]{};
    DWORD size = sizeof data;
    if (RegGetValueW(HKEY_CURRENT_USER, kApprovedKey, kRunValue, RRF_RT_REG_BINARY, nullptr, data, &size) != ERROR_SUCCESS ||
        size == 0)
        return false;
    return (data[0] & 1) != 0;
}

bool RegistryStore::clearDisabledInWindows()
{
    LSTATUS rc = RegDeleteKeyValueW(HKEY_CURRENT_USER, kApprovedKey, kRunValue);
    return rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND || rc == ERROR_PATH_NOT_FOUND;
}

bool RegistryStore::fileExists(const std::wstring &path)
{
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// --- the logic ----------------------------------------------------------------------------

Prefs load(Store &store)
{
    Prefs p;
    p.keepInTray = store.pref(kKeepInTray).value_or(1) != 0;
    p.startWithWindows = store.pref(kStartWithWindows).value_or(1) != 0;
    p.disabledInWindows = store.runValue().has_value() && store.disabledInWindows();
    return p;
}

std::wstring runCommand(const std::wstring &exe) { return L"\"" + exe + L"\" " + kBackgroundArg; }

std::wstring programOf(const std::wstring &command)
{
    std::wstring s = trim(command);
    if (s.empty())
        return {};
    if (s[0] == L'"') {
        size_t end = s.find(L'"', 1);
        return s.substr(1, end == std::wstring::npos ? std::wstring::npos : end - 1);
    }
    size_t args = s.find(L" -");
    return trim(s.substr(0, args));
}

bool isOurs(const std::wstring &command)
{
    std::wstring program = programOf(command);
    size_t slash = program.find_last_of(L"\\/");
    std::wstring name = slash == std::wstring::npos ? program : program.substr(slash + 1);
    return equalNoCase(name, L"NilesoftShellPro.exe");
}

Sync onStartup(Store &store, const std::wstring &exe)
{
    const Prefs p = load(store);
    const bool want = p.keepInTray && p.startWithWindows;
    const auto current = store.runValue();
    const bool ours = current && isOurs(*current);
    const bool firstTime = store.pref(kDefaultsApplied).value_or(0) == 0;
    if (firstTime)
        store.setPref(kDefaultsApplied, 1);

    if (!want) {
        if (ours && store.deleteRunValue())
            return Sync::Removed;
        return Sync::Unchanged;
    }
    if (firstTime && !current) // a Run value that isn't ours is left alone
        return store.setRunValue(runCommand(exe)) ? Sync::Created : Sync::Unchanged;
    if (ours) {
        const std::wstring program = programOf(*current);
        if (!equalNoCase(program, exe) && !store.fileExists(program))
            return store.setRunValue(runCommand(exe)) ? Sync::Repaired : Sync::Unchanged;
    }
    return Sync::Unchanged;
}

namespace {

void apply(Store &store, const std::wstring &exe)
{
    const Prefs p = load(store);
    store.setPref(kDefaultsApplied, 1);
    if (p.keepInTray && p.startWithWindows) {
        store.setRunValue(runCommand(exe));
        store.clearDisabledInWindows(); // the user asked for it here: on in Windows too
    } else if (auto current = store.runValue(); current && isOurs(*current)) {
        store.deleteRunValue();
    }
}

} // namespace

void setKeepInTray(Store &store, const std::wstring &exe, bool on)
{
    store.setPref(kKeepInTray, on ? 1 : 0);
    apply(store, exe);
}

void setStartWithWindows(Store &store, const std::wstring &exe, bool on)
{
    store.setPref(kStartWithWindows, on ? 1 : 0);
    apply(store, exe);
}

} // namespace startup

namespace background {

bool checkDue(std::time_t now, std::time_t lastCheck, std::chrono::seconds interval)
{
    return lastCheck <= 0 || lastCheck > now || now - lastCheck >= std::time_t(interval.count());
}

bool shouldNotify(const std::string &version, const std::string &lastNotified, const std::string &skipped)
{
    return !version.empty() && version != lastNotified && version != skipped;
}

} // namespace background
