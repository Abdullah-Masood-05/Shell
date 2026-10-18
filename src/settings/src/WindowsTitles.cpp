#include "WindowsTitles.h"

#include "Tr.h"

#include <windows.h>

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace wintitles {

namespace {

struct Entry
{
    const char *name;     // after id. / title.
    const wchar_t *module;
    unsigned menu;        // 0: a string resource; else the menu resource that holds item `res`
    unsigned res;
    const char *english;  // nsp::mark()ed when the app has a translation
    bool win7Only = false;
};

// Module and resource ids as in Initializer::load_mui (src/dll/src/Initializer.cpp).
// Where shell.dll reads a name from two places, the first one it tries is used.
constexpr Entry kEntries[] = {
    // ntshrui.dll (Windows 10 1511 and later)
    { "give_access_to", L"ntshrui.dll", 0, 103, nsp::mark("Give access to") },
    { "share_with", L"ntshrui.dll", 0, 104, "Share with" },
    { "share", L"ntshrui.dll", 0, 107, nsp::mark("Share") },
    { "open", L"appresolver.dll", 0, 8550, "Open" },
    { "run_as_administrator", L"appresolver.dll", 0, 8551, "Run as administrator" },
    { "open_in_new_window", L"windows.storage.dll", 0, 8517, "Open in new window" },
    { "open_in_new_tab", L"windows.storage.dll", 0, 8519, "Open in new tab" },
    { "troubleshoot_compatibility", L"acppage.dll", 0, 2022, "Troubleshoot compatibility" },

    // shell32.dll menus
    { "open_with", L"shell32.dll", 208, 1, "Open with" },
    { "create_shortcut", L"shell32.dll", 210, 16, nsp::mark("Create shortcut") },
    { "cut", L"shell32.dll", 210, 24, "Cut" },
    { "copy", L"shell32.dll", 210, 25, "Copy" },
    { "paste", L"shell32.dll", 210, 26, "Paste" },
    { "rename", L"shell32.dll", 210, 18, "Rename" },
    { "delete", L"shell32.dll", 210, 17, "Delete" },
    { "properties", L"shell32.dll", 211, 19, "Properties" },
    { "view", L"shell32.dll", 215, 28674, "View" },
    { "sort_by", L"shell32.dll", 215, 28673, "Sort by" },
    { "group_by", L"shell32.dll", 215, 28676, "Group by" },
    { "refresh", L"shell32.dll", 215, 28931, "Refresh" },
    { "customize_this_folder", L"shell32.dll", 215, 28722, nsp::mark("Customize this folder") },
    { "paste_shortcut", L"shell32.dll", 215, 28700, "Paste shortcut" },
    { "undo", L"shell32.dll", 215, 28699, "Undo" },
    { "redo", L"shell32.dll", 215, 28704, "Redo" },
    { "show_desktop_icons", L"shell32.dll", 218, 29698, "Show desktop icons" },
    { "send_to", L"shell32.dll", 223, 79, nsp::mark("Send to") },
    { "map_network_drive", L"shell32.dll", 225, 14, nsp::mark("Map network drive") },
    { "disconnect_network_drive", L"shell32.dll", 225, 9, nsp::mark("Disconnect network drive") },
    { "restore", L"shell32.dll", 226, 139, "Restore" },
    { "add_a_network_location", L"shell32.dll", 228, 1, "Add a network location" },
    { "format", L"shell32.dll", 247, 8, nsp::mark("Format") },
    { "eject", L"shell32.dll", 247, 10, nsp::mark("Eject") },
    { "erase_this_disc", L"shell32.dll", 247, 3, "Erase this disc" },
    { "include_in_library", L"shell32.dll", 262, 0, nsp::mark("Include in library") },

    // shell32.dll strings
    { "cast_to_device", L"shell32.dll", 0, 31289, nsp::mark("Cast to device") },
    { "copy_as_path", L"shell32.dll", 0, 30328, nsp::mark("Copy as path") },
    { "copy_path", L"shell32.dll", 0, 30329, "Copy path" },
    { "copy_to", L"shell32.dll", 0, 30339, "Copy to" },
    { "move_to", L"shell32.dll", 0, 30340, "Move to" },
    { "empty_recycle_bin", L"shell32.dll", 0, 10564, nsp::mark("Empty Recycle Bin") },
    { "extract_all", L"shell32.dll", 0, 37514, "Extract all" },
    { "open_file_location", L"shell32.dll", 0, 1033, "Open file location" },
    { "open_folder_location", L"shell32.dll", 0, 1040, "Open folder location" },
    { "open_powershell_window_here", L"shell32.dll", 0, 8508, nsp::mark("Open PowerShell window here") },
    { "open_command_window_here", L"shell32.dll", 0, 8506, "Open command window here" },
    { "pin_to_quick_access", L"shell32.dll", 0, 51377, nsp::mark("Pin to Quick access") },
    { "pin_current_folder_to_quick_access", L"shell32.dll", 0, 51388, nsp::mark("Pin current folder to Quick access") },
    { "pin_to_start", L"shell32.dll", 0, 51201, nsp::mark("Pin to Start") },
    { "pin_to_taskbar", L"shell32.dll", 0, 5386, nsp::mark("Pin to taskbar") },
    { "unpin_from_quick_access", L"shell32.dll", 0, 51379, nsp::mark("Unpin from Quick access") },
    { "unpin_from_start", L"shell32.dll", 0, 51394, nsp::mark("Unpin from Start") },
    { "unpin_from_taskbar", L"shell32.dll", 0, 5387, nsp::mark("Unpin from taskbar") },
    { "pin_to_start_menu", L"shell32.dll", 0, 5381, "Pin to Start menu", true },
    { "unpin_from_start_menu", L"shell32.dll", 0, 5382, "Unpin from Start menu", true },
    { "add_to_favorites", L"shell32.dll", 0, 51389, "Add to Favorites" },
    { "remove_from_favorites", L"shell32.dll", 0, 51390, "Remove from Favorites" },
    { "remove_from_recent", L"shell32.dll", 0, 51391, "Remove from Recent" },
    { "remove_from_quick_access", L"shell32.dll", 0, 51381, "Remove from Quick access" },
    { "print", L"shell32.dll", 0, 31250, nsp::mark("Print") },
    { "rotate_left", L"shell32.dll", 0, 37610, nsp::mark("Rotate left") },
    { "rotate_right", L"shell32.dll", 0, 37612, nsp::mark("Rotate right") },
    { "run_as_another_user", L"shell32.dll", 0, 37419, "Run as another user" },
    { "edit", L"shell32.dll", 0, 37398, "Edit" },
    { "new", L"shell32.dll", 0, 30315, "New" },
    { "new_folder", L"shell32.dll", 0, 16859, "New folder" },
    { "more_options", L"shell32.dll", 0, 31153, nsp::mark("More options") },
    { "play", L"shell32.dll", 0, 31283, "Play" },
    { "preview", L"shell32.dll", 0, 51260, "Preview" },
    { "mount", L"shell32.dll", 0, 31472, "Mount" },
    { "manage", L"shell32.dll", 0, 37602, "Manage" },
    { "permanently_delete", L"shell32.dll", 0, 37394, "Permanently delete" },
    { "command_prompt", L"shell32.dll", 0, 22022, "Command Prompt" },
    { "control_panel", L"shell32.dll", 0, 4161, "Control Panel" },
    { "device_manager", L"shell32.dll", 0, 24225, "Device Manager" },
    { "file_explorer_options", L"shell32.dll", 0, 22985, "File Explorer Options" },

    // other modules
    { "go_to", L"explorerframe.dll", 266, 33104, nsp::mark("Go to") },
    { "set_as_desktop_background", L"stobject.dll", 0, 417, nsp::mark("Set as desktop background") },
    { "next_desktop_background", L"stobject.dll", 0, 416, "Next desktop background" },
    { "restore_previous_versions", L"twext.dll", 0, 1037, nsp::mark("Restore previous versions") },
    { "turn_on_bitlocker", L"fvecpl.dll", 0, 1140, "Turn on BitLocker" },
    { "turn_off_bitlocker", L"fvecpl.dll", 0, 1141, "Turn off BitLocker" },
    { "display_settings", L"display.dll", 0, 4, "Display settings" },
    { "personalize", L"themecpl.dll", 0, 10, "Personalize" },
    { "settings", L"twinui.dll", 0, 5621, "Settings" },
    { "windows_powershell", L"twinui.pcshell.dll", 0, 10928, "Windows PowerShell" },
    { "windows_terminal", L"twinui.pcshell.dll", 0, 10944, "Windows Terminal" },
    { "terminal", L"twinui.pcshell.dll", 0, 10960, nsp::mark("Terminal") },
    { "burn_disc_image", L"isoburn.exe", 0, 351, "Burn disc image" },
    { "task_manager", L"explorer.exe", 205, 420, "Task Manager" },
    { "taskbar_settings", L"explorer.exe", 205, 413, "Taskbar settings" },
    { "show_the_desktop", L"explorer.exe", 205, 407, "Show the desktop" },
};

const Entry *find(std::string_view name)
{
    std::string low;
    for (char c : name)
        low.push_back(char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
    for (const auto &e : kEntries)
        if (low == e.name)
            return &e;
    return nullptr;
}

std::string narrow(const wchar_t *s, size_t n)
{
    if (n == 0)
        return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, s, int(n), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, int(n), out.data(), len, nullptr, nullptr);
    return out;
}

// Mapped as data only, from System32 or the Windows folder, never the search path
// (as shell.dll does).
HMODULE loadResources(const wchar_t *module)
{
    constexpr DWORD flags = LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE;
    if (auto h = LoadLibraryExW(module, nullptr, flags | LOAD_LIBRARY_SEARCH_SYSTEM32))
        return h;
    wchar_t dir[MAX_PATH]{};
    UINT n = GetWindowsDirectoryW(dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return nullptr;
    std::wstring path = std::wstring(dir) + L"\\" + module;
    return LoadLibraryExW(path.c_str(), nullptr, flags);
}

std::optional<std::string> read(const Entry &e)
{
    HMODULE h = loadResources(e.module);
    if (!h)
        return std::nullopt;
    std::optional<std::string> out;
    if (e.menu == 0) {
        const wchar_t *p = nullptr;
        int n = LoadStringW(h, e.res, reinterpret_cast<LPWSTR>(&p), 0);
        if (n > 0 && p)
            out = narrow(p, size_t(n));
    } else if (HMENU menu = LoadMenuW(h, MAKEINTRESOURCEW(e.menu))) {
        MENUITEMINFOW mii{ sizeof(mii), MIIM_STRING };
        if (GetMenuItemInfoW(menu, e.res, FALSE, &mii) && mii.cch > 0) {
            std::wstring buf(mii.cch + 1, L'\0');
            mii.dwTypeData = buf.data();
            mii.cch += 1;
            if (GetMenuItemInfoW(menu, e.res, FALSE, &mii))
                out = narrow(buf.c_str(), wcslen(buf.c_str()));
        }
        DestroyMenu(menu);
    }
    FreeLibrary(h);
    if (out) {
        *out = stripAccelerators(*out);
        if (out->empty())
            out.reset();
    }
    return out;
}

bool windowsShowsAppLanguage()
{
    static const std::string windows = [] {
        auto list = nsp::windowsLanguages();
        return list.empty() ? std::string("en") : list.front();
    }();
    const auto &app = nsp::currentLanguage();
    if (app.empty() || app == "en")
        return windows.size() >= 2 && (windows.compare(0, 2, "en") == 0 || windows.compare(0, 2, "EN") == 0);
    std::vector<std::string> pref{ windows }, available{ app };
    return nsp::resolveLanguage(pref, available) == app;
}

} // namespace

const char *englishTitle(std::string_view name)
{
    auto e = find(name);
    return e ? e->english : nullptr;
}

std::optional<std::string> systemTitle(std::string_view name)
{
    auto e = find(name);
    if (!e)
        return std::nullopt;
    if (e->win7Only)
        return std::string();
    static std::mutex lock;
    static std::map<const Entry *, std::optional<std::string>> cache;
    std::lock_guard guard(lock);
    auto it = cache.find(e);
    if (it == cache.end())
        it = cache.emplace(e, read(*e)).first;
    return it->second;
}

std::optional<std::string> displayTitle(std::string_view name)
{
    auto e = find(name);
    if (!e)
        return std::nullopt;
    if (e->win7Only)
        return std::string();
    if (!windowsShowsAppLanguage())
        return std::string(nsp::tr(e->english));
    if (auto t = systemTitle(name))
        return t;
    return std::string(nsp::tr(e->english));
}

std::string stripAccelerators(std::string_view title)
{
    std::string s(title);
    // A tab starts the shortcut text.
    if (auto tab = s.find('\t'); tab != std::string::npos)
        s.erase(tab);
    // East Asian style: "复制为路径(&A)" (or with full-width parentheses).
    for (std::string_view open : { std::string_view("("), std::string_view("\xEF\xBC\x88") }) {
        for (size_t at = s.find(open); at != std::string::npos; at = s.find(open, at + 1)) {
            size_t amp = at + open.size();
            if (amp + 1 < s.size() && s[amp] == '&' && s[amp + 1] != '&') {
                size_t key = amp + 1;
                size_t close = key + 1;
                // one ASCII key, then ")" or "）"
                if (s.compare(close, 1, ")") == 0) {
                    s.erase(at, close + 1 - at);
                    break;
                }
                if (s.compare(close, 3, "\xEF\xBC\x89") == 0) {
                    s.erase(at, close + 3 - at);
                    break;
                }
            }
        }
    }
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '&') {
            if (i + 1 < s.size() && s[i + 1] == '&') {
                out.push_back('&');
                ++i;
            }
            continue;
        }
        out.push_back(s[i]);
    }
    // "Format..." and "格式化…": the dialog marker is not part of the name.
    while (!out.empty() && (out.back() == ' '))
        out.pop_back();
    if (out.size() >= 3 && out.compare(out.size() - 3, 3, "...") == 0)
        out.erase(out.size() - 3);
    else if (out.size() >= 3 && out.compare(out.size() - 3, 3, "\xE2\x80\xA6") == 0)
        out.erase(out.size() - 3);
    while (!out.empty() && (out.back() == ' '))
        out.pop_back();
    return out;
}

} // namespace wintitles
