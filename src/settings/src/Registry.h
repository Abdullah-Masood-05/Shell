#pragma once

// HKCU helpers: the Windows theme and accent, the registered shell.dll, and the app's
// own settings under HKCU\Software\Nilesoft\Shell\Settings.

#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace registry {

std::optional<DWORD> readDword(HKEY root, const wchar_t *key, const wchar_t *name);
std::optional<std::wstring> readString(HKEY root, const wchar_t *key, const wchar_t *name);
bool writeDword(HKEY root, const wchar_t *key, const wchar_t *name, DWORD value);

// HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize: AppsUseLightTheme.
// Defaults to true (light) when the value is missing, like Windows.
bool appsUseLightTheme();

// The accent colour as 0xRRGGBB. HKCU\Software\Microsoft\Windows\DWM: AccentColor (ABGR).
std::optional<uint32_t> accentColor();

// shell.dll paths registered for the context-menu CLSID, HKCU first, then HKLM.
std::vector<std::wstring> registeredDllPaths();

// HKCU\Software\Nilesoft\Shell\Settings
namespace app {
inline constexpr const wchar_t *key = L"Software\\Nilesoft\\Shell\\Settings";

std::optional<DWORD> get(const wchar_t *name);
bool set(const wchar_t *name, DWORD value);

struct Placement
{
    int x = 0, y = 0, width = 0, height = 0; // physical pixels
    bool maximized = false;
};
std::optional<Placement> loadPlacement();
void savePlacement(const Placement &p, bool keepNormalRect);
} // namespace app

} // namespace registry
