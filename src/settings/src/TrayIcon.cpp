#include "TrayIcon.h"

#include <commctrl.h>
#include <shellapi.h>

#include <cwchar>

namespace {

constexpr UINT kMsgTray = WM_APP + 1; // the icon's callback message
constexpr UINT kIconId = 1;
constexpr int kAppIcon = 1; // IDI_APP in res/settings.rc.in

// Popup menus follow the Windows app mode (light or dark) only when the app opts in,
// through uxtheme's unnamed exports (SetPreferredAppMode, ordinal 135, Windows 10 1903
// and later; AllowDarkModeForWindow, 133). Without them the menu is always light.
void allowDarkMenus(HWND hwnd)
{
    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux)
        return;
    using SetPreferredAppMode = int(WINAPI *)(int);
    using AllowDarkModeForWindow = bool(WINAPI *)(HWND, bool);
    using FlushMenuThemes = void(WINAPI *)();
    OSVERSIONINFOW v{ sizeof v };
    // GetVersionEx lies without a manifest entry; RtlGetVersion doesn't.
    if (auto rtl = reinterpret_cast<LONG(WINAPI *)(OSVERSIONINFOW *)>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")))
        rtl(&v);
    if (v.dwBuildNumber < 18362)
        return;
    if (auto set = reinterpret_cast<SetPreferredAppMode>(GetProcAddress(ux, MAKEINTRESOURCEA(135))))
        set(1); // AllowDark: follow the Windows app mode
    if (auto allow = reinterpret_cast<AllowDarkModeForWindow>(GetProcAddress(ux, MAKEINTRESOURCEA(133))))
        allow(hwnd, true);
    if (auto flush = reinterpret_cast<FlushMenuThemes>(GetProcAddress(ux, MAKEINTRESOURCEA(136))))
        flush();
}

} // namespace

TrayIcon::TrayIcon(Handlers handlers) : handlers_(std::move(handlers)) {}

TrayIcon::~TrayIcon()
{
    remove();
    if (hwnd_)
        DestroyWindow(hwnd_);
    if (small_)
        DestroyIcon(small_);
    if (large_)
        DestroyIcon(large_);
}

bool TrayIcon::create()
{
    if (hwnd_)
        return true;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{ sizeof wc };
    wc.lpfnWndProc = &TrayIcon::wndProc;
    wc.hInstance = inst;
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);
    // Not "Nilesoft Shell Pro": older versions look for the main window by that title.
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, L"Nilesoft Shell Pro background", WS_POPUP, 0, 0, 0, 0,
                            nullptr, nullptr, inst, this);
    if (!hwnd_)
        return false;
    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
    // Explorer and a second start of the app may run at a lower integrity level.
    for (UINT msg : { taskbarCreated_, kMsgShow, kMsgQuit })
        ChangeWindowMessageFilterEx(hwnd_, msg, MSGFLT_ALLOW, nullptr);
    allowDarkMenus(hwnd_);
    return true;
}

void TrayIcon::loadIcons()
{
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (small_)
        DestroyIcon(small_);
    if (large_)
        DestroyIcon(large_);
    small_ = large_ = nullptr;
    // The sizes Windows wants at the current DPI (16 px at 100%, 32 px for the toast).
    LoadIconMetric(inst, MAKEINTRESOURCEW(kAppIcon), LIM_SMALL, &small_);
    LoadIconMetric(inst, MAKEINTRESOURCEW(kAppIcon), LIM_LARGE, &large_);
}

bool TrayIcon::add()
{
    if (!hwnd_)
        return false;
    loadIcons();
    NOTIFYICONDATAW nid{ sizeof nid };
    nid.hWnd = hwnd_;
    nid.uID = kIconId;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = kMsgTray;
    nid.hIcon = small_;
    wcscpy_s(nid.szTip, L"Nilesoft Shell Pro");
    if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
        // Left over from a copy that didn't exit cleanly, or the taskbar isn't up yet.
        Shell_NotifyIconW(NIM_DELETE, &nid);
        if (!Shell_NotifyIconW(NIM_ADD, &nid))
            return false;
    }
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
    added_ = true;
    return true;
}

void TrayIcon::remove()
{
    if (!added_ || !hwnd_)
        return;
    NOTIFYICONDATAW nid{ sizeof nid };
    nid.hWnd = hwnd_;
    nid.uID = kIconId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    added_ = false;
}

void TrayIcon::setVisible(bool visible)
{
    visible_ = visible;
    if (visible && !added_)
        add();
    else if (!visible)
        remove();
}

bool TrayIcon::notify(const std::wstring &title, const std::wstring &text)
{
    if (!added_)
        return false;
    NOTIFYICONDATAW nid{ sizeof nid };
    nid.hWnd = hwnd_;
    nid.uID = kIconId;
    nid.uFlags = NIF_INFO | NIF_SHOWTIP;
    wcsncpy_s(nid.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(nid.szInfo, text.c_str(), _TRUNCATE);
    // The app's own icon; nothing while Windows is in quiet hours or a full-screen app runs.
    nid.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON | NIIF_RESPECT_QUIET_TIME;
    nid.hBalloonIcon = large_;
    return Shell_NotifyIconW(NIM_MODIFY, &nid) != FALSE;
}

bool TrayIcon::signalRunning(bool show)
{
    HWND other = FindWindowW(kWindowClass, nullptr);
    if (!other)
        return false;
    if (show) {
        // This process was just started by the user, so it may hand the foreground on.
        DWORD pid = 0;
        GetWindowThreadProcessId(other, &pid);
        AllowSetForegroundWindow(pid ? pid : ASFW_ANY);
    }
    PostMessageW(other, kMsgShow, show ? 1 : 0, 0);
    return true;
}

void TrayIcon::showMenu(POINT at)
{
    HMENU menu = CreatePopupMenu();
    if (!menu)
        return;
    auto item = [&](Command c, bool bold = false) {
        std::wstring text = handlers_.label ? handlers_.label(c) : std::wstring();
        AppendMenuW(menu, MF_STRING, UINT_PTR(c) + 1, text.c_str());
        if (bold)
            SetMenuDefaultItem(menu, UINT(c) + 1, FALSE);
    };
    item(Command::Open, true);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    item(Command::CheckUpdates);
    item(Command::RestartExplorer);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    item(Command::Quit);

    // The menu closes when it loses focus only if its owner is the foreground window.
    SetForegroundWindow(hwnd_);
    UINT flags = TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON;
    flags |= GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
    UINT chosen = UINT(TrackPopupMenuEx(menu, flags, at.x, at.y, hwnd_, nullptr));
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu);
    if (chosen && handlers_.command)
        handlers_.command(Command(chosen - 1));
}

LRESULT CALLBACK TrayIcon::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_NCCREATE) {
        auto *cs = reinterpret_cast<CREATESTRUCTW *>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, LONG_PTR(cs->lpCreateParams));
    }
    auto *self = reinterpret_cast<TrayIcon *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self && self->hwnd_ == hwnd)
        return self->handle(msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT TrayIcon::handle(UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == taskbarCreated_ && taskbarCreated_) {
        // Explorer restarted: its notification area forgot the icon.
        added_ = false;
        if (visible_)
            add();
        return 0;
    }
    switch (msg) {
    case kMsgTray:
        // NOTIFYICON_VERSION_4: the event in LOWORD(lParam), the anchor point in wParam.
        switch (LOWORD(lParam)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
            if (handlers_.open)
                handlers_.open();
            break;
        case WM_CONTEXTMENU:
            showMenu(POINT{ short(LOWORD(wParam)), short(HIWORD(wParam)) });
            break;
        case NIN_BALLOONUSERCLICK:
            if (handlers_.balloonClicked)
                handlers_.balloonClicked();
            break;
        }
        return 0;
    case kMsgShow:
        if (wParam && handlers_.open)
            handlers_.open();
        return 0;
    case kMsgQuit:
        if (handlers_.quit)
            handlers_.quit();
        return 0;
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        // Signing out, shutting down, or an installer closing the app (Restart Manager).
        if (wParam && handlers_.quit)
            handlers_.quit();
        return 0;
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
        // The icon sizes follow the DPI.
        if (added_) {
            loadIcons();
            NOTIFYICONDATAW nid{ sizeof nid };
            nid.hWnd = hwnd_;
            nid.uID = kIconId;
            nid.uFlags = NIF_ICON | NIF_SHOWTIP;
            nid.hIcon = small_;
            Shell_NotifyIconW(NIM_MODIFY, &nid);
        }
        break;
    case WM_SETTINGCHANGE:
        // The Windows app mode changed: the menu follows it.
        if (lParam && wcscmp(reinterpret_cast<const wchar_t *>(lParam), L"ImmersiveColorSet") == 0)
            allowDarkMenus(hwnd_);
        break;
    }
    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}
