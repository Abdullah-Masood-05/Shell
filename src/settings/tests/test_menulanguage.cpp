// MenuLanguage: the right-click menu follows the Language setting through
// imports/settings-app-lang.nss. Every case works in a temporary folder (never the
// real %APPDATA%); the last one runs the real shell.exe from src/bin on it.

#include "doctest.h"

#include "MenuLanguage.h"

#include "nss/ConfigStore.h"
#include "nss/NssDocument.h"
#include "nss/OverridesWriter.h"
#include "nss/Text.h"

#include "../../shared/ConfigUpgrade.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;

namespace {

fs::path binDir() { return fs::path(SETTINGS_SOURCE_BIN).lexically_normal(); }
bool haveShell()
{
    return fs::is_regular_file(binDir() / L"shell.exe") && fs::is_regular_file(binDir() / L"shell.dll") &&
           fs::is_regular_file(binDir() / L"shell.nss");
}

struct Scratch
{
    fs::path path;
    Scratch()
    {
        static int n = 0;
        path = fs::temp_directory_path() /
               (L"settings-menulang-tests-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++n));
        fs::remove_all(path);
        fs::create_directories(path);
        // The shipped files, as a personal config has them.
        fs::copy_file(binDir() / L"shell.nss", path / L"shell.nss");
        fs::copy(binDir() / L"imports", path / L"imports", fs::copy_options::recursive);
    }
    ~Scratch()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    fs::path root() const { return path / L"shell.nss"; }
    fs::path file() const { return path / L"imports" / L"settings-app-lang.nss"; }
    std::string read(const fs::path &p) const
    {
        std::ifstream in(p, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    void write(const fs::path &p, const std::string &text) const { std::ofstream(p, std::ios::binary | std::ios::trunc) << text; }
};

size_t count(const std::string &text, const std::string &what)
{
    size_t n = 0;
    for (auto at = text.find(what); at != std::string::npos; at = text.find(what, at + 1))
        ++n;
    return n;
}

std::string runShell(const std::wstring &args, const fs::path &out)
{
    std::wstring cmd = L"\"" + (binDir() / L"shell.exe").wstring() + L"\" -silent " + args + L" \"-out:" + out.wstring() + L"\"";
    STARTUPINFOW si{ sizeof si };
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return {};
    WaitForSingleObject(pi.hProcess, 60000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    std::ifstream in(out, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("menu language: the app's language picks the menu's language file")
{
    const std::vector<std::string> files{ "ar", "de-DE", "en", "es-ES", "it", "pt-BR", "zh-CN", "zh-TW" };
    CHECK(menulang::pick("es", files) == "es-ES");     // the base language finds es-ES
    CHECK(menulang::pick("es-ES", files) == "es-ES");  // the exact name
    CHECK(menulang::pick("zh-TW", files) == "zh-TW");
    CHECK(menulang::pick("zh-CN", files) == "zh-CN");
    CHECK(menulang::pick("ar", files) == "ar");
    CHECK(menulang::pick("de", files) == "de-DE");
    CHECK(menulang::pick("pt", files) == "pt-BR");
    CHECK(menulang::pick("en", files) == "en");        // English is a choice of its own
    CHECK(menulang::pick("AR", files) == "ar");        // the case of the name doesn't matter
    CHECK(menulang::pick("", files).empty());          // Auto: the menu follows Windows
    CHECK(menulang::pick("xx", files).empty());        // no such menu language: same as Auto
    CHECK(menulang::pick("fr-CA", files).empty());
    CHECK(menulang::pick("en", {}).empty());

    // The ones that ship, as the files in src/bin/imports/lang are named.
    auto shipped = menulang::menuFiles(binDir() / L"imports" / L"lang");
    if (!shipped.empty()) {
        CHECK(menulang::pick("es", shipped) == "es-ES");
        CHECK(menulang::pick("zh-TW", shipped) == "zh-TW");
        CHECK(menulang::pick("ar", shipped) == "ar");
        CHECK(menulang::pick("en", shipped) == "en");
    }
}

TEST_CASE("menu language: the generated file reads back, and the app owns it")
{
    auto es = menulang::generate("es-ES");
    CHECK(es.rfind(std::string(nss::OverridesWriter::header), 0) == 0);
    CHECK(es.find("import lang 'lang/en.nss'\r\nimport lang 'lang/es-ES.nss'\r\n") != std::string::npos);
    CHECK(menulang::read(es) == "es-ES");
    CHECK(nss::NssDocument::fromText(es).diagnostics().empty());

    auto en = menulang::generate("en", nss::Eol::LF);
    CHECK(count(en, "import lang") == 1);
    CHECK(en.find('\r') == std::string::npos);
    CHECK(menulang::read(en) == "en");

    CHECK(menulang::read("// import lang 'lang/ar.nss'\r\n").empty()); // switched off
    CHECK(menulang::read("").empty());
    // A hand edit: the last language wins, as in shell.dll.
    CHECK(menulang::read("import lang 'lang/en.nss'\nimport lang \"lang\\zh-TW.nss\"\n") == "zh-TW");

    // Updates must treat it as the app's file, never as a shipped file the user edited.
    CHECK(config_upgrade::is_app_owned("imports/settings-app-lang.nss"));
}

TEST_CASE("menu language: writes and removes the file and its import, without churn")
{
    if (!haveShell()) {
        MESSAGE("shell.nss not in src/bin; skipped");
        return;
    }
    Scratch s;
    const auto original = s.read(s.root());
    const std::string importLine = "import 'imports/settings-app-lang.nss'";

    auto r = menulang::apply(s.root(), "es");
    REQUIRE(r.ok);
    CHECK(r.changed);
    CHECK_FALSE(r.snapshotId.empty()); // the backup the other pages take too
    REQUIRE(fs::is_regular_file(s.file()));
    CHECK(menulang::read(s.read(s.file())) == "es-ES");
    auto rootText = s.read(s.root());
    CHECK(count(rootText, importLine) == 1);
    // At the end, after the `import lang` lines, so it wins.
    CHECK(rootText.find(importLine) > rootText.find("import lang"));
    CHECK(rootText.rfind("import ") == rootText.find(importLine));

    // The same language again: nothing is rewritten, no new backup.
    const auto fileBytes = s.read(s.file());
    const auto rootBytes = s.read(s.root());
    const auto snapshots = nss::ConfigStore(s.root()).snapshots().size();
    r = menulang::apply(s.root(), "es");
    CHECK(r.ok);
    CHECK_FALSE(r.changed);
    CHECK(s.read(s.file()) == fileBytes);
    CHECK(s.read(s.root()) == rootBytes);
    CHECK(nss::ConfigStore(s.root()).snapshots().size() == snapshots);

    // A file the user edited by hand stays while the language is the same.
    s.write(s.file(), fileBytes + "// mine\r\n");
    r = menulang::apply(s.root(), "es");
    CHECK_FALSE(r.changed);
    CHECK(s.read(s.file()) == fileBytes + "// mine\r\n");

    // Another language replaces it; the import isn't added twice.
    r = menulang::apply(s.root(), "zh-TW");
    REQUIRE(r.ok);
    CHECK(r.changed);
    CHECK(menulang::read(s.read(s.file())) == "zh-TW");
    CHECK(count(s.read(s.root()), importLine) == 1);

    r = menulang::apply(s.root(), "en");
    CHECK(r.changed);
    CHECK(menulang::read(s.read(s.file())) == "en");

    // A language with no menu file: as Auto, nothing is written... and what was there goes.
    r = menulang::apply(s.root(), "xx");
    CHECK(r.ok);
    CHECK(r.changed);
    CHECK_FALSE(fs::exists(s.file()));
    CHECK(count(s.read(s.root()), "settings-app-lang") == 0);

    // Auto with nothing to remove does nothing at all, not even a backup.
    const auto before = nss::ConfigStore(s.root()).snapshots().size();
    r = menulang::apply(s.root(), "");
    CHECK(r.ok);
    CHECK_FALSE(r.changed);
    CHECK(nss::ConfigStore(s.root()).snapshots().size() == before);
    // Unknown language, never written: also nothing.
    r = menulang::apply(s.root(), "xx");
    CHECK_FALSE(r.changed);
    CHECK_FALSE(fs::exists(s.file()));

    // Pick and then Auto: shell.nss is as it was.
    REQUIRE(menulang::apply(s.root(), "ar").changed);
    REQUIRE(fs::exists(s.file()));
    r = menulang::apply(s.root(), "");
    CHECK(r.changed);
    CHECK_FALSE(fs::exists(s.file()));
    CHECK(s.read(s.root()) == original);
}

TEST_CASE("menu language: shell.exe accepts the file and the menu shows that language (real shell.exe)")
{
    if (!haveShell()) {
        MESSAGE("shell.exe/shell.dll/shell.nss not in src/bin; skipped");
        return;
    }
    Scratch s;
    const auto check = [&](const wchar_t *what) { return runShell(std::wstring(what) + L" \"" + s.root().wstring() + L"\"", s.path / L"out.json"); };

    REQUIRE(menulang::apply(s.root(), "es").ok);
    CHECK(check(L"-config:check").find("\"ok\": true") != std::string::npos);
    auto tree = check(L"-config:tree -context:back");
    CHECK(tree.find("Personalizaci") != std::string::npos); // loc.personalization in Spanish

    REQUIRE(menulang::apply(s.root(), "zh-TW").ok);
    CHECK(check(L"-config:check").find("\"ok\": true") != std::string::npos);
    CHECK(check(L"-config:tree -context:back").find("\xE5\x80\x8B\xE4\xBA\xBA\xE5\x8C\x96") != std::string::npos); // 個人化

    REQUIRE(menulang::apply(s.root(), "en").ok);
    CHECK(check(L"-config:tree -context:back").find("Personalization") != std::string::npos);
}
