// Nilesoft Shell Settings (settings.exe).
//
// Development switches:
//   --page <home|appearance|behavior|rules|menus|builder|editor|backups|preferences>
//   --no-onboarding    don't show the first-run dialog this time
//   --editor-edit      open the Config Editor in its edit view
//   --editor-open <file> [--editor-line <n>]    open a file (and line) in the Config Editor
//   --snapshot <prefix>    render a short tour (as started, Home, Preferences, a placeholder,
//                          Home in light)
//                          to <prefix>-N.bmp, then exit; for screenshots without screen capture

#include "app.h"

#include "AppController.h"
#include "BackupsController.h"
#include "ContextMenusController.h"
#include "EditorController.h"
#include "MenuBuilderController.h"
#include "Registry.h"

#include <windows.h>
#include <shellapi.h>

#include <chrono>
#include <cwchar>
#include <fstream>
#include <memory>
#include <string>

namespace {

std::wstring g_snapshotPath;

// Writes a top-down 32-bit BMP (used by --snapshot for screenshots in CI-like runs).
bool writeBmp(const std::wstring &path, const slint::SharedPixelBuffer<slint::Rgba8Pixel> &px)
{
    const uint32_t w = px.width(), h = px.height();
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    ih.biSize = sizeof ih;
    ih.biWidth = LONG(w);
    ih.biHeight = -LONG(h);
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    ih.biSizeImage = w * h * 4;
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof fh + sizeof ih;
    fh.bfSize = fh.bfOffBits + ih.biSizeImage;
    std::ofstream out(path, std::ios::binary);
    if (!out)
        return false;
    out.write(reinterpret_cast<const char *>(&fh), sizeof fh);
    out.write(reinterpret_cast<const char *>(&ih), sizeof ih);
    for (auto it = px.begin(); it != px.end(); ++it) {
        const char bgra[4] = { char(it->b), char(it->g), char(it->r), char(it->a) };
        out.write(bgra, 4);
    }
    return bool(out);
}

AppOptions parseArgs()
{
    AppOptions o;
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv)
        return o;
    static constexpr const wchar_t *pages[] = { L"home",  L"appearance", L"behavior", L"rules",      L"menus",
                                                L"builder", L"editor",   L"backups",  L"preferences" };
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--no-onboarding") == 0)
            o.noOnboarding = true;
        else if (_wcsicmp(argv[i], L"--page") == 0 && i + 1 < argc) {
            ++i;
            for (int p = 0; p < int(std::size(pages)); ++p)
                if (_wcsicmp(argv[i], pages[p]) == 0)
                    o.startPage = p;
        } else if (_wcsicmp(argv[i], L"--snapshot") == 0 && i + 1 < argc) {
            g_snapshotPath = argv[++i];
        }
    }
    LocalFree(argv);
    return o;
}

// One window per user session: a second start brings the first one forward.
bool activateExistingInstance(HANDLE &mutex)
{
    mutex = CreateMutexW(nullptr, TRUE, L"Local\\Nilesoft.Shell.Settings");
    if (GetLastError() != ERROR_ALREADY_EXISTS)
        return false;
    if (HWND hwnd = FindWindowW(nullptr, L"Nilesoft Shell Settings")) {
        if (IsIconic(hwnd))
            ShowWindow(hwnd, SW_RESTORE);
        SetForegroundWindow(hwnd);
    }
    return true;
}

} // namespace

int main()
{
    HANDLE mutex = nullptr;
    if (activateExistingInstance(mutex))
        return 0;

    auto options = parseArgs();
    auto ui = AppWindow::create();
    auto controller = std::make_unique<AppController>(ui, options);
    auto editor = std::make_unique<EditorController>(ui);
    auto backups = std::make_unique<BackupsController>(ui);
    auto contextMenus = std::make_unique<ContextMenusController>(ui);
    auto menuBuilder = std::make_unique<MenuBuilderController>(ui);

    // Restore the last size and position (physical pixels) before the first frame.
    auto placement = registry::app::loadPlacement();
    if (placement) {
        ui->window().set_position(slint::PhysicalPosition({ placement->x, placement->y }));
        ui->window().set_size(slint::PhysicalSize({ uint32_t(placement->width), uint32_t(placement->height) }));
    }

    ui->window().on_close_requested([&] {
        controller->saveWindowPlacement();
        return slint::CloseRequestResponse::HideWindow;
    });

    ui->show();

    // The native window is created once the event loop runs; poll briefly for it.
    slint::Timer hwndPoll, snapshotTour;
    hwndPoll.start(slint::TimerMode::Repeated, std::chrono::milliseconds(10), [&] {
        if (HWND hwnd = ui->window().win32_hwnd()) {
            hwndPoll.stop();
            controller->attachWindow(hwnd);
            if (placement && placement->maximized)
                ui->window().set_maximized(true);
            controller->start();
            if (!g_snapshotPath.empty())
                snapshotTour.start(slint::TimerMode::Repeated, std::chrono::milliseconds(1200), [&] {
                    static int step = 0;
                    if (++step < 3)
                        return; // let the first status load land
                    int shot = step - 2;
                    if (auto px = ui->window().take_snapshot())
                        writeBmp(g_snapshotPath + L"-" + std::to_wstring(shot) + L".bmp", *px);
                    const auto &state = ui->global<AppState>();
                    state.set_onboarding_shown(false);
                    static constexpr int tour[] = { 0, 8, 1, 0 };
                    if (shot > int(std::size(tour))) {
                        snapshotTour.stop();
                        slint::quit_event_loop();
                        return;
                    }
                    state.set_page(tour[shot - 1]);
                    if (shot == int(std::size(tour)))
                        ui->invoke_apply_theme(false); // last shot: light theme (not saved)
                });
        }
    });

    slint::run_event_loop();

    menuBuilder.reset();
    contextMenus.reset();
    backups.reset();
    editor.reset();
    controller.reset();
    if (mutex)
        CloseHandle(mutex);
    return 0;
}
