// Nilesoft Shell Pro, the app (NilesoftShellPro.exe).
//
// Development switches:
//   --page <home|appearance|behavior|rules|menus|builder|editor|backups|preferences|openwith|menumap|snippets>
//   --no-onboarding    don't show the first-run dialog this time
//   --onboarding <1|2|3>   show the first-run dialog at that step, whatever the status;
//                          with --snapshot, the tour starts with that step and the ones after it
//   --editor-edit      open the Config Editor in its edit view
//   --editor-open <file> [--editor-line <n>]    open a file (and line) in the Config Editor
//   --editor-select <first>-<last>, --editor-find <text>    select those lines, or open the
//                          find box with that text, in the Config Editor's edit view (screenshots)
//   --openwith-select <n>  open row n of the Open with list, with its Test result
//   --snippets-code <id>   the Snippets page shows that snippet's code
//   --snippets-select <id> [--snippets-context <file|folder|back|desktop|drive|taskbar>]
//                          the Snippets page previews that snippet (in that context)
//   --taskbar-select <n>, --taskbar-picker   Context Menus opens entry n of the Taskbar menu
//                          list, or its installed apps list
//   --snapshot <prefix>    render a short tour (as started, Home, Preferences, Appearance,
//                          Home in light; tools/screenshots.ps1 relies on this order)
//                          to <prefix>-N.bmp, then exit; for screenshots without screen capture.
//                          The window is 1120x760 and placed off-screen, and a running
//                          window of the app is left alone (no single-instance check)
//   --snapshot-pages <page,page,...>    the pages of the tour instead (the first shot is
//                          still the start page, the last the start page in light)
//   --snapshot-dark        start the tour in the dark theme whatever Windows uses
//   --snapshot-height <px>    a taller window for the tour, to show more of a long page
//   --snapshot-width <px>     a narrower or wider window for the tour
//   --snapshot-scroll-end     every page of the tour scrolled to its end
//   --map-context <file|folder|back|desktop|drive|taskbar>, --map-view <outline|nodes>,
//   --map-expand, --map-select <title>   how the Menu map page starts (screenshots)
//   --status-json <file>   Home shows this `shell.exe -status` output instead of running it
//   --lang <auto|en|zh-CN|zh-TW>   the app's language for this run (not saved)
//   --po <file>            the translator preview: shows the app in that .po file, like
//                          Preferences > Language > Test a translation file
//   --set <key>=<literal>, --save, --appearance-tab <n>    see AppearanceController.h
//   --check-updates-now    check for updates right away (with NSP_UPDATE_FEED=<https URL or
//                          local JSON file> instead of the GitHub API, for tests)
//
// Update helper (UpdateController.h; started from a copy in %TEMP%, never by hand):
//   --apply-update <staging> <installdir> <pid>, --apply-msi <msi> <installdir> <pid>
// and what it passes back when it starts the app again:
//   --updated <msi|portable>, --update-failed <message>, --update-cancelled,
//   and --ui-lang <en|zh-CN|...>: the language the app was showing

#include "app.h"

#include "AppController.h"
#include "AppearanceController.h"
#include "BackupsController.h"
#include "BehaviorController.h"
#include "ConfigUpdateController.h"
#include "ContextMenusController.h"
#include "EditorController.h"
#include "MenuBuilderController.h"
#include "MenuMapController.h"
#include "OnboardingController.h"
#include "OpenWithController.h"
#include "Registry.h"
#include "SaveService.h"
#include "SnippetsController.h"
#include "Text.h"
#include "UpdateController.h"

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shobjidl.h>

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
int g_snapshotHeight = 0;
int g_snapshotWidth = 0;
bool g_snapshotScrollEnd = false;
UpdateOptions g_update;
std::string g_snippetsCode;
std::string g_snippetsSelect;
std::string g_snippetsContext;
MenuMapController::DevOptions g_map;
bool g_mapSet = false;

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
                                                L"builder", L"editor",   L"backups",  L"preferences",
                                                L"openwith", L"menumap", L"snippets" };
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
        } else if (_wcsicmp(argv[i], L"--lang") == 0 && i + 1 < argc) {
            o.language = text::narrow(argv[++i]);
        } else if (_wcsicmp(argv[i], L"--po") == 0 && i + 1 < argc) {
            o.testPo = argv[++i];
        } else if (_wcsicmp(argv[i], L"--check-updates-now") == 0) {
            g_update.checkNow = true;
        } else if (_wcsicmp(argv[i], L"--ui-lang") == 0 && i + 1 < argc) {
            o.shownLanguage = text::narrow(argv[++i]);
        } else if (_wcsicmp(argv[i], L"--updated") == 0 && i + 1 < argc) {
            g_update.updated = argv[++i];
        } else if (_wcsicmp(argv[i], L"--update-failed") == 0 && i + 1 < argc) {
            g_update.updateFailed = argv[++i];
        } else if (_wcsicmp(argv[i], L"--update-cancelled") == 0) {
            g_update.updateCancelled = true;
        } else if (_wcsicmp(argv[i], L"--snippets-code") == 0 && i + 1 < argc) {
            g_snippetsCode = text::narrow(argv[++i]);
        } else if (_wcsicmp(argv[i], L"--snippets-select") == 0 && i + 1 < argc) {
            g_snippetsSelect = text::narrow(argv[++i]);
        } else if (_wcsicmp(argv[i], L"--snippets-context") == 0 && i + 1 < argc) {
            g_snippetsContext = text::narrow(argv[++i]);
        } else if (_wcsicmp(argv[i], L"--map-context") == 0 && i + 1 < argc) {
            static constexpr const wchar_t *contexts[] = { L"file", L"folder", L"back", L"desktop", L"drive", L"taskbar" };
            ++i;
            for (int c = 0; c < int(std::size(contexts)); ++c)
                if (_wcsicmp(argv[i], contexts[c]) == 0)
                    g_map.context = c;
            g_mapSet = true;
        } else if (_wcsicmp(argv[i], L"--map-view") == 0 && i + 1 < argc) {
            g_map.view = _wcsicmp(argv[++i], L"nodes") == 0 ? 1 : 0;
            g_mapSet = true;
        } else if (_wcsicmp(argv[i], L"--map-expand") == 0) {
            g_map.expandAll = true;
            g_mapSet = true;
        } else if (_wcsicmp(argv[i], L"--map-select") == 0 && i + 1 < argc) {
            g_map.select = text::narrow(argv[++i]);
            g_mapSet = true;
        } else if (_wcsicmp(argv[i], L"--snapshot-dark") == 0) {
            g_snapshotDark = true;
        } else if (_wcsicmp(argv[i], L"--snapshot-height") == 0 && i + 1 < argc) {
            g_snapshotHeight = std::clamp(_wtoi(argv[++i]), 0, 4000);
        } else if (_wcsicmp(argv[i], L"--snapshot-width") == 0 && i + 1 < argc) {
            g_snapshotWidth = std::clamp(_wtoi(argv[++i]), 0, 4000);
        } else if (_wcsicmp(argv[i], L"--snapshot-scroll-end") == 0) {
            g_snapshotScrollEnd = true;
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

// The taskbar groups the window with the Start menu shortcut, which the installer gives
// the same ID (setup.wxs, System.AppUserModel.ID).
constexpr const wchar_t *appUserModelId = L"Nilesoft.ShellPro";

// One window per user session: a second start brings the first one forward. The mutex
// name predates the rename and stays, so an older build that is still open counts too.
bool activateExistingInstance(HANDLE &mutex)
{
    mutex = CreateMutexW(nullptr, TRUE, L"Local\\Nilesoft.Shell.Settings");
    if (GetLastError() != ERROR_ALREADY_EXISTS)
        return false;
    HWND hwnd = FindWindowW(nullptr, L"Nilesoft Shell Pro");
    if (!hwnd)
        hwnd = FindWindowW(nullptr, L"Nilesoft Shell Settings"); // the title before the rename
    if (hwnd) {
        if (IsIconic(hwnd))
            ShowWindow(hwnd, SW_RESTORE);
        SetForegroundWindow(hwnd);
    }
    return true;
}

} // namespace

int main()
{
    // The update helper: no window, no single-instance check.
    if (int rc = runUpdateHelper(); rc >= 0)
        return rc;

    auto options = parseArgs();
    const bool snapshot = !g_snapshotPath.empty();
    // Snapshots write nothing and check only when asked to.
    if (snapshot) {
        g_update.persist = false;
        g_update.allowAutoCheck = g_update.checkNow;
    }

    // Snapshots never touch a running window of the app (no single-instance check).
    HANDLE mutex = nullptr;
    if (!snapshot && activateExistingInstance(mutex))
        return 0;

    // Before the first window exists, so the window picks it up.
    SetCurrentProcessExplicitAppUserModelID(appUserModelId);

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
    auto menuMap = std::make_unique<MenuMapController>(ui, *saveService);
    if (g_mapSet)
        menuMap->setDevOptions(g_map);
    auto openWith = std::make_unique<OpenWithController>(ui);
    // The --snapshot tour writes nothing: no refresh of the snippet copies.
    auto snippetsPage = std::make_unique<SnippetsController>(ui, !snapshot);
    if (!g_snippetsCode.empty())
        snippetsPage->showCode(g_snippetsCode);
    if (!g_snippetsSelect.empty())
        snippetsPage->selectForPreview(g_snippetsSelect, g_snippetsContext);
    // The Snippets preview draws with the Appearance page's theme and Light/Dark.
    appearance->onPreview([&](const theme::Resolved &r) { snippetsPage->setTheme(r); });
    auto onboarding = std::make_unique<OnboardingController>(ui, *saveService);
    // After an update (a new version, or the helper's --updated): the personal config's
    // menu files the user never changed are brought up to this version.
    auto configUpdate = std::make_unique<ConfigUpdateController>(ui, !snapshot && options.statusFile.empty(),
                                                                 !g_update.updated.empty());
    configUpdate->onFilesChanged([&] { saveService->reloadConfig(); });
    // Installing an update closes the app first: the helper waits for this process to end.
    auto updates = std::make_unique<UpdateController>(ui, g_update, [&] {
        controller->saveWindowPlacement();
        ui->hide();
        slint::quit_event_loop();
    });
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
        if (g_snapshotHeight > 0 || g_snapshotWidth > 0)
            ui->window().set_size(slint::LogicalSize({ g_snapshotWidth > 0 ? float(g_snapshotWidth) : 1120.f,
                                                       g_snapshotHeight > 0 ? float(g_snapshotHeight) : 760.f }));
        ui->global<AppState>().set_scroll_to_end(g_snapshotScrollEnd);
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
            updates->attachWindow(hwnd);
            if (placement && placement->maximized)
                ui->window().set_maximized(true);
            controller->start();
            appearance->start();
            updates->start();
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

    updates.reset();
    configUpdate.reset();
    onboarding.reset();
    behavior.reset();
    appearance.reset();
    menuMap.reset();
    saveService.reset();
    snippetsPage.reset();
    openWith.reset();
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

