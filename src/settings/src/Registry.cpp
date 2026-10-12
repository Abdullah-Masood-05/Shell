#include "Registry.h"

namespace registry {

namespace {
constexpr const wchar_t *kClsidInproc =
    L"Software\\Classes\\CLSID\\{BAE3934B-8A6A-4BFB-81BD-3FC599A1BAF1}\\InprocServer32";

uint32_t abgrToRgb(DWORD abgr)
{
    return ((abgr & 0xFF) << 16) | (abgr & 0xFF00) | ((abgr >> 16) & 0xFF);
}
} // namespace

std::optional<DWORD> readDword(HKEY root, const wchar_t *key, const wchar_t *name)
{
    DWORD value = 0, size = sizeof value;
    if (RegGetValueW(root, key, name, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        return std::nullopt;
    return value;
}

std::optional<std::wstring> readString(HKEY root, const wchar_t *key, const wchar_t *name)
{
    DWORD size = 0;
    constexpr DWORD flags = RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_SUBKEY_WOW6464KEY;
    if (RegGetValueW(root, key, name, flags, nullptr, nullptr, &size) != ERROR_SUCCESS)
        return std::nullopt;
    std::wstring s(size / sizeof(wchar_t) + 1, L'\0');
    size = DWORD(s.size() * sizeof(wchar_t));
    if (RegGetValueW(root, key, name, flags, nullptr, s.data(), &size) != ERROR_SUCCESS)
        return std::nullopt;
    s.resize(wcsnlen(s.c_str(), s.size()));
    return s;
}

bool writeDword(HKEY root, const wchar_t *key, const wchar_t *name, DWORD value)
{
    return RegSetKeyValueW(root, key, name, REG_DWORD, &value, sizeof value) == ERROR_SUCCESS;
}

bool appsUseLightTheme()
{
    auto v = readDword(HKEY_CURRENT_USER,
                       L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                       L"AppsUseLightTheme");
    return !v || *v != 0;
}

std::optional<uint32_t> accentColor()
{
    auto v = readDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor");
    if (!v)
        return std::nullopt;
    return abgrToRgb(*v);
}

std::optional<uint32_t> accentFor(bool dark)
{
    BYTE palette[32]{};
    DWORD size = sizeof palette;
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",
                     L"AccentPalette", RRF_RT_REG_BINARY, nullptr, palette, &size) == ERROR_SUCCESS &&
        size >= 32) {
        // Eight RGBA entries: light3, light2, light1, base, dark1, dark2, dark3, extra.
        const BYTE *c = palette + (dark ? 1 : 4) * 4;
        return (uint32_t(c[0]) << 16) | (uint32_t(c[1]) << 8) | c[2];
    }
    return accentColor();
}

std::vector<std::wstring> registeredDllPaths()
{
    std::vector<std::wstring> out;
    for (HKEY root : { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE }) {
        if (auto v = readString(root, kClsidInproc, nullptr); v && !v->empty()) {
            std::wstring expanded(MAX_PATH * 2, L'\0');
            DWORD n = ExpandEnvironmentStringsW(v->c_str(), expanded.data(), DWORD(expanded.size()));
            if (n > 0 && n <= expanded.size()) {
                expanded.resize(n - 1);
                out.push_back(expanded);
            } else {
                out.push_back(*v);
            }
        }
    }
    return out;
}

namespace app {

std::optional<DWORD> get(const wchar_t *name) { return readDword(HKEY_CURRENT_USER, key, name); }

bool set(const wchar_t *name, DWORD value) { return writeDword(HKEY_CURRENT_USER, key, name, value); }

std::optional<Placement> loadPlacement()
{
    auto x = get(L"WindowX"), y = get(L"WindowY"), w = get(L"WindowWidth"), h = get(L"WindowHeight");
    if (!x || !y || !w || !h)
        return std::nullopt;
    Placement p;
    p.x = int(*x);
    p.y = int(*y);
    p.width = int(*w);
    p.height = int(*h);
    p.maximized = get(L"WindowMaximized").value_or(0) != 0;
    if (p.width < 200 || p.height < 200)
        return std::nullopt;
    // Only restore onto a monitor that still exists.
    RECT r{ p.x, p.y, p.x + p.width, p.y + p.height };
    if (!MonitorFromRect(&r, MONITOR_DEFAULTTONULL))
        return std::nullopt;
    return p;
}

void savePlacement(const Placement &p, bool keepNormalRect)
{
    set(L"WindowMaximized", p.maximized ? 1 : 0);
    if (keepNormalRect)
        return;
    set(L"WindowX", DWORD(p.x));
    set(L"WindowY", DWORD(p.y));
    set(L"WindowWidth", DWORD(p.width));
    set(L"WindowHeight", DWORD(p.height));
}

} // namespace app
} // namespace registry
