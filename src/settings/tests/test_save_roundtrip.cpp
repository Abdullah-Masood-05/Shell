// The save path of the Appearance, Behavior and Rules pages end to end:
// PendingChanges -> planSave (imports/settings-app.nss, imported last) ->
// ConfigStore::save with the real `shell.exe -config:check` -> ConfigGraph reads
// the new values back. The config is seeded with `shell.exe -silent -config:init`
// under a temporary %APPDATA%; skipped when src/bin has no shell.exe.

#include "doctest.h"

#include "PendingChanges.h"

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
               (L"settings-roundtrip-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++n));
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

std::string readText(const fs::path &p)
{
    std::string bytes;
    nss::readFile(p, bytes);
    return nss::decode(bytes).text;
}

// The effective value of `key` and whether it comes from settings-app.nss.
std::pair<std::string, bool> effective(const nss::ConfigGraph &g, const fs::path &overrides, const char *key)
{
    const auto *ev = g.find(key);
    if (!ev)
        return { "", false };
    return { ev->effective().value, nss::pathKey(ev->effective().file) == nss::pathKey(overrides) };
}

nss::SaveResult commit(const fs::path &root, const PendingChanges &pending, SavePlan *planOut = nullptr)
{
    auto plan = planSave(root, pending);
    REQUIRE_MESSAGE(plan.ok, plan.error);
    if (planOut)
        *planOut = plan;
    nss::SaveOptions options;
    options.reason = pending.describe();
    options.validator = nss::makeShellCheckValidator(shellExe());
    options.verify = options.validator;
    nss::ConfigStore store(root);
    return store.save(plan.writes, options);
}

} // namespace

TEST_CASE("save round trip: pending changes through shell.exe -config:check (real shell.exe)")
{
    if (!fs::is_regular_file(shellExe()) || !fs::is_regular_file(binDir() / L"shell.dll")) {
        MESSAGE("shell.exe/shell.dll not in src/bin; skipped");
        return;
    }
    TempAppData appData;
    REQUIRE(runShell(L"-silent -config:init"));
    const auto root = appData.path / L"Nilesoft" / L"Shell" / L"shell.nss";
    REQUIRE(fs::is_regular_file(root));
    const auto overrides = (root.parent_path() / L"imports" / L"settings-app.nss").lexically_normal();
    nss::GraphOptions graphOptions;
    graphOptions.appDir = binDir();

    // The seeded config: the exclude condition is the shipped expression.
    {
        auto g = nss::ConfigGraph::load(root, graphOptions);
        auto [where, fromApp] = effective(g, overrides, "settings.exclude.where");
        CHECK(where == "!process.is_explorer");
        CHECK_FALSE(fromApp);
    }

    // 1. Appearance, Behavior and Rules changes in one save.
    const std::string shots = "C:\\Users\\Test User\\Pictures\\Menu shots";
    PendingChanges pending;
    REQUIRE(pending.set("theme.name", "\"modern\"", "Appearance").empty());
    REQUIRE(pending.set("theme.dark", "true", "Appearance").empty());
    REQUIRE(pending.set("theme.background.color", "#20232A", "Appearance").empty());
    REQUIRE(pending.set("theme.background.opacity", "85", "Appearance").empty());
    REQUIRE(pending.set("theme.background.effect", "3", "Appearance").empty());
    REQUIRE(pending.set("theme.item.radius", "3", "Appearance").empty());
    REQUIRE(pending.set("theme.font.name", "\"Segoe UI Variable Text\"", "Appearance").empty());
    REQUIRE(pending.set("theme.item.padding", "[8, 4]", "Appearance").empty());
    REQUIRE(pending.set("settings.showdelay", "150", "Behavior").empty());
    REQUIRE(pending.set("settings.tip.enabled", "true", "Behavior").empty());
    REQUIRE(pending.set("settings.tip.time", "1.5", "Behavior").empty());
    REQUIRE(pending.set("settings.tip.default", "[#202020, #FFFFFF]", "Behavior").empty());
    REQUIRE(pending.set("settings.screenshot.enabled", "true", "Behavior").empty());
    REQUIRE(pending.set("settings.screenshot.directory", nss::quoteString(shots), "Behavior").empty());
    REQUIRE(pending.set("settings.modify.image", "2", "Behavior").empty());
    REQUIRE(pending.set("settings.exclude.where", "!process.is_explorer || process.name == \"notepad\"", "Rules").empty());
    REQUIRE(pending.set("settings.exclude.process", "[\"notepad\", \"code\"]", "Rules").empty());
    REQUIRE(pending.set("settings.exclude.window", "\"ConsoleWindowClass\"", "Rules").empty());

    SavePlan plan;
    auto saved = commit(root, pending, &plan);
    REQUIRE_MESSAGE(saved.ok, (saved.error + " " + saved.check.message));
    CHECK_FALSE(saved.rolledBack);
    CHECK(plan.rootChanged);
    CHECK(fs::is_regular_file(overrides));
    CHECK(!saved.snapshotId.empty());

    // The root imports settings-app.nss as its last statement.
    auto rootDoc = nss::NssDocument::load(root);
    REQUIRE(rootDoc);
    CHECK_FALSE(nss::OverridesWriter::ensureImportedLast(*rootDoc));

    // The whole tree passes the real check, and the graph reads every value back from
    // settings-app.nss.
    CHECK(nss::makeShellCheckValidator(shellExe())(root).ok);
    {
        auto g = nss::ConfigGraph::load(root, graphOptions);
        for (const auto &[key, change] : pending.items()) {
            auto [value, fromApp] = effective(g, overrides, key.c_str());
            CHECK_MESSAGE(fromApp, key);
            CHECK_MESSAGE(nss::Literal::parse(value).text == *change.literal, key);
        }
        auto dir = nss::Literal::parse(effective(g, overrides, "settings.screenshot.directory").first);
        CHECK(dir.kind == nss::LiteralKind::String);
        CHECK(dir.string == shots);
    }

    // 2. A condition shell.exe rejects: nothing is written.
    const auto before = readText(overrides);
    PendingChanges bad;
    REQUIRE(bad.set("settings.exclude.where", "process.name == ", "Rules").empty()); // Any: not checked here
    auto rejected = commit(root, bad);
    CHECK_FALSE(rejected.ok);
    CHECK_FALSE(rejected.check.ok);
    CHECK(readText(overrides) == before);

    // 3. Resets drop keys from settings-app.nss; the file's own values apply again.
    PendingChanges reset;
    reset.reset("settings.exclude.where", "Rules");
    reset.reset("theme.item.radius", "Appearance");
    auto afterReset = commit(root, reset);
    REQUIRE_MESSAGE(afterReset.ok, (afterReset.error + " " + afterReset.check.message));
    {
        auto g = nss::ConfigGraph::load(root, graphOptions);
        auto [where, fromApp] = effective(g, overrides, "settings.exclude.where");
        CHECK(where == "!process.is_explorer");
        CHECK_FALSE(fromApp);
        CHECK(effective(g, overrides, "theme.background.color").second);
    }
}
