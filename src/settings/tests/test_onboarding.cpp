// The first-run dialog's "Pick a look" step without a window: a card stages
// theme.name the way the Appearance page does (PendingChanges::stage with the
// saved value), Finish saves it through planSave + ConfigStore with the real
// `shell.exe -config:check`, and the card that is selected afterwards is the
// effective value read back from the config graph.

#include "doctest.h"

#include "PendingChanges.h"
#include "ThemeModel.h"

#include "nss/ConfigGraph.h"
#include "nss/ConfigStore.h"
#include "nss/Literal.h"
#include "nss/OverridesWriter.h"
#include "nss/Text.h"

#include <windows.h>

#include <atomic>
#include <filesystem>

namespace fs = std::filesystem;

namespace {

fs::path binDir() { return fs::path(SETTINGS_SOURCE_BIN).lexically_normal(); }
fs::path shellExe() { return binDir() / L"shell.exe"; }

struct TempAppData
{
    fs::path path;
    std::wstring previous;
    TempAppData()
    {
        static std::atomic<int> n{ 0 };
        path = fs::temp_directory_path() /
               (L"settings-onboarding-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++n));
        fs::remove_all(path);
        fs::create_directories(path);
        wchar_t buf[MAX_PATH * 2]{};
        if (GetEnvironmentVariableW(L"APPDATA", buf, DWORD(std::size(buf))))
            previous = buf;
        SetEnvironmentVariableW(L"APPDATA", path.c_str());
    }
    ~TempAppData()
    {
        SetEnvironmentVariableW(L"APPDATA", previous.empty() ? nullptr : previous.c_str());
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

bool runShell(const std::wstring &args)
{
    std::wstring cmd = L"\"" + shellExe().wstring() + L"\" " + args;
    STARTUPINFOW si{ sizeof si };
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(shellExe().c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                        &si, &pi))
        return false;
    WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;
}

// What SaveService::current() reports for a key with nothing pending: the effective value.
std::optional<std::string> saved(const nss::ConfigGraph &g, const char *key)
{
    if (const auto *ev = g.find(key))
        return ev->effective().value;
    return std::nullopt;
}

// The selected card: AppearanceController's index for the effective theme.name.
int selectedCard(const nss::ConfigGraph &g)
{
    auto value = saved(g, "theme.name");
    return value ? theme::themeIndex(nss::Literal::parse(*value)) : 0;
}

} // namespace

TEST_CASE("onboarding: same values and the theme cards")
{
    CHECK(sameValue("\"modern\"", "'modern'"));
    CHECK(sameValue("8", "8.0"));
    CHECK(sameValue("#fff", "#FFFFFF"));
    CHECK(sameValue("[8, 4]", "[8,4]"));
    CHECK_FALSE(sameValue("\"modern\"", "\"classic\""));
    CHECK_FALSE(sameValue("8", "\"8\""));
    CHECK(sameValue("!process.is_explorer", "!process.is_explorer"));

    CHECK(theme::themeNameLiteral(0) == "\"auto\"");
    CHECK(theme::themeNameLiteral(4) == "\"black\"");
    CHECK(theme::themeNameLiteral(5).empty());
    CHECK(theme::themeNameLiteral(-1).empty());
    for (int i = 0; i < 5; ++i)
        CHECK(theme::themeIndex(nss::Literal::parse(theme::themeNameLiteral(i))) == i);
    CHECK(theme::themeIndex(nss::Literal::parse("'Classic'")) == 2);
    CHECK(theme::themeIndex(nss::Literal::parse("\"nope\"")) == 0);
    CHECK(theme::themeIndex(nss::Literal::parse("2"), "classic") == 2); // legacy number: as resolved

    // Staging the saved value is a no-op; staging it back drops the pending change.
    PendingChanges p;
    const std::optional<std::string> file = "\"auto\"";
    CHECK(p.stage("theme.name", theme::themeNameLiteral(0), "Onboarding", file).empty());
    CHECK(p.empty());
    CHECK(p.stage("theme.name", theme::themeNameLiteral(1), "Onboarding", file).empty());
    REQUIRE(p.find("theme.name"));
    CHECK(*p.find("theme.name")->literal == "\"modern\"");
    CHECK(p.stage("theme.name", theme::themeNameLiteral(0), "Onboarding", file).empty());
    CHECK(p.empty());
    // Nothing saved (built-in default): every choice is staged.
    CHECK(p.stage("theme.name", theme::themeNameLiteral(0), "Onboarding", std::nullopt).empty());
    CHECK(p.count() == 1);
    // Invalid values are refused like set() refuses them.
    PendingChanges q;
    CHECK_FALSE(q.stage("theme.name", "", "Onboarding", file).empty());
    CHECK(q.empty());
}

TEST_CASE("onboarding: a theme picked in step 3 is saved on Finish (real shell.exe)")
{
    if (!fs::is_regular_file(shellExe()) || !fs::is_regular_file(binDir() / L"shell.dll")) {
        MESSAGE("shell.exe/shell.dll not in src/bin; skipped");
        return;
    }
    // Step 1: shell.exe -config:init into a throw-away %APPDATA%.
    TempAppData appData;
    REQUIRE(runShell(L"-silent -config:init"));
    const auto root = appData.path / L"Nilesoft" / L"Shell" / L"shell.nss";
    REQUIRE(fs::is_regular_file(root));
    const auto overrides =
        (root.parent_path() / fs::path(nss::widen(nss::OverridesWriter::relativePath))).lexically_normal();
    nss::GraphOptions graphOptions;
    graphOptions.appDir = binDir();

    // The card shown as selected is the effective value of the seeded config.
    auto before = nss::ConfigGraph::load(root, graphOptions);
    const int initial = selectedCard(before);
    const int pick = initial == 1 ? 2 : 1; // modern, or classic when modern is in use

    // Step 3: the user clicks a card. Clicking the selected one stages nothing.
    PendingChanges pending;
    CHECK(pending.stage("theme.name", theme::themeNameLiteral(initial), "Onboarding", saved(before, "theme.name")).empty());
    CHECK(pending.empty());
    REQUIRE(pending.stage("theme.name", theme::themeNameLiteral(pick), "Onboarding", saved(before, "theme.name")).empty());
    REQUIRE(pending.count() == 1);

    // Finish: the save every page uses, validated by shell.exe -config:check.
    auto plan = planSave(root, pending);
    REQUIRE_MESSAGE(plan.ok, plan.error);
    nss::SaveOptions options;
    options.reason = pending.describe();
    options.validator = nss::makeShellCheckValidator(shellExe());
    options.verify = options.validator;
    auto result = nss::ConfigStore(root).save(plan.writes, options);
    REQUIRE_MESSAGE(result.ok, (result.error + " " + result.check.message));
    CHECK(!result.snapshotId.empty());
    CHECK(options.reason.find("Onboarding") != std::string::npos);

    // The file holds theme.name, and the graph reads it back from there.
    std::string bytes;
    REQUIRE(nss::readFile(overrides, bytes));
    const auto text = nss::decode(bytes).text;
    CHECK(text.find(theme::themeNameLiteral(pick)) != std::string::npos);
    auto after = nss::ConfigGraph::load(root, graphOptions);
    const auto *ev = after.find("theme.name");
    REQUIRE(ev);
    CHECK(nss::pathKey(ev->effective().file) == nss::pathKey(overrides));
    CHECK(selectedCard(after) == pick);

    // Opening the dialog again: the saved theme is preselected, and picking it again is a no-op.
    PendingChanges again;
    CHECK(again.stage("theme.name", theme::themeNameLiteral(pick), "Onboarding", saved(after, "theme.name")).empty());
    CHECK(again.empty());
}
