// The app in the background (Background.h): the Run entry that starts it with Windows,
// against a fake registry, and the update-check schedule and notify-once rule.

#include "doctest.h"

#include "Background.h"

#include <map>
#include <optional>
#include <set>
#include <string>

using namespace startup;

namespace {

struct FakeStore final : Store
{
    std::map<std::wstring, DWORD> prefs;
    std::optional<std::wstring> run;
    bool disabled = false;
    std::set<std::wstring> files;
    int runWrites = 0;

    std::optional<DWORD> pref(const wchar_t *name) override
    {
        auto it = prefs.find(name);
        return it == prefs.end() ? std::nullopt : std::optional<DWORD>(it->second);
    }
    bool setPref(const wchar_t *name, DWORD value) override
    {
        prefs[name] = value;
        return true;
    }
    std::optional<std::wstring> runValue() override { return run; }
    bool setRunValue(const std::wstring &command) override
    {
        run = command;
        ++runWrites;
        return true;
    }
    bool deleteRunValue() override
    {
        run.reset();
        return true;
    }
    bool disabledInWindows() override { return disabled; }
    bool clearDisabledInWindows() override
    {
        disabled = false;
        return true;
    }
    bool fileExists(const std::wstring &path) override { return files.count(path) > 0; }
};

const std::wstring kExe = L"C:\\Program Files\\Nilesoft Shell\\NilesoftShellPro.exe";
const std::wstring kCommand = L"\"C:\\Program Files\\Nilesoft Shell\\NilesoftShellPro.exe\" --background";

} // namespace

TEST_CASE("startup: the Run command and its program")
{
    CHECK(runCommand(kExe) == kCommand);
    CHECK(programOf(kCommand) == kExe);
    CHECK(programOf(L"  C:\\Tools\\NilesoftShellPro.exe --background") == L"C:\\Tools\\NilesoftShellPro.exe");
    CHECK(programOf(L"C:\\Tools\\NilesoftShellPro.exe") == L"C:\\Tools\\NilesoftShellPro.exe");
    CHECK(programOf(L"\"C:\\unterminated") == L"C:\\unterminated");
    CHECK(programOf(L"") == L"");
    CHECK(isOurs(kCommand));
    CHECK(isOurs(L"\"D:\\Portable\\nilesoftshellpro.EXE\" --background"));
    CHECK_FALSE(isOurs(L"\"C:\\Program Files\\Other\\Other.exe\" --background"));
    CHECK_FALSE(isOurs(L"\"C:\\x\\NilesoftShellPro.exe.bak\""));
}

TEST_CASE("startup: defaults on the first start of this version")
{
    FakeStore s;
    auto p = load(s);
    CHECK(p.keepInTray);
    CHECK(p.startWithWindows);
    CHECK(p.startsWithWindows());

    CHECK(onStartup(s, kExe) == Sync::Created);
    CHECK(s.run == kCommand);
    CHECK(s.prefs[kDefaultsApplied] == 1);

    // Later starts leave it as it is.
    CHECK(onStartup(s, kExe) == Sync::Unchanged);
    CHECK(s.runWrites == 1);
}

TEST_CASE("startup: a Run entry the user removed elsewhere is not put back")
{
    FakeStore s;
    onStartup(s, kExe);
    s.run.reset(); // e.g. Autoruns, or the registry editor
    CHECK(onStartup(s, kExe) == Sync::Unchanged);
    CHECK_FALSE(s.run.has_value());
}

TEST_CASE("startup: someone else's Run value of that name is left alone")
{
    FakeStore s;
    s.run = L"\"C:\\Other\\Thing.exe\"";
    CHECK(onStartup(s, kExe) == Sync::Unchanged);
    CHECK(s.run == L"\"C:\\Other\\Thing.exe\"");
    s.prefs[kStartWithWindows] = 0;
    CHECK(onStartup(s, kExe) == Sync::Unchanged);
    CHECK(s.run.has_value());
}

TEST_CASE("startup: the switches add and remove the Run entry")
{
    FakeStore s;
    onStartup(s, kExe);

    setStartWithWindows(s, kExe, false);
    CHECK_FALSE(s.run.has_value());
    CHECK(s.prefs[kStartWithWindows] == 0);
    CHECK_FALSE(load(s).startsWithWindows());
    // Stays off on the next start.
    CHECK(onStartup(s, kExe) == Sync::Unchanged);
    CHECK_FALSE(s.run.has_value());

    setStartWithWindows(s, kExe, true);
    CHECK(s.run == kCommand);

    // No tray: no start with Windows either, but the choice is kept for later.
    setKeepInTray(s, kExe, false);
    CHECK_FALSE(s.run.has_value());
    CHECK(s.prefs[kStartWithWindows] == 1);
    CHECK_FALSE(load(s).startsWithWindows());
    CHECK(onStartup(s, kExe) == Sync::Unchanged);
    CHECK_FALSE(s.run.has_value());

    setKeepInTray(s, kExe, true);
    CHECK(s.run == kCommand);
    CHECK(load(s).startsWithWindows());
}

TEST_CASE("startup: the switch turned on before the first start counts as the choice")
{
    FakeStore s;
    setStartWithWindows(s, kExe, false);
    CHECK(s.prefs[kDefaultsApplied] == 1);
    CHECK(onStartup(s, kExe) == Sync::Unchanged);
    CHECK_FALSE(s.run.has_value());
}

TEST_CASE("startup: settings off but an old Run entry of ours is there")
{
    FakeStore s;
    s.prefs[kDefaultsApplied] = 1;
    s.prefs[kKeepInTray] = 0;
    s.run = kCommand;
    CHECK(onStartup(s, kExe) == Sync::Removed);
    CHECK_FALSE(s.run.has_value());
}

TEST_CASE("startup: a Run entry for a copy that moved points to this one")
{
    FakeStore s;
    s.prefs[kDefaultsApplied] = 1;
    s.run = L"\"D:\\Old\\NilesoftShellPro.exe\" --background";
    CHECK(onStartup(s, kExe) == Sync::Repaired);
    CHECK(s.run == kCommand);

    // Another copy that still exists (an MSI and a portable copy): left alone.
    s.run = L"\"D:\\Portable\\NilesoftShellPro.exe\" --background";
    s.files.insert(L"D:\\Portable\\NilesoftShellPro.exe");
    CHECK(onStartup(s, kExe) == Sync::Unchanged);
    CHECK(s.run == L"\"D:\\Portable\\NilesoftShellPro.exe\" --background");
}

TEST_CASE("startup: switched off in Windows' startup apps")
{
    FakeStore s;
    onStartup(s, kExe);
    s.disabled = true;
    auto p = load(s);
    CHECK(p.startWithWindows);
    CHECK(p.disabledInWindows);
    CHECK_FALSE(p.startsWithWindows()); // the switch shows what Windows will do
    // Only switching it on in the app turns it back on in Windows.
    CHECK(onStartup(s, kExe) == Sync::Unchanged);
    CHECK(s.disabled);
    setStartWithWindows(s, kExe, true);
    CHECK_FALSE(s.disabled);
    CHECK(load(s).startsWithWindows());
}

TEST_CASE("background: the update-check schedule")
{
    using namespace background;
    const std::time_t now = 1'800'000'000;
    CHECK(checkDue(now, 0));                                // never checked
    CHECK(checkDue(now, now + 3600));                       // the clock went backwards
    CHECK_FALSE(checkDue(now, now - 60));                   // just checked
    CHECK_FALSE(checkDue(now, now - 7 * 3600));             // 7 hours ago
    CHECK(checkDue(now, now - 8 * 3600));                   // 8 hours ago
    CHECK(checkDue(now, now - 3 * 24 * 3600));              // the PC slept for days
    CHECK_FALSE(checkDue(now, now - 3600, std::chrono::hours(2)));
    // Between six and twelve hours, looked at often enough not to drift far past it.
    CHECK(kInterval >= std::chrono::hours(6));
    CHECK(kInterval <= std::chrono::hours(12));
    CHECK(kTick <= kInterval / 4);
    CHECK(kFirstCheck >= std::chrono::seconds(30));
    CHECK(kFirstCheck <= std::chrono::minutes(5));
}

TEST_CASE("background: notify once per version, never for a skipped one")
{
    using namespace background;
    CHECK(shouldNotify("2.2.1", "", ""));
    CHECK_FALSE(shouldNotify("2.2.1", "2.2.1", ""));
    CHECK(shouldNotify("2.2.2", "2.2.1", ""));
    CHECK_FALSE(shouldNotify("2.2.2", "2.2.1", "2.2.2"));
    CHECK(shouldNotify("2.2.3", "2.2.1", "2.2.2"));
    CHECK_FALSE(shouldNotify("", "", ""));
}
