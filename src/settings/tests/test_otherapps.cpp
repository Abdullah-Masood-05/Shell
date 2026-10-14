// Other apps' menu items (src/OtherApps.cpp): turning registry verbs and shell
// extension handlers into the Open with page's list. Fixture data only (taken
// from a real PC with Git, VLC, File Pilot, PowerShell 7 and 7-Zip); the real
// registry is never read here.

#include "doctest.h"

#include "OtherApps.h"

#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace otherapps;

namespace {

Verb verb(std::string type, std::string key, std::string text, std::string command, std::string mui = {})
{
    Verb v;
    v.type = std::move(type);
    v.key = std::move(key);
    v.text = std::move(text);
    v.command = std::move(command);
    v.muiVerb = std::move(mui);
    return v;
}

Handler handler(std::string type, std::string name, std::string clsid, std::string className, std::string server)
{
    Handler h;
    h.type = std::move(type);
    h.name = std::move(name);
    h.clsid = std::move(clsid);
    h.className = std::move(className);
    h.server = std::move(server);
    return h;
}

Scan fixture()
{
    Scan s;
    const std::string git = "\"C:\\Program Files\\Git\\cmd\\git-gui.exe\" \"--working-dir\" \"%1\"";
    const std::string fp = "C:\\Users\\me\\AppData\\Local\\Voidstar\\FilePilot\\FPilot.exe \"%1\"";
    // Third-party verbs.
    s.verbs.push_back(verb("*", "OpenInFilePilot", "Open in File Pilot", fp));
    s.verbs.push_back(verb("Directory", "OpenInFilePilot", "Open in File Pilot", fp));
    auto fpUser = verb("Directory", "OpenInFilePilot", "Open in File Pilot", fp);
    fpUser.user = true;
    s.verbs.push_back(fpUser);
    s.verbs.push_back(verb("Directory", "git_gui", "Open Git &GUI here", git));
    s.verbs.push_back(verb("Directory\\Background", "git_gui", "Open Git &GUI here", git));
    s.verbs.push_back(verb("Directory", "git_shell", "Open Git Ba&sh here", "\"C:\\Program Files\\Git\\git-bash.exe\" \"--cd=%1\""));
    s.verbs.push_back(verb("Directory", "AddToPlaylistVLC", "Add to VLC media player's Playlist",
                           "\"C:\\Program Files\\VideoLAN\\VLC\\vlc.exe\" --started-from-file --playlist-enqueue \"%1\""));
    auto pwsh7 = verb("Directory", "PowerShell7x64", "", "", "&PowerShell 7");
    pwsh7.submenu = true;
    pwsh7.icon = "C:\\Program Files\\PowerShell\\7\\pwsh.exe,0";
    s.verbs.push_back(pwsh7);
    s.verbs.push_back(verb("Directory\\Background", "Open Alacritty here", "",
                           "C:\\Program Files\\Alacritty\\alacritty.exe --working-directory \"%v\""));
    s.verbs.push_back(verb("Drive", "noctty", "Open noctty here", "\"C:\\Program Files\\noctty\\noctty.exe\" --working-directory=\"%V\\.\""));
    s.verbs.push_back(verb("Directory", "ZCode.OpenInZCode", "Open in ZCode",
                           "\"C:\\Users\\me\\AppData\\Local\\Programs\\ZCode\\ZCode.exe\" --open-workspace \"%1\"", "Open in ZCode"));
    auto shift = verb("Directory", "shiftonly", "Only with Shift", "D:\\Tools\\x.exe \"%1\"");
    shift.extended = true;
    s.verbs.push_back(shift);
    s.verbs.push_back(verb("Directory", "indirect", "", "D:\\Tools\\y.exe \"%1\"", "@D:\\Tools\\y.dll,-12"));
    s.verbs.push_back(verb("Directory", "unreadable", "", "D:\\Tools\\z.exe \"%1\"", "@D:\\Tools\\z.dll,-1"));

    // Windows' own verbs: left out.
    auto cmd = verb("Directory", "cmd", "@shell32.dll,-8506", "cmd.exe /s /k pushd \"%V\"");
    cmd.extended = true;
    s.verbs.push_back(cmd);
    s.verbs.push_back(verb("Directory", "find", "", "%SystemRoot%\\Explorer.exe"));
    s.verbs.push_back(verb("Drive", "pintohome", "", "", "@shell32.dll,-51377"));
    s.verbs.push_back(verb("Directory\\Background", "WSL", "@wsl.exe,-2", "wsl.exe --cd \"%V\""));
    s.verbs.push_back(verb("Drive", "encrypt-bde", "@C:\\WINDOWS\\System32\\fvewiz.dll,-920",
                           "C:\\WINDOWS\\System32\\BitLockerWizard.exe %1 T"));
    s.verbs.push_back(verb("Directory", "Bitlocker2", "Turn on", "c:\\windows\\system32\\other.exe %1"));
    s.verbs.push_back(verb("*", "UpdateEncryptionSettingsWork", "", "", "@efscore.dll,-101"));
    s.verbs.push_back(verb("Directory", "opennewwindow", "", ""));
    s.verbs.push_back(verb("Directory", "Windows.Share", "Share", ""));
    auto hidden = verb("*", "removeit", "Never shown", "D:\\Tools\\r.exe");
    hidden.hiddenByApp = true; // ProgrammaticAccessOnly
    s.verbs.push_back(hidden);

    // Handlers.
    const std::string sevenZip = "{23170F69-40C1-278A-1000-000100020000}";
    s.handlers.push_back(handler("*", "7-Zip", sevenZip, "", "C:\\Program Files\\7-Zip\\7-zip.dll"));
    s.handlers.push_back(handler("Directory", "7-Zip", sevenZip, "", "C:\\Program Files\\7-Zip\\7-zip.dll"));
    s.handlers.push_back(handler("Directory\\Background", "NvCplDesktopContext", "{3D1975AF-48C6-4f8e-A182-BE0E08FA86A9}",
                                 "NVIDIA CPL Context Menu Extension",
                                 "C:\\WINDOWS\\System32\\DriverStore\\FileRepository\\nv.inf_amd64\\nvshext.dll"));
    s.handlers.push_back(handler("Directory", "{11111111-2222-3333-4444-555555555555}", "{11111111-2222-3333-4444-555555555555}",
                                 "Acme Sync Menu", "C:\\Program Files\\Acme\\menu.dll"));
    // Windows' and Shell's own handlers: left out.
    s.handlers.push_back(handler("*", " @nilesoft.shell", "{BAE3934B-8A6A-4BFB-81BD-3FC599A1BAF1}", "Nilesoft.Shell",
                                 "C:\\Program Files\\Nilesoft Shell\\shell.dll"));
    s.handlers.push_back(handler("*", "Sharing", "{f81e9010-6ea4-11ce-a7ff-00aa003ca9f6}", "", "C:\\WINDOWS\\system32\\ntshrui.dll"));
    s.handlers.push_back(handler("Directory\\Background", "Gadgets", "{6B9228DA-9C15-419e-856C-19E768A13BDC}", "",
                                 "C:\\Program Files\\Windows Sidebar\\sbdrop.dll"));
    s.handlers.push_back(handler("Folder", "Gone", "{99999999-2222-3333-4444-555555555555}", "", "")); // not registered
    return s;
}

std::string resolver(const std::string &s)
{
    return s == "@D:\\Tools\\y.dll,-12" ? std::string("&Indirect title...") : std::string{};
}

const Item *find(const std::vector<Item> &items, const std::string &title)
{
    auto it = std::find_if(items.begin(), items.end(), [&](const Item &i) { return i.title == title; });
    return it == items.end() ? nullptr : &*it;
}

} // namespace

TEST_CASE("other apps: titles as Shell matches them")
{
    CHECK(menuTitle("Open Git &GUI here") == "Open Git GUI here");
    CHECK(menuTitle("&PowerShell 7") == "PowerShell 7");
    CHECK(menuTitle("Save && exit") == "Save & exit");
    CHECK(menuTitle("  Add to archive...  ") == "Add to archive");
    CHECK(menuTitle("Run\tCtrl+R") == "Run");
    CHECK(menuTitle("&") == "");
}

TEST_CASE("other apps: programs and Windows' own files")
{
    CHECK(commandProgram("\"C:\\Program Files\\Git\\git-bash.exe\" \"--cd=%1\"") == "C:\\Program Files\\Git\\git-bash.exe");
    CHECK(commandProgram("C:\\Program Files\\Alacritty\\alacritty.exe --working-directory \"%v\"") ==
          "C:\\Program Files\\Alacritty\\alacritty.exe");
    CHECK(commandProgram("cmd.exe /s /k pushd \"%V\"") == "cmd.exe");
    CHECK(commandProgram("") == "");

    const std::string root = "C:\\Windows";
    CHECK(isWindowsFile("@shell32.dll,-8506", root));
    CHECK(isWindowsFile("@%SystemRoot%\\system32\\shell32.dll,-1", root));
    CHECK(isWindowsFile("C:\\WINDOWS\\System32\\BitLockerWizard.exe", root));
    CHECK(isWindowsFile("cmd.exe", root));
    CHECK(isWindowsFile("C:\\Program Files\\Windows Defender\\shellext.dll", root));
    CHECK(!isWindowsFile("C:\\WINDOWS\\System32\\DriverStore\\FileRepository\\x\\nvshext.dll", root)); // a vendor's driver
    CHECK(!isWindowsFile("C:\\Program Files\\Git\\git-bash.exe", root));
    CHECK(!isWindowsFile("pwsh.exe", root));
    CHECK(!isWindowsFile("@C:\\Program Files\\App\\app.dll,-100", root));
    CHECK(!isWindowsFile("", root));
    CHECK(!isWindowsFile("C:\\WindowsApps2\\x.exe", root)); // not under C:\Windows\

    CHECK(describeWhere(Files) == "Files");
    CHECK(describeWhere(Folders | Background) == "Folders and folder background");
    CHECK(describeWhere(Files | Folders | Background | Drives) == "Files, folders, folder background and drives");
}

TEST_CASE("other apps: one row per title, Windows' own left out")
{
    auto items = collect(fixture(), resolver, "C:\\Windows");
    std::vector<std::string> titles;
    for (const auto &i : items)
        titles.push_back(i.title);
    // Sorted by title; Windows' verbs and handlers, Shell's own handler, the
    // never-shown verb, the unregistered handler and the unreadable string are not there.
    CHECK(titles == std::vector<std::string>{ "7-Zip", "Acme Sync Menu", "Add to VLC media player's Playlist",
                                              "Indirect title", "NvCplDesktopContext", "Only with Shift",
                                              "Open Alacritty here", "Open Git Bash here", "Open Git GUI here",
                                              "Open in File Pilot", "Open in ZCode", "Open noctty here", "PowerShell 7" });

    auto *gui = find(items, "Open Git GUI here");
    REQUIRE(gui);
    CHECK(gui->kind == "verb");
    CHECK(gui->name == "git_gui");
    CHECK(gui->titleKnown);
    CHECK(gui->where == (Folders | Background));
    CHECK(gui->program == "C:\\Program Files\\Git\\cmd\\git-gui.exe");
    CHECK(gui->keys == std::vector<std::string>{ "HKCR\\Directory\\Background\\shell\\git_gui", "HKCR\\Directory\\shell\\git_gui" });

    auto *fp = find(items, "Open in File Pilot"); // files, folders, and HKCU too: one row
    REQUIRE(fp);
    CHECK(fp->where == (Files | Folders));
    CHECK(fp->keys.size() == 3);

    auto *pwsh = find(items, "PowerShell 7"); // a submenu, its icon is the program
    REQUIRE(pwsh);
    CHECK(pwsh->program == "C:\\Program Files\\PowerShell\\7\\pwsh.exe");

    CHECK(find(items, "Only with Shift")->shiftOnly);
    CHECK(!gui->shiftOnly);
    CHECK(find(items, "Open Alacritty here")->name == "Open Alacritty here"); // the key is the title

    auto *zip = find(items, "7-Zip");
    REQUIRE(zip);
    CHECK(zip->kind == "handler");
    CHECK(!zip->titleKnown);
    CHECK(zip->name == "7-Zip");
    CHECK(zip->where == (Files | Folders));
    CHECK(zip->keys.size() == 2);
    auto *acme = find(items, "Acme Sync Menu"); // named by its CLSID: the class name stands in
    REQUIRE(acme);
    CHECK(acme->name == "Acme Sync Menu");

    // No resolver: an indirect title can't be shown or matched, so it's left out.
    auto plain = collect(fixture(), nullptr, "C:\\Windows");
    CHECK(!find(plain, "Indirect title"));
    CHECK(plain.size() == items.size() - 1);
    CHECK(collect({}, resolver, "C:\\Windows").empty());
}

TEST_CASE("other apps: the file a command line needs")
{
    auto env = [](std::string_view name) -> std::string {
        if (name == "LOCALAPPDATA")
            return "C:\\Users\\me\\AppData\\Local";
        if (name == "ProgramFiles")
            return "C:\\Program Files";
        if (name == "ProgramFiles(x86)")
            return "C:\\Program Files (x86)";
        return {};
    };
    // Variables: set ones expanded; %1, %V, %*, %L and unset ones left alone.
    CHECK(expandVars("%LOCALAPPDATA%\\x %1 %V %* %L %NOPE%", env) == "C:\\Users\\me\\AppData\\Local\\x %1 %V %* %L %NOPE%");
    CHECK(expandVars("\"%ProgramFiles(x86)%\\a.exe\" \"%1\"", env) == "\"C:\\Program Files (x86)\\a.exe\" \"%1\"");
    CHECK(expandVars("%1%LOCALAPPDATA%", env) == "%1C:\\Users\\me\\AppData\\Local");
    CHECK(expandVars("%LOCALAPPDATA%", nullptr) == "%LOCALAPPDATA%");

    // Quoted, unquoted with spaces, with %1/%V arguments.
    CHECK(commandTarget("\"C:\\Users\\me\\AppData\\Local\\Programs\\ZCode\\ZCode.exe\" --open-workspace \"%1\"") ==
          "C:\\Users\\me\\AppData\\Local\\Programs\\ZCode\\ZCode.exe");
    CHECK(commandTarget("C:\\Program Files\\Alacritty\\alacritty.exe --working-directory \"%V\"") ==
          "C:\\Program Files\\Alacritty\\alacritty.exe");
    CHECK(commandTarget("  \"D:\\Tools\\x.exe\"  ") == "D:\\Tools\\x.exe");
    CHECK(commandTarget("D:\\Tools\\noargs.exe") == "D:\\Tools\\noargs.exe");
    // Environment variables, quoted or not.
    CHECK(commandTarget("\"%LOCALAPPDATA%\\Programs\\ZCode\\ZCode.exe\" \"%1\"", env) ==
          "C:\\Users\\me\\AppData\\Local\\Programs\\ZCode\\ZCode.exe");
    CHECK(commandTarget("%ProgramFiles%\\App\\app.exe %V", env) == "C:\\Program Files\\App\\app.exe");
    // Windows' wrappers: the file they run.
    CHECK(commandTarget("rundll32.exe \"C:\\Program Files\\App\\menu.dll\",Run %1") == "C:\\Program Files\\App\\menu.dll");
    CHECK(commandTarget("C:\\Windows\\System32\\rundll32.exe D:\\App\\menu.dll,Run \"%1\"") == "D:\\App\\menu.dll");
    CHECK(commandTarget("rundll32.exe shell32.dll,Control_RunDLL") == "rundll32.exe"); // System32's: the wrapper
    CHECK(commandTarget("cmd.exe /c \"\"C:\\Program Files\\App\\run.cmd\" \"%V\"\"") == "C:\\Program Files\\App\\run.cmd");
    CHECK(commandTarget("cmd /s /k \"D:\\Tools\\go.bat\" %1") == "D:\\Tools\\go.bat");
    CHECK(commandTarget("cmd.exe /s /k pushd \"%V\"") == "cmd.exe");
    CHECK(commandTarget("wscript.exe //B \"%ProgramFiles%\\App\\open.vbs\" \"%1\"", env) == "C:\\Program Files\\App\\open.vbs");
    // Nothing to go on.
    CHECK(commandTarget("") == "");
    CHECK(commandTarget("code \"%1\"") == "code");

    CHECK(isFullPath("C:\\x.exe"));
    CHECK(isFullPath("\\\\server\\share\\x.exe"));
    CHECK(!isFullPath("x.exe"));
    CHECK(!isFullPath("Tools\\x.exe"));
    CHECK(!isFullPath("\\\\?\\C:\\x"));
}

TEST_CASE("other apps: leftovers of uninstalled programs (temporary folder)")
{
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / ("settings-otherapps-tests-" + std::to_string(GetCurrentProcessId()));
    fs::remove_all(dir);
    fs::create_directories(dir / "Installed");
    fs::create_directories(dir / "OnlyAFolder.exe"); // a folder isn't a program
    auto touch = [](const fs::path &p) { std::ofstream(p, std::ios::binary) << "x"; };
    touch(dir / "Installed" / "app.exe");
    touch(dir / "Installed" / "ext.dll");
    touch(dir / "Installed" / "menu.dll");
    const std::string root = dir.string();
    FileSystem files = realFiles();
    auto realEnv = files.env;
    files.env = [&](std::string_view name) { return name == "FIXTURE" ? root : realEnv(name); };

    Scan s;
    s.verbs.push_back(verb("Directory", "installed", "Open in App", "\"%FIXTURE%\\Installed\\app.exe\" \"%1\""));
    s.verbs.push_back(verb("Directory", "ZCode.OpenInZCode", "Open in ZCode",
                           "\"" + root + "\\Uninstalled\\ZCode\\ZCode.exe\" --open-workspace \"%1\""));
    s.verbs.push_back(verb("Directory\\Background", "folder", "Open folder thing", root + "\\OnlyAFolder.exe %V"));
    s.verbs.push_back(verb("Directory", "onpath", "Open on PATH", "code \"%1\"")); // can't tell: not marked
    s.verbs.push_back(verb("*", "dll", "Run menu", "rundll32.exe \"" + root + "\\Installed\\menu.dll\",Run %1"));
    s.verbs.push_back(verb("*", "deaddll", "Run gone", "rundll32.exe \"" + root + "\\Gone\\gone.dll\",Run %1"));
    // The same title twice: one key's program is there, so it isn't a leftover.
    s.verbs.push_back(verb("*", "half", "Half gone", "\"" + root + "\\Gone\\half.exe\" \"%1\""));
    s.verbs.push_back(verb("Directory", "half", "Half gone", "\"%FIXTURE%\\Installed\\app.exe\" \"%1\""));
    s.handlers.push_back(handler("*", "Good", "{11111111-0000-0000-0000-000000000001}", "", root + "\\Installed\\ext.dll"));
    s.handlers.push_back(handler("*", "Stale", "{11111111-0000-0000-0000-000000000002}", "", "\"" + root + "\\Gone\\stale.dll\""));
    s.handlers.push_back(handler("*", "ByName", "{11111111-0000-0000-0000-000000000003}", "", "Vendor\\ext.dll")); // relative: can't tell

    auto items = collect(s, resolver, "C:\\Windows", files);
    auto missing = [&](const std::string &title) {
        auto *i = find(items, title);
        REQUIRE_MESSAGE(i, title);
        return i->missing;
    };
    CHECK(missing("Open in App") == "");
    CHECK(missing("Open in ZCode") == root + "\\Uninstalled\\ZCode\\ZCode.exe");
    CHECK(missing("Open folder thing") == root + "\\OnlyAFolder.exe");
    CHECK(missing("Open on PATH") == "");
    CHECK(missing("Run menu") == "");
    CHECK(missing("Run gone") == root + "\\Gone\\gone.dll");
    CHECK(missing("Half gone") == "");
    CHECK(missing("Good") == "");
    CHECK(missing("Stale") == root + "\\Gone\\stale.dll");
    CHECK(missing("ByName") == "");
    // A third-party DLL run through rundll32 is listed, with the DLL as its program.
    CHECK(find(items, "Run menu")->program == root + "\\Installed\\menu.dll");

    // Without a file system nothing is marked.
    for (const auto &i : collect(s, resolver, "C:\\Windows"))
        CHECK(i.missing.empty());
    std::error_code ec;
    fs::remove_all(dir, ec);
}
