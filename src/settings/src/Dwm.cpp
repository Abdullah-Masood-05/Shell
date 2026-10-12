#include "Dwm.h"

#include <dwmapi.h>

namespace dwm {

void applyTitleBar(HWND hwnd, bool dark)
{
    if (!hwnd)
        return;
    BOOL useDark = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDark, sizeof useDark);
    int backdrop = DWMSBT_MAINWINDOW; // Mica
    DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof backdrop);
    // Repaint the non-client area so a theme switch shows at once.
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

void applyIcon(HWND hwnd)
{
    if (!hwnd)
        return;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    auto load = [&](int metricX, int metricY) {
        return static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                             GetSystemMetrics(metricX), GetSystemMetrics(metricY),
                                             LR_DEFAULTCOLOR | LR_SHARED));
    };
    if (HICON bigIcon = load(SM_CXICON, SM_CYICON))
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, LPARAM(bigIcon));
    if (HICON smallIcon = load(SM_CXSMICON, SM_CYSMICON))
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, LPARAM(smallIcon));
}

} // namespace dwm
