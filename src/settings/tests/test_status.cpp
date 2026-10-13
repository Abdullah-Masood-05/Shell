// `shell.exe -status` parsing and the problems Home shows for it (Status.h).

#include "doctest.h"

#include "Status.h"

namespace {

// A machine install with a second copy registered by an old setup, as the core reports it.
const char *kConflict = R"({
  "version": "2.0.0",
  "elevated": false,
  "installDir": "C:\\Program Files\\Nilesoft Shell",
  "dll": { "path": "C:\\Program Files\\Nilesoft Shell\\shell.dll", "exists": true },
  "registration": {
    "mode": "machine",
    "machine": { "registered": true, "dll": "C:\\Program Files\\Nilesoft Shell\\shell.dll" },
    "user": { "registered": false, "dll": "" }
  },
  "win11": { "applicable": true, "classicMenu": true, "machineTreatAs": "ours", "userTreatAs": "absent" },
  "config": { "path": "C:\\Users\\me\\AppData\\Roaming\\Nilesoft\\Shell\\shell.nss", "source": "appdata", "exists": true },
  "userConfigDir": "C:\\Users\\me\\AppData\\Roaming\\Nilesoft\\Shell",
  "conflicts": [
    { "clsid": "{BAE3934B-8A6A-4BFB-81BD-3FC599A1BAF3}", "role": "folderExtensions", "scope": "machine",
      "dll": "c:\\windows\\nilesoft\\shell.dll", "exists": true },
    { "clsid": "{BAE3934B-8A6A-4BFB-81BD-3FC599A1BAF4}", "role": "background", "scope": "machine",
      "dll": "C:\\Windows\\Nilesoft\\Shell.dll", "exists": true },
    { "clsid": "{BAE3934B-8A6A-4BFB-81BD-3FC599A1BAF5}", "role": "drive", "scope": "user",
      "dll": "D:\\Tools\\Shell\\shell.dll", "exists": false }
  ],
  "log": { "path": "C:\\ProgramData\\Nilesoft\\Shell\\shell.log" }
})";

// An older shell.exe: no "conflicts", no "log".
const char *kPlain = R"({
  "version": "1.9.19",
  "installDir": "C:\\Tools\\Shell",
  "dll": { "path": "C:\\Tools\\Shell\\shell.dll", "exists": true },
  "registration": { "mode": "user", "machine": { "registered": false }, "user": { "registered": true, "dll": "C:\\Tools\\Shell\\shell.dll" } },
  "win11": { "applicable": false },
  "config": { "path": "C:\\Tools\\Shell\\shell.nss", "source": "install", "exists": true }
})";

bool hasAction(const std::vector<StatusIssue> &issues, const char *action)
{
    for (const auto &i : issues)
        if (i.action == action)
            return true;
    return false;
}

} // namespace

TEST_CASE("status: conflicts and log.path are parsed")
{
    std::string error;
    auto s = parseStatus(kConflict, error);
    REQUIRE(s);
    REQUIRE(s->conflicts.size() == 3);
    CHECK(s->conflicts[0].clsid == "{BAE3934B-8A6A-4BFB-81BD-3FC599A1BAF3}");
    CHECK(s->conflicts[0].role == "folderExtensions");
    CHECK(s->conflicts[0].scope == "machine");
    CHECK(s->conflicts[0].dll == "c:\\windows\\nilesoft\\shell.dll");
    CHECK(s->conflicts[0].exists);
    CHECK_FALSE(s->conflicts[2].exists);
    CHECK(s->logPath == "C:\\ProgramData\\Nilesoft\\Shell\\shell.log");
    CHECK(s->activeLog() == "C:\\ProgramData\\Nilesoft\\Shell\\shell.log"); // the active dll is installDir's

    // The same dll under two CLSIDs (different case) is one copy.
    auto dlls = s->conflictDlls();
    REQUIRE(dlls.size() == 2);
    CHECK(dlls[0] == "c:\\windows\\nilesoft\\shell.dll");
    CHECK(dlls[1] == "D:\\Tools\\Shell\\shell.dll");
}

TEST_CASE("status: a conflict is the first issue, with the elevated fix")
{
    std::string error;
    auto s = parseStatus(kConflict, error);
    REQUIRE(s);
    auto issues = statusIssues(*s, true);
    REQUIRE(!issues.empty());
    const auto &first = issues.front();
    CHECK(first.kind == "error");
    CHECK(first.title == "Another copy of Nilesoft Shell is registered: c:\\windows\\nilesoft\\shell.dll");
    CHECK(first.message.find("Two copies stop the menu from appearing.") == 0);
    CHECK(first.message.find("D:\\Tools\\Shell\\shell.dll") != std::string::npos);
    CHECK(first.action == "fix-conflicts");
    CHECK(first.actionLabel == "Fix (needs administrator)");
    CHECK(issues.size() == 1); // registered for all users, personal config: nothing else
}

TEST_CASE("status: absent conflicts mean none; the log is next to the dll")
{
    std::string error;
    auto s = parseStatus(kPlain, error);
    REQUIRE(s);
    CHECK(s->conflicts.empty());
    CHECK(s->conflictDlls().empty());
    CHECK(s->logPath.empty());
    CHECK(s->activeLog().empty()); // not reported: Home falls back to the log next to the dll
    auto issues = statusIssues(*s, true);
    CHECK_FALSE(hasAction(issues, "fix-conflicts"));
    CHECK(hasAction(issues, "config-init")); // the shared config
}

TEST_CASE("status: a conflict naming installDir's dll is not another copy")
{
    std::string error;
    auto s = parseStatus(R"({"dll":{"path":"C:\\A\\shell.dll","exists":true},
                             "registration":{"mode":"user","user":{"registered":true,"dll":"C:\\A\\shell.dll"}},
                             "config":{"exists":true,"source":"appdata"},
                             "conflicts":[{"dll":"c:/a/SHELL.dll","exists":true}]})",
                         error);
    REQUIRE(s);
    CHECK(s->conflictDlls().empty());
    CHECK(statusIssues(*s, true).empty());

    // Run from a build folder while the installed copy is registered (what shell.exe
    // printed on a dev machine): the registered copy in use isn't flagged, the stale
    // c:\windows\nilesoft one is.
    auto dev = parseStatus(R"({"installDir":"D:\\src\\bin","dll":{"path":"D:\\src\\bin\\shell.dll","exists":true},
        "registration":{"mode":"machine","machine":{"registered":true,"dll":"C:\\Program Files\\Nilesoft Shell\\shell.dll"},
                        "user":{"registered":false,"dll":null}},
        "config":{"exists":true,"source":"appdata"},
        "conflicts":[
          {"clsid":"{BAE3934B-8A6A-4BFB-81BD-3FC599A1BAF1}","role":"contextMenu","scope":"machine","dll":"C:\\Program Files\\Nilesoft Shell\\shell.dll","exists":true},
          {"clsid":"{BAE3934B-8A6A-4BFB-81BD-3FC599A1BAF2}","role":"iconOverlay","scope":"machine","dll":"C:\\Program Files\\Nilesoft Shell\\shell.dll","exists":true},
          {"clsid":"{BAE3934B-8A6A-4BFB-81BD-3FC599A1BAF3}","role":"folderExtensions","scope":"machine","dll":"c:\\windows\\nilesoft\\shell.dll","exists":true}],
        "loadedInExplorer":[{"path":"c:\\windows\\nilesoft\\shell.dll","ours":false}],
        "log":{"path":"D:\\src\\bin\\shell.log","exists":false}})",
                           error);
    REQUIRE(dev);
    REQUIRE(dev->conflictDlls().size() == 1);
    CHECK(dev->conflictDlls()[0] == "c:\\windows\\nilesoft\\shell.dll");
    CHECK(dev->activeLog().empty()); // the log reported is the build folder's, not the active copy's

    // Installed normally, with a stale per-user registration that wins: that one is flagged.
    auto stale = parseStatus(R"({"installDir":"C:\\Program Files\\Nilesoft Shell",
        "dll":{"path":"C:\\Program Files\\Nilesoft Shell\\shell.dll","exists":true},
        "registration":{"mode":"both","machine":{"registered":true,"dll":"C:\\Program Files\\Nilesoft Shell\\shell.dll"},
                        "user":{"registered":true,"dll":"C:\\Old\\shell.dll"}},
        "config":{"exists":true,"source":"appdata"},
        "conflicts":[{"clsid":"{BAE3934B-8A6A-4BFB-81BD-3FC599A1BAF1}","role":"contextMenu","scope":"user","dll":"C:\\Old\\shell.dll","exists":true}]})",
                             error);
    REQUIRE(stale);
    REQUIRE(stale->conflictDlls().size() == 1);
    CHECK(stale->conflictDlls()[0] == "C:\\Old\\shell.dll");
    auto staleIssues = statusIssues(*stale, true);
    REQUIRE(staleIssues.size() == 1); // the conflict; not also "registered twice"
    CHECK(staleIssues[0].action == "fix-conflicts");

    // Malformed entries are skipped, not fatal.
    auto bad = parseStatus(R"({"conflicts":[1, "x", {"dll":"C:\\B\\shell.dll"}], "log": "nope"})", error);
    REQUIRE(bad);
    CHECK(bad->conflicts.size() == 1);
    CHECK(bad->logPath.empty());
}

TEST_CASE("status: registered for you and for all users with different copies")
{
    std::string error;
    auto s = parseStatus(R"({"registration":{"mode":"both",
                               "machine":{"registered":true,"dll":"C:\\Program Files\\Nilesoft Shell\\shell.dll"},
                               "user":{"registered":true,"dll":"C:\\Users\\me\\Shell\\shell.dll"}},
                             "config":{"exists":true,"source":"appdata"}})",
                         error);
    REQUIRE(s);
    auto issues = statusIssues(*s, true);
    REQUIRE(issues.size() == 1);
    CHECK(issues[0].kind == "warning");
    CHECK(issues[0].action == "unregister-user");

    // The same copy both ways is harmless.
    s->userDll = s->machineDll;
    CHECK(statusIssues(*s, true).empty());
}
