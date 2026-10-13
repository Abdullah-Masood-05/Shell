// Nilesoft Shell Settings (settings.exe).
//
// Development switches:
//   --page <home|appearance|behavior|rules|menus|builder|editor|backups|preferences>
//   --no-onboarding    don't show the first-run dialog this time
//   --onboarding <1|2|3>   show the first-run dialog at that step, whatever the status;
//                          with --snapshot, the tour starts with that step and the ones after it
//   --editor-edit      open the Config Editor in its edit view
//   --editor-open <file> [--editor-line <n>]    open a file (and line) in the Config Editor
//   --snapshot <prefix>    render a short tour (as started, Home, Preferences, Appearance,
//                          Home in light; tools/screenshots.ps1 relies on this order)
//                          to <prefix>-N.bmp, then exit; for screenshots without screen capture.
//                          The window is 1120x760 and placed off-screen, and a running
//                          Settings window is left alone (no single-instance check)
//   --snapshot-pages <page,page,...>    the pages of the tour instead (the first shot is
//                          still the start page, the last the start page in light)
//   --snapshot-dark        start the tour in the dark theme whatever Windows uses
//   --status-json <file>   Home shows this `shell.exe -status` output instead of running it
//   --set <key>=<literal>, --save    see AppearanceController.h

#include "app.h"

#include "AppController.h"
#include "AppearanceController.h"
#include "BackupsController.h"
#include "BehaviorController.h"
#include "ContextMenusController.h"
#include "EditorController.h"
#include "MenuBuilderController.h"
#include "OnboardingController.h"
#include "Registry.h"
#include "SaveService.h"

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cwchar>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

std::wstring g_snapshotPath;
std::vector<int> g_snapshotPages;
bool g_snapshotDark = false;

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
        } else if (_wcsicmp(argv[i], L"--status-json") == 0 && i + 1 < argc) {
            o.statusFile = argv[++i];
        } else if (_wcsicmp(argv[i], L"--onboarding") == 0 && i + 1 < argc) {
            o.onboardingStep = std::clamp(_wtoi(argv[++i]), 1, 3) - 1;
        } else if (_wcsicmp(argv[i], L"--snapshot") == 0 && i + 1 < argc) {
            g_snapshotPath = argv[++i];
        } else if (_wcsicmp(argv[i], L"--snapshot-dark") == 0) {
            g_snapshotDark = true;
        } else if (_wcsicmp(argv[i], L"--snapshot-pages") == 0 && i + 1 < argc) {
            std::wstring list = argv[++i];
            size_t start = 0;
            while (start <= list.size()) {
                size_t comma = list.find(L',', start);
                auto name = list.substr(start, comma == std::wstring::npos ? std::wstring::npos : comma - start);
                for (int p = 0; p < int(std::size(pages)); ++p)
                    if (_wcsicmp(name.c_str(), pages[p]) == 0)
                        g_snapshotPages.push_back(p);
                if (comma == std::wstring::npos)
                    break;
                start = comma + 1;
            }
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
    auto options = parseArgs();
    const bool snapshot = !g_snapshotPath.empty();

    // Snapshots never touch a running Settings window (no single-instance check).
    HANDLE mutex = nullptr;
    if (!snapshot && activateExistingInstance(mutex))
        return 0;

    // The UI thread is an STA (the folder picker and SHOpenFolderAndSelectItems need COM).
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    auto ui = AppWindow::create();
    auto controller = std::make_unique<AppController>(ui, options);
    auto editor = std::make_unique<EditorController>(ui);
    auto backups = std::make_unique<BackupsController>(ui);
    auto saveService = std::make_unique<SaveService>(ui, ShellCli::locate());
    auto appearance = std::make_unique<AppearanceController>(ui, *saveService);
    auto behavior = std::make_unique<BehaviorController>(ui, *saveService);
    auto contextMenus = std::make_unique<ContextMenusController>(ui);
    auto menuBuilder = std::make_unique<MenuBuilderController>(ui);
    auto onboarding = std::make_unique<OnboardingController>(ui, *saveService);
    // A personal config created from Home or the first-run dialog is the new root: read it.
    controller->onConfigChanged([&] { saveService->reloadConfig(); });

    // Restore the last size and position (physical pixels) before the first frame.
    // Snapshots: the default size, off-screen.
    auto placement = snapshot ? std::nullopt : registry::app::loadPlacement();
    if (placement) {
        ui->window().set_position(slint::PhysicalPosition({ placement->x, placement->y }));
        ui->window().set_size(slint::PhysicalSize({ uint32_t(placement->width), uint32_t(placement->height) }));
    } else if (snapshot) {
        ui->window().set_position(slint::PhysicalPosition(
            { GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN) + 64, 0 }));
    }

    ui->window().on_close_requested([&] {
        controller->saveWindowPlacement();
        return slint::CloseRequestResponse::HideWindow;
    });

    ui->show();

    // The snapshot tour: the first shot is the window as started; each later one is a
    // frame from this list. With --onboarding, the dialog's later steps come first.
    struct Frame
    {
        int page;
        int onboarding; // the dialog's step, or -1 for no dialog
        bool light;
    };
    std::vector<Frame> frames;
    if (snapshot) {
        if (options.onboardingStep >= 0)
            for (int s = options.onboardingStep + 1; s <= 2; ++s)
                frames.push_back({ options.startPage, s, false });
        for (int page : g_snapshotPages.empty() ? std::vector<int>{ 0, 8, 1 } : g_snapshotPages)
            frames.push_back({ page, -1, false });
        // Last, in light (not saved): Home, or the start page for --snapshot-pages.
        frames.push_back({ g_snapshotPages.empty() ? 0 : options.startPage, -1, true });
    }

    // The native window is created once the event loop runs; poll briefly for it.
    slint::Timer hwndPoll, snapshotTour;
    hwndPoll.start(slint::TimerMode::Repeated, std::chrono::milliseconds(10), [&] {
        if (HWND hwnd = ui->window().win32_hwnd()) {
            hwndPoll.stop();
            controller->attachWindow(hwnd);
            saveService->attachWindow(hwnd);
            if (placement && placement->maximized)
                ui->window().set_maximized(true);
            controller->start();
            appearance->start();
            if (snapshot)
                snapshotTour.start(slint::TimerMode::Repeated, std::chrono::milliseconds(1200), [&] {
                    static int step = 0;
                    if (++step < 3) {
                        if (g_snapshotDark && step == 1) {
                            ui->invoke_apply_theme(true);
                            ui->global<AppearanceState>().invoke_set_preview_dark(true);
                        }
                        return; // let the first status load land
                    }
                    const int shot = step - 2;
                    if (auto px = ui->window().take_snapshot())
                        writeBmp(g_snapshotPath + L"-" + std::to_wstring(shot) + L".bmp", *px);
                    if (shot > int(frames.size())) {
                        snapshotTour.stop();
                        slint::quit_event_loop();
                        return;
                    }
                    const auto &frame = frames[size_t(shot - 1)];
                    const auto &state = ui->global<AppState>();
                    state.set_onboarding_shown(frame.onboarding >= 0);
                    if (frame.onboarding >= 0)
                        state.set_onboarding_step(frame.onboarding);
                    state.set_page(frame.page);
                    if (frame.light) {
                        ui->invoke_apply_theme(false);
                        ui->global<AppearanceState>().invoke_set_preview_dark(false);
                    }
                });
        }
    });

    slint::run_event_loop();

    onboarding.reset();
    behavior.reset();
    appearance.reset();
    saveService.reset();
    menuBuilder.reset();
    contextMenus.reset();
    backups.reset();
    editor.reset();
    controller.reset();
    if (SUCCEEDED(com))
        CoUninitialize();
    if (mutex)
        CloseHandle(mutex);
    return 0;
}

