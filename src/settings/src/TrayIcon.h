#pragma once

// The system tray icon and the app's hidden instance window, in plain Win32.
//
// The window is never shown. It is a normal top-level window, not a message-only one
// (HWND_MESSAGE), because message-only windows don't get broadcasts, and the icon has
// to come back after Explorer restarts (the "TaskbarCreated" broadcast). It lives on the
// UI thread, so Slint's (winit's) message loop dispatches its messages; handlers that
// touch the UI should go through slint::invoke_from_event_loop (TrayController does).
//
// The same window lets a second start of the app find the first one (signalRunning),
// lets the MSI ask the app to quit before it replaces its files (kMsgQuit), and lets a
// newer version that starts ask an older one to quit (its title has the version).

#include <windows.h>

#include <functional>
#include <string>

class TrayIcon
{
public:
    enum class Command { Open, CheckUpdates, RestartExplorer, Quit };

    struct Handlers
    {
        std::function<void()> open;            // left click, Enter, a second start of the app
        std::function<void()> balloonClicked;  // the notification
        std::function<void(Command)> command;  // the menu
        std::function<std::wstring(Command)> label; // the menu's text, asked each time it opens
        std::function<void()> quit;            // Windows signs out / shuts down, or the installer asks
    };

    static constexpr const wchar_t *kWindowClass = L"Nilesoft.ShellPro.Instance";
    static constexpr UINT kMsgShow = WM_APP + 2; // wParam 1: show the window, 0: just be there
    static constexpr UINT kMsgQuit = WM_APP + 3;
    // kWindowClass, with AppChannel's instanceSuffix() (tests).
    static std::wstring windowClass();

    explicit TrayIcon(Handlers handlers);
    ~TrayIcon();
    TrayIcon(const TrayIcon &) = delete;
    TrayIcon &operator=(const TrayIcon &) = delete;

    // The hidden window, with this title (AppChannel instanceTitle(): it says which version
    // runs). False if it couldn't be created.
    bool create(const std::wstring &title);
    HWND hwnd() const { return hwnd_; }

    // The icon in the notification area, or not.
    void setVisible(bool visible);
    bool visible() const { return visible_; }

    // A notification (a balloon, which Windows 10 and 11 show as a toast) with the app's icon.
    bool notify(const std::wstring &title, const std::wstring &text);

    // A second start of the app: asks the running one to show its window (show) or only
    // checks that it is there. False when no running copy has the window.
    static bool signalRunning(bool show);

    // The running copy's hidden window (nullptr: none), and its title.
    static HWND findRunning();
    static std::wstring titleOf(HWND other);
    // Asks the running copy to quit (kMsgQuit, as the installer does) and waits for its
    // process to end. False if it didn't end in time.
    static bool quitRunning(HWND other, DWORD timeoutMs);

private:
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT handle(UINT msg, WPARAM wParam, LPARAM lParam);
    bool add();
    void remove();
    void loadIcons();
    void showMenu(POINT at);

    Handlers handlers_;
    HWND hwnd_ = nullptr;
    HICON small_ = nullptr;
    HICON large_ = nullptr;
    UINT taskbarCreated_ = 0;
    bool visible_ = false;
    bool added_ = false;
};
