// Snippets (src/Snippets.h): the bundled catalog, the RubicBG catalog and its downloads,
// catalog parsing, the app-owned list file and the import line in shell.nss, requirement
// checks, refreshing unedited copies, and every bundled snippet through
// `shell.exe -config:check` (skipped without shell.exe). RubicBG's snippets go through the
// same check when NSP_RUBICBG_DIR names a checkout of the pinned commit
// (scripts/check-snippets.ps1 downloads them instead).

#include "doctest.h"

#include "Snippets.h"

#include "nss/ConfigStore.h"
#include "nss/Text.h"

#include <nlohmann/json.hpp>

#include <windows.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <set>

namespace fs = std::filesystem;
using namespace snippets;

namespace {

fs::path binDir() { return fs::path(SETTINGS_SOURCE_BIN).lexically_normal(); }

std::string readBytes(const fs::path &p)
{
    std::string bytes;
    nss::readFile(p, bytes);
    return bytes;
}

void writeBytes(const fs::path &p, std::string_view bytes)
{
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), std::streamsize(bytes.size()));
}

struct TempDir
{
    fs::path path;
    TempDir()
    {
        static std::atomic<int> n{ 0 };
        path = fs::temp_directory_path() / (L"snippets-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++n));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

// A catalog of one or two made-up snippets.
std::string entry(const std::string &id, const std::string &text, const std::string &extra = {})
{
    return "{\"id\":\"" + id + "\",\"title\":\"T " + id + "\",\"description\":\"D\",\"category\":\"Files\",\"author\":\"A\","
           "\"license\":\"MIT\",\"sha256\":\"" + hashText(text) + "\"" + extra + "}";
}

Catalog catalogOf(const std::map<std::string, std::string> &files, const std::string &entries)
{
    return parseCatalog("{\"format\":1,\"snippets\":[" + entries + "]}",
                        [&](const std::string &id) -> std::optional<std::string> {
                            auto it = files.find(id);
                            if (it == files.end())
                                return std::nullopt;
                            return it->second;
                        },
                        "test");
}

Probe fakeProbe(std::set<std::wstring> files, std::set<std::wstring> path)
{
    Probe p;
    p.fileExists = [files](const std::wstring &f) { return files.count(f) > 0; };
    p.expand = [](const std::wstring &t) {
        std::wstring out = t;
        auto rep = [&](const std::wstring &from, const std::wstring &to) {
            for (size_t pos; (pos = out.find(from)) != std::wstring::npos;)
                out.replace(pos, from.size(), to);
        };
        rep(L"%ProgramFiles%", L"C:\\PF");
        rep(L"%LOCALAPPDATA%", L"C:\\LAD");
        return out;
    };
    p.onPath = [path](const std::wstring &exe) { return path.count(exe) > 0; };
    return p;
}

} // namespace

TEST_CASE("snippets: the bundled catalog matches src/bin/snippets")
{
    auto catalog = BundledSource{}.load();
    for (const auto &e : catalog.errors)
        MESSAGE(e);
    CHECK(catalog.errors.empty());
    CHECK(catalog.snippets.size() >= 8);
    CHECK(catalog.snippets.size() <= 12);

    std::set<std::string> ids;
    for (const auto &s : catalog.snippets) {
        CAPTURE(s.id);
        ids.insert(s.id);
        // The embedded copy is the file in the repository, and the hash is current
        // (scripts/update-snippet-hashes.ps1).
        auto onDisk = readBytes(binDir() / "snippets" / (s.id + ".nss"));
        CHECK(normalize(onDisk) == s.text);
        CHECK(hashText(onDisk) == s.sha256);
        CHECK(s.author == "Nilesoft Shell Pro");
        CHECK(s.license == "MIT");
        CHECK(!s.description.empty());
        CHECK(s.source.rfind("https://", 0) == 0);
        CHECK(categoryLabel(s.category) != nullptr);
        CHECK(s.origin == "bundled");
        for (const char *lang : { "zh-CN", "zh-TW" }) {
            CAPTURE(lang);
            auto t = s.translations.find(lang);
            REQUIRE(t != s.translations.end());
            CHECK(!t->second.title.empty());
            CHECK(!t->second.description.empty());
            CHECK(s.titleIn(lang) == t->second.title);
        }
        CHECK(s.titleIn("de") == s.title);
        // The text starts with the name and the license, the way the page describes them.
        CHECK(s.text.find("MIT license") != std::string::npos);
        // No em or en dashes in what users read.
        CHECK(s.text.find("\xE2\x80\x94") == std::string::npos);
        CHECK(s.description.find("\xE2\x80\x94") == std::string::npos);
    }
    // Every file in the folder is in the catalog.
    for (const auto &f : fs::directory_iterator(binDir() / "snippets"))
        if (f.path().extension() == ".nss")
            CHECK_MESSAGE(ids.count(f.path().stem().string()) == 1, f.path().filename().string());

    const auto *admin = catalog.find("admin-terminal-here");
    REQUIRE(admin);
    CHECK(admin->risk == Risk::Admin);
    const auto *vt = catalog.find("virustotal-lookup");
    REQUIRE(vt);
    CHECK(vt->risk == Risk::Network);
    const auto *seven = catalog.find("sevenzip-each");
    REQUIRE(seven);
    CHECK(seven->needs == std::vector<std::string>{ "7-zip" });
}

TEST_CASE("snippets: catalog parsing")
{
    const std::string good = "item(title='x' cmd='a')\n";
    std::map<std::string, std::string> files = { { "good", good }, { "bad-hash", "item()" }, { "twice", good } };

    auto c = catalogOf(files, entry("good", good,
                                    ",\"risk\":\"Admin\",\"requires\":[\"7-zip\",\"\",3],\"min_version\":\"2.0.6\","
                                    "\"translations\":{\"zh-CN\":{\"title\":\"\xE5\xA5\xBD\"},\"x\":5},\"unknown\":true") +
                                  "," + entry("Bad_Id", good) + "," + entry("missing", good) + "," +
                                  "{\"id\":\"bad-hash\",\"title\":\"x\",\"sha256\":\"" + hashText("other") + "\"}," +
                                  entry("twice", good) + "," + entry("twice", good) + "," + "{\"id\":\"untitled\"}," + "7");
    REQUIRE(c.snippets.size() == 2);
    const auto &s = c.snippets[0];
    CHECK(s.id == "good");
    CHECK(s.category == "files"); // lower case
    CHECK(s.risk == Risk::Admin);
    CHECK(s.needs == std::vector<std::string>{ "7-zip" });
    CHECK(s.minVersion == "2.0.6");
    CHECK(s.text == good);
    CHECK(s.origin == "test");
    CHECK(s.titleIn("zh-CN") == "\xE5\xA5\xBD");
    CHECK(s.descriptionIn("zh-CN") == "D"); // no translated description: English
    CHECK(c.snippets[1].id == "twice");
    CHECK(c.errors.size() == 6); // Bad_Id, missing, bad-hash, the second twice, untitled, 7
    CHECK(c.find("good") == &c.snippets[0]);
    CHECK(c.find("nope") == nullptr);

    CHECK(catalogOf(files, "").snippets.empty());
    auto broken = parseCatalog("not json", {}, "test");
    CHECK(broken.snippets.empty());
    CHECK(broken.errors.size() == 1);

    CHECK(validId("copy-file-hash"));
    CHECK(validId("7zip"));
    CHECK_FALSE(validId(""));
    CHECK_FALSE(validId("-x"));
    CHECK_FALSE(validId("a/b"));
    CHECK_FALSE(validId("..\\x"));
    CHECK_FALSE(validId("Upper"));
}

TEST_CASE("snippets: text normalization and hashes")
{
    CHECK(normalize("\xEF\xBB\xBF" "a\r\nb\rc\n") == "a\nb\nc\n");
    CHECK(hashText("a\r\nb") == hashText("\xEF\xBB\xBF" "a\nb"));
    CHECK(hashText("a\nb") != hashText("a\nb\n"));
    CHECK(hashText("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(toCrlf("a\nb\r\nc") == "a\r\nb\r\nc");
}

TEST_CASE("snippets: the list file")
{
    const std::string h1(64, 'a'), h2(64, 'b');
    auto text = generateList({ { "copy-file-hash", h1 }, { "compress-zip", h2 } });
    CHECK(text.rfind(std::string(kListHeader), 0) == 0);
    CHECK(text.find("\r\n// snippet copy-file-hash sha256=" + h1 + "\r\nimport 'snippets/copy-file-hash.nss'\r\n") !=
          std::string::npos);
    CHECK(text.find("import 'snippets/compress-zip.nss'\r\n") != std::string::npos);
    auto back = parseList(text);
    REQUIRE(back.size() == 2);
    CHECK(back[0].id == "copy-file-hash");
    CHECK(back[0].sha256 == h1);
    CHECK(back[1].id == "compress-zip");
    CHECK(back[1].sha256 == h2);

    // Other spellings of the import line count; other lines don't; no hash is "".
    auto loose = parseList("import \"snippets\\a.nss\"\n  IMPORT  'Snippets/B.nss'  \n// import 'snippets/c.nss'\n"
                           "import 'imports/snippets/d.nss'\nimport 'snippets/../e.nss'\nimport 'snippets/a.nss'\n"
                           "// snippet a sha256=nothex\n");
    REQUIRE(loose.size() == 2);
    CHECK(loose[0].id == "a");
    CHECK(loose[0].sha256.empty());
    CHECK(loose[1].id == "b");
    CHECK(parseList(generateList({})).empty());
    CHECK(copyPath("x") == "imports/snippets/x.nss");
}

TEST_CASE("snippets: turning one on and off writes the right files and import lines")
{
    const std::string v1 = "item(title='one' cmd='a')\n";
    std::map<std::string, std::string> files = { { "one", v1 }, { "two", v1 } };
    auto c = catalogOf(files, entry("one", v1) + "," + entry("two", v1));

    ConfigFiles config; // nothing yet
    auto on = planSet(c, config, "one", true);
    CHECK(on.error.empty());
    CHECK(on.needsImport);
    REQUIRE(on.writes.size() == 2);
    CHECK(on.writes[0].path == "imports/snippets/one.nss");
    CHECK(on.writes[0].text == "item(title='one' cmd='a')\r\n");
    CHECK(on.writes[1].path == "imports/settings-app-snippets.nss");
    CHECK(on.writes[1].text.find("import 'snippets/one.nss'") != std::string::npos);
    CHECK(on.writes[1].text.find("// snippet one sha256=" + hashText(v1)) != std::string::npos);
    CHECK(on.removes.empty());
    CHECK(on.changes == std::vector<std::string>{ "turn on one" });

    config.list = on.writes[1].text;
    config.copies["one"] = on.writes[0].text;
    CHECK(copyState(*c.find("one"), config) == CopyState::Current);
    CHECK(copyState(*c.find("two"), config) == CopyState::Off);

    auto second = planSet(c, config, "two", true);
    REQUIRE(second.writes.size() == 2);
    auto entries = parseList(second.writes[1].text);
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].id == "one");
    CHECK(entries[1].id == "two");
    config.list = second.writes[1].text;
    config.copies["two"] = second.writes[0].text;

    auto off = planSet(c, config, "one", false);
    REQUIRE(off.writes.size() == 1);
    CHECK(off.writes[0].text.find("snippets/one.nss") == std::string::npos);
    CHECK(off.writes[0].text.find("import 'snippets/two.nss'") != std::string::npos);
    CHECK(off.removes == std::vector<std::string>{ "imports/snippets/one.nss" });
    CHECK_FALSE(off.needsImport);

    // Off when it isn't on: only a stray copy goes.
    ConfigFiles stray;
    stray.copies["one"] = v1;
    auto cleanup = planSet(c, stray, "one", false);
    CHECK(cleanup.writes.empty());
    CHECK(cleanup.removes == std::vector<std::string>{ "imports/snippets/one.nss" });
    CHECK(planSet(c, ConfigFiles{}, "one", false).empty());
    CHECK_FALSE(planSet(c, config, "nope", true).error.empty());

    // shell.nss imports the list once, at the end; a commented-out import is turned back on.
    auto root = nss::NssDocument::fromText("import 'imports/theme.nss'\r\n");
    CHECK(ensureImported(root));
    CHECK(root.text() == "import 'imports/theme.nss'\r\nimport 'imports/settings-app-snippets.nss'\r\n");
    CHECK_FALSE(ensureImported(root));
    auto commented = nss::NssDocument::fromText("import 'imports/theme.nss'\r\n// import 'imports/settings-app-snippets.nss'\r\n");
    CHECK(ensureImported(commented));
    CHECK(commented.text().find("\r\nimport 'imports/settings-app-snippets.nss'") != std::string::npos);
    CHECK(commented.text().find("// import 'imports/settings-app-snippets.nss'") == std::string::npos);
}

TEST_CASE("snippets: unedited copies follow the bundled version, edited ones are kept")
{
    const std::string v1 = "item(title='v1' cmd='a')\n", v2 = "item(title='v2' cmd='a')\n";
    std::map<std::string, std::string> newFiles = { { "a", v2 }, { "b", v2 }, { "c", v2 }, { "d", v2 } };
    auto c = catalogOf(newFiles, entry("a", v2) + "," + entry("b", v2) + "," + entry("c", v2) + "," + entry("d", v2));

    ConfigFiles config;
    config.list = generateList({ { "a", hashText(v1) }, { "b", hashText(v1) }, { "c", hashText(v1) }, { "gone", hashText(v1) } });
    config.copies["a"] = toCrlf(v1);                          // unedited, older version
    config.copies["b"] = "\xEF\xBB\xBF" + toCrlf(v1) + "// mine\r\n"; // edited
    config.copies["c"] = std::nullopt;                        // deleted by hand
    config.copies["d"] = toCrlf(v1);                          // not on

    CHECK(copyState(*c.find("a"), config) == CopyState::Outdated);
    CHECK(copyState(*c.find("b"), config) == CopyState::Edited);
    CHECK(copyState(*c.find("c"), config) == CopyState::Missing);
    CHECK(copyState(*c.find("d"), config) == CopyState::Off);

    auto plan = planRefresh(c, config);
    REQUIRE(plan.writes.size() == 3);
    CHECK(plan.writes[0].path == "imports/snippets/a.nss");
    CHECK(plan.writes[0].text == toCrlf(v2));
    CHECK(plan.writes[1].path == "imports/snippets/c.nss");
    CHECK(plan.writes[2].path == std::string(kListPath));
    CHECK(plan.changes == std::vector<std::string>{ "update a", "restore c" });
    auto entries = parseList(plan.writes[2].text);
    REQUIRE(entries.size() == 4);
    CHECK(entries[0].sha256 == hashText(v2));
    CHECK(entries[1].sha256 == hashText(v1)); // the edited copy keeps its record
    CHECK(entries[2].sha256 == hashText(v2));
    CHECK(entries[3].id == "gone");           // no longer in the catalog: left alone

    // Once written, nothing more to do.
    config.list = plan.writes[2].text;
    config.copies["a"] = plan.writes[0].text;
    config.copies["c"] = plan.writes[1].text;
    CHECK(planRefresh(c, config).empty());
    CHECK(planRefresh(c, ConfigFiles{}).empty());

    // Turning an edited snippet on again (it already is) keeps the user's copy.
    auto again = planSet(c, config, "b", true);
    REQUIRE(again.writes.size() == 1);
    CHECK(again.writes[0].path == std::string(kListPath));
}

TEST_CASE("snippets: requirements")
{
    auto none = fakeProbe({}, {});
    auto all = fakeProbe({ L"C:\\PF\\7-Zip\\7z.exe", L"C:\\LAD\\Microsoft\\WindowsApps\\wt.exe", L"C:\\Tools\\foo.exe" },
                         { L"git.exe", L"bar.exe", L"baz.cmd" });

    auto r = checkRequirement("7-zip", none);
    CHECK(r.name == "7-Zip");
    CHECK_FALSE(r.met);
    CHECK(checkRequirement("7-Zip", all).met);
    CHECK(checkRequirement("windows-terminal", all).met);
    CHECK(checkRequirement("windows-terminal", all).name == "Windows Terminal");
    CHECK(checkRequirement("git", all).met); // on PATH
    CHECK_FALSE(checkRequirement("git", none).met);
    CHECK(checkRequirement("vscode", none).name == "Visual Studio Code");

    auto path = checkRequirement("C:\\Tools\\foo.exe", all);
    CHECK(path.met);
    CHECK(path.name == "foo");
    CHECK_FALSE(checkRequirement("C:\\Tools\\other.exe", all).met);
    CHECK(checkRequirement("%ProgramFiles%\\7-Zip\\7z.exe", all).met);

    CHECK(checkRequirement("bar", all).met);     // bar.exe on PATH
    CHECK(checkRequirement("baz.cmd", all).met);
    CHECK(checkRequirement("bar", all).name == "bar");
    CHECK_FALSE(checkRequirement("qux", all).met);

    // The real probe answers without throwing.
    auto real = systemProbe();
    CHECK(checkRequirement("cmd", real).met);
    CHECK(checkRequirement("%SystemRoot%\\System32\\cmd.exe", real).met);
}

TEST_CASE("snippets: minimum versions")
{
    CHECK(versionMet("2.0.0", "2.0.6-beta.2"));
    CHECK(versionMet("2.0.6", "2.0.6-beta.2"));
    CHECK_FALSE(versionMet("2.0.7", "2.0.6"));
    CHECK_FALSE(versionMet("3.0.0", "2.9.9"));
    CHECK(versionMet("", "2.0.6"));
    CHECK(versionMet("garbage", "2.0.6"));
}

namespace {

// Turns every snippet of `catalog` on and off through ConfigStore and shell.exe
// -config:check, alone and then all together, in a copy of the shipped config.
void checkWithShell(const Catalog &catalog, const fs::path &shellExe)
{
    REQUIRE(!catalog.snippets.empty());

    // The shipped config in a temporary folder, as -config:init would make it.
    TempDir tmp;
    fs::copy_file(binDir() / "shell.nss", tmp.path / "shell.nss");
    fs::copy(binDir() / "imports", tmp.path / "imports", fs::copy_options::recursive);
    const auto root = tmp.path / "shell.nss";
    auto check = nss::makeShellCheckValidator(shellExe);

    auto read = [&] {
        ConfigFiles files;
        if (fs::exists(tmp.path / kListPath))
            files.list = readBytes(tmp.path / kListPath);
        for (const auto &s : catalog.snippets) {
            auto p = tmp.path / copyPath(s.id);
            files.copies[s.id] = fs::exists(p) ? std::optional<std::string>(readBytes(p)) : std::nullopt;
        }
        return files;
    };
    // The way the app saves: through ConfigStore, checked on a temporary copy first.
    auto apply = [&](const Plan &plan) {
        std::vector<nss::FileWrite> writes;
        for (const auto &w : plan.writes)
            writes.push_back(nss::FileWrite{ tmp.path / nss::widen(w.path), w.text, std::nullopt });
        if (plan.needsImport) {
            auto doc = nss::NssDocument::load(root);
            REQUIRE(doc);
            if (ensureImported(*doc))
                writes.push_back(nss::FileWrite{ root, doc->text(), doc->format() });
        }
        nss::SaveOptions options;
        options.reason = "test";
        options.validator = check;
        auto result = nss::ConfigStore(root).save(std::move(writes), options);
        for (const auto &r : plan.removes)
            fs::remove(tmp.path / nss::widen(r));
        return result;
    };
    auto clean = [&](const nss::CheckResult &r) {
        auto j = nlohmann::json::parse(r.message, nullptr, false);
        return r.ok && j.is_object() && j["errors"].empty() && j["warnings"].empty();
    };

    // Each one alone, then all of them together.
    for (const auto &s : catalog.snippets) {
        CAPTURE(s.id);
        auto result = apply(planSet(catalog, read(), s.id, true));
        CHECK_MESSAGE(result.ok, result.check.message);
        auto r = check(root);
        CHECK_MESSAGE(clean(r), r.message);
        CHECK(copyState(s, read()) == CopyState::Current);
        CHECK(apply(planSet(catalog, read(), s.id, false)).ok);
        CHECK_FALSE(fs::exists(tmp.path / copyPath(s.id)));
    }
    for (const auto &s : catalog.snippets)
        REQUIRE(apply(planSet(catalog, read(), s.id, true)).ok);
    auto all = check(root);
    CHECK_MESSAGE(clean(all), all.message);
    CHECK(parseList(*read().list).size() == catalog.snippets.size());
    // shell.nss imports the list exactly once.
    auto rootText = readBytes(root);
    CHECK(rootText.find("import 'imports/settings-app-snippets.nss'") != std::string::npos);
    CHECK(rootText.find("settings-app-snippets") == rootText.rfind("settings-app-snippets"));
    CHECK(planRefresh(catalog, read()).empty());
}

} // namespace

TEST_CASE("snippets: every bundled snippet passes shell.exe -config:check")
{
    const auto shellExe = binDir() / "shell.exe";
    if (!fs::exists(shellExe)) {
        MESSAGE("skipped: no shell.exe in src/bin (build src/Shell.sln first)");
        return;
    }
    checkWithShell(BundledSource{}.load(), shellExe);

    TempDir tmp;
    fs::copy_file(binDir() / "shell.nss", tmp.path / "shell.nss");
    fs::copy(binDir() / "imports", tmp.path / "imports", fs::copy_options::recursive);
    const auto root = tmp.path / "shell.nss";
    auto check = nss::makeShellCheckValidator(shellExe);
    auto read = [&] {
        ConfigFiles files;
        if (fs::exists(tmp.path / kListPath))
            files.list = readBytes(tmp.path / kListPath);
        return files;
    };
    auto apply = [&](const Plan &plan) {
        std::vector<nss::FileWrite> writes;
        for (const auto &w : plan.writes)
            writes.push_back(nss::FileWrite{ tmp.path / nss::widen(w.path), w.text, std::nullopt });
        if (plan.needsImport) {
            auto doc = nss::NssDocument::load(root);
            REQUIRE(doc);
            if (ensureImported(*doc))
                writes.push_back(nss::FileWrite{ root, doc->text(), doc->format() });
        }
        nss::SaveOptions options;
        options.reason = "test";
        options.validator = check;
        return nss::ConfigStore(root).save(std::move(writes), options);
    };

    // A snippet Shell rejects is not saved.
    std::map<std::string, std::string> broken = { { "broken", "item(title='x' cmd=\n" } };
    auto bad = catalogOf(broken, entry("broken", broken["broken"]));
    ConfigFiles before = read();
    auto result = apply(planSet(bad, before, "broken", true));
    CHECK_FALSE(result.ok);
    CHECK_FALSE(fs::exists(tmp.path / copyPath("broken")));
    CHECK(read().list == before.list);
}

// --- RubicBG's snippets ------------------------------------------------------------------

namespace {

const std::string kCommit(40, 'a');
const std::string kRepo = "Abdullah-Masood-05/Nilesoft-Shell-Snippets";

std::string remoteCatalog(const std::string &entries, const std::string &commit = kCommit)
{
    return "{\"format\":1,\"repository\":\"" + kRepo + "\",\"commit\":\"" + commit +
           "\",\"author\":\"RubicBG\",\"author_url\":\"https://github.com/RubicBG/Nilesoft-Shell-Snippets\","
           "\"snippets\":[" + entries + "]}";
}

std::string remoteEntry(const std::string &id, const std::string &path, const std::string &text)
{
    return "{\"id\":\"" + id + "\",\"path\":\"" + path + "\",\"title\":\"T " + id +
           "\",\"description\":\"D\",\"category\":\"files\",\"risk\":\"none\",\"sha256\":\"" + hashText(text) + "\"}";
}

updater::HttpResult served(const std::string &body)
{
    updater::HttpResult r;
    r.ok = true;
    r.status = 200;
    r.body = body;
    return r;
}

} // namespace

TEST_CASE("snippets: the RubicBG catalog")
{
    auto rubic = RubicSource{}.load();
    for (const auto &e : rubic.errors)
        MESSAGE(e);
    CHECK(rubic.errors.empty());
    CHECK(rubic.snippets.size() >= 10);

    // The catalog names the commit the code pins.
    std::string json;
    REQUIRE(nss::readFile(fs::path(SETTINGS_SOURCE_DIR) / "snippets" / "rubicbg-catalog.json", json));
    auto j = nlohmann::json::parse(json);
    CHECK(j["commit"].get<std::string>() == kRubicCommit);
    CHECK(j["repository"].get<std::string>() == kRubicRepository);
    CHECK(kRubicCommit.size() == 40);

    auto bundled = BundledSource{}.load();
    for (const auto &s : rubic.snippets) {
        CAPTURE(s.id);
        CHECK(s.id.rfind("rubicbg-", 0) == 0);
        CHECK(bundled.find(s.id) == nullptr);
        CHECK(s.remote());
        CHECK(s.text.empty()); // nothing of it is in the exe
        CHECK(s.author == "RubicBG");
        CHECK(s.authorUrl == kRubicUrl);
        CHECK(s.origin == "rubicbg");
        CHECK(updater::isAllowedSnippetUrl(s.url));
        CHECK(s.url.find(std::string(kRubicCommit)) != std::string::npos);
        CHECK(s.risk != Risk::Admin);
        CHECK(categoryLabel(s.category) != nullptr);
        CHECK(!s.description.empty());
        CHECK(s.description.find("\xE2\x80\x94") == std::string::npos);
        CHECK(s.description.find("\xE2\x80\x93") == std::string::npos);
        for (const char *lang : { "zh-CN", "zh-TW" }) {
            CAPTURE(lang);
            auto t = s.translations.find(lang);
            REQUIRE(t != s.translations.end());
            CHECK(!t->second.title.empty());
            CHECK(!t->second.description.empty());
        }
        for (const auto &n : s.needs)
            CHECK(!checkRequirement(n, fakeProbe({}, {})).name.empty());
    }

    // Every source together: bundled first, no id twice.
    auto all = loadAll();
    CHECK(all.errors.empty());
    CHECK(all.snippets.size() == bundled.snippets.size() + rubic.snippets.size());
    CHECK(all.snippets.front().origin == "bundled");
    CHECK(all.snippets.back().origin == "rubicbg");
}

TEST_CASE("snippets: remote catalog parsing")
{
    const std::string text = "item(title='x' cmd='a')\n";
    auto c = parseRemoteCatalog(remoteCatalog(remoteEntry("rubicbg-a", "ex1/a.b.nss", text) + "," +
                                              remoteEntry("rubicbg-up", "../x.nss", text) + "," +
                                              remoteEntry("rubicbg-abs", "/x.nss", text) + "," +
                                              remoteEntry("rubicbg-bs", "ex1\\\\x.nss", text) + "," +
                                              remoteEntry("rubicbg-q", "x.nss?y", text) + "," +
                                              remoteEntry("rubicbg-txt", "x.txt", text) + "," +
                                              remoteEntry("rubicbg-dup", "ex1/a.b.nss", text) + "," +
                                              "{\"id\":\"rubicbg-nohash\",\"path\":\"n.nss\",\"title\":\"N\"}"),
                                "rubicbg", kRepo, kCommit);
    REQUIRE(c.snippets.size() == 1);
    const auto &s = c.snippets[0];
    CHECK(s.url == "https://raw.githubusercontent.com/" + kRepo + "/" + kCommit + "/ex1/a.b.nss");
    CHECK(s.author == "RubicBG");
    CHECK(s.text.empty());
    CHECK(c.errors.size() == 7);

    // Another commit or repository: refused whole.
    CHECK(parseRemoteCatalog(remoteCatalog(remoteEntry("rubicbg-a", "a.nss", text), std::string(40, 'b')), "r", kRepo,
                             kCommit)
              .snippets.empty());
    CHECK(parseRemoteCatalog(remoteCatalog(remoteEntry("rubicbg-a", "a.nss", text)), "r", "someone/else", kCommit)
              .snippets.empty());

    CHECK(validRemotePath("ex3.multifunction/all.copy.hash.cmd.nss"));
    CHECK(validRemotePath("a.nss"));
    CHECK_FALSE(validRemotePath("a/../b.nss"));
    CHECK_FALSE(validRemotePath("a//b.nss"));
    CHECK_FALSE(validRemotePath("a/./b.nss"));
    CHECK_FALSE(validRemotePath("a b.nss"));
    CHECK_FALSE(validRemotePath("a.nss/"));
    CHECK_FALSE(validRemotePath(".nss"));
}

TEST_CASE("snippets: download addresses")
{
    const std::string base = "https://raw.githubusercontent.com/Abdullah-Masood-05/Nilesoft-Shell-Snippets/";
    CHECK(updater::isAllowedSnippetUrl(base + kCommit + "/ex1/a.nss"));
    CHECK_FALSE(updater::isAllowedSnippetUrl("https://raw.githubusercontent.com:8443/Abdullah-Masood-05/Nilesoft-Shell-Snippets/" +
                                             kCommit + "/a.nss"));
    CHECK(updater::isAllowedSnippetUrl("https://RAW.githubusercontent.com:443/Abdullah-Masood-05/Nilesoft-Shell-Snippets/" +
                                       kCommit + "/a.nss"));
    CHECK_FALSE(updater::isAllowedSnippetUrl(base + "main/ex1/a.nss"));         // a branch
    CHECK_FALSE(updater::isAllowedSnippetUrl(base + kCommit.substr(1) + "/a.nss")); // not a full SHA
    CHECK_FALSE(updater::isAllowedSnippetUrl(base + kCommit + "/"));
    CHECK_FALSE(updater::isAllowedSnippetUrl(base + kCommit + "/a.nss?x=1"));
    CHECK_FALSE(updater::isAllowedSnippetUrl(base + kCommit + "/../a.nss"));
    CHECK_FALSE(updater::isAllowedSnippetUrl("http://raw.githubusercontent.com/Abdullah-Masood-05/Nilesoft-Shell-Snippets/" +
                                             kCommit + "/a.nss"));
    CHECK_FALSE(updater::isAllowedSnippetUrl("https://raw.githubusercontent.com/RubicBG/Nilesoft-Shell-Snippets/" + kCommit +
                                             "/a.nss"));
    CHECK_FALSE(updater::isAllowedSnippetUrl("https://evil.com/Abdullah-Masood-05/Nilesoft-Shell-Snippets/" + kCommit + "/a.nss"));
    CHECK_FALSE(updater::isAllowedSnippetUrl("https://x@raw.githubusercontent.com/Abdullah-Masood-05/Nilesoft-Shell-Snippets/" +
                                             kCommit + "/a.nss"));
    // The release allow-list doesn't take snippet addresses, nor the other way round.
    CHECK_FALSE(updater::isAllowedDownloadUrl(base + kCommit + "/a.nss"));
    CHECK_FALSE(updater::isAllowedSnippetUrl("https://github.com/Abdullah-Masood-05/Shell/releases/download/v2.1.0/setup-x64.msi"));
}

TEST_CASE("snippets: a remote snippet is downloaded, checked, then written")
{
    const std::string text = "item(title='remote' cmd='a')\n";
    auto catalog = parseRemoteCatalog(remoteCatalog(remoteEntry("rubicbg-a", "ex1/a.nss", text)), "rubicbg", kRepo, kCommit);
    REQUIRE(catalog.snippets.size() == 1);
    const auto &s = catalog.snippets[0];

    // Not downloaded: nothing can be planned.
    auto plan = planSet(catalog, {}, "rubicbg-a", true);
    CHECK_FALSE(plan.error.empty());
    CHECK(plan.writes.empty());

    std::string asked;
    auto ok = fetchText(s, [&](const std::string &url) {
        asked = url;
        return served("\xEF\xBB\xBFitem(title='remote' cmd='a')\r\n"); // a BOM and CRLF are fine
    });
    CHECK(ok.ok);
    CHECK(ok.text == text);
    CHECK(asked == s.url);

    // A different file, a failed request, no connection: an error and no text.
    auto wrong = fetchText(s, [](const std::string &) { return served("item(title='evil' cmd='b')\n"); });
    CHECK_FALSE(wrong.ok);
    CHECK(wrong.text.empty());
    CHECK_FALSE(wrong.error.empty());
    auto notFound = fetchText(s, [](const std::string &) {
        updater::HttpResult r;
        r.status = 404;
        return r;
    });
    CHECK_FALSE(notFound.ok);
    CHECK(notFound.error.find("404") != std::string::npos);
    auto offline = fetchText(s, [](const std::string &) {
        updater::HttpResult r;
        r.offline = true;
        return r;
    });
    CHECK_FALSE(offline.ok);
    CHECK_FALSE(offline.error.empty());

    // A snippet whose address isn't allowed is never requested.
    auto other = s;
    other.url = "https://example.com/a.nss";
    bool called = false;
    CHECK_FALSE(fetchText(other, [&](const std::string &) {
                    called = true;
                    return served(text);
                }).ok);
    CHECK_FALSE(called);

    // Downloaded: planned like a bundled one.
    auto withText = catalog;
    withText.snippets[0].text = ok.text;
    plan = planSet(withText, {}, "rubicbg-a", true);
    CHECK(plan.error.empty());
    REQUIRE(plan.writes.size() == 2);
    CHECK(plan.writes[0].path == "imports/snippets/rubicbg-a.nss");
    CHECK(plan.writes[0].text == toCrlf(text));

    // The refresh leaves a missing copy alone until the text is there.
    ConfigFiles files;
    files.list = generateList({ { "rubicbg-a", s.sha256 } });
    files.copies["rubicbg-a"] = std::nullopt;
    CHECK(copyState(s, files) == CopyState::Missing);
    CHECK(planRefresh(catalog, files).empty());
    CHECK_FALSE(planRefresh(withText, files).empty());
}

TEST_CASE("snippets: every RubicBG snippet passes shell.exe -config:check")
{
    const auto shellExe = binDir() / "shell.exe";
    wchar_t dir[MAX_PATH]{};
    if (!fs::exists(shellExe) || GetEnvironmentVariableW(L"NSP_RUBICBG_DIR", dir, MAX_PATH) == 0) {
        MESSAGE("skipped: needs shell.exe in src/bin and NSP_RUBICBG_DIR (a checkout of the pinned commit)");
        return;
    }
    auto catalog = RubicSource{}.load();
    std::string json;
    REQUIRE(nss::readFile(fs::path(SETTINGS_SOURCE_DIR) / "snippets" / "rubicbg-catalog.json", json));
    auto j = nlohmann::json::parse(json);
    for (const auto &e : j["snippets"]) {
        auto id = e["id"].get<std::string>();
        auto bytes = readBytes(fs::path(dir) / nss::widen(e["path"].get<std::string>()));
        CAPTURE(id);
        for (auto &s : catalog.snippets)
            if (s.id == id) {
                REQUIRE(hashText(bytes) == s.sha256);
                s.text = normalize(bytes);
            }
    }
    checkWithShell(catalog, shellExe);
}

TEST_CASE("snippets: hand edit survives an app save")
{
    const std::string v1 = "item(title='one' cmd='a')\n";
    std::map<std::string, std::string> files = { { "one", v1 }, { "two", v1 }, { "three", v1 } };
    auto c = catalogOf(files, entry("one", v1) + "," + entry("two", v1) + "," + entry("three", v1));

    // The page turned on "one" and "two".
    ConfigFiles config;
    auto a = planSet(c, config, "one", true);
    config.list = a.writes[1].text;
    config.copies["one"] = a.writes[0].text;
    auto b = planSet(c, config, "two", true);
    config.list = b.writes[1].text;
    config.copies["two"] = b.writes[0].text;

    // The user edits the list in the Config Editor: a comment, an import of a
    // file of their own, and "one" turned off by commenting out its import.
    auto text = *config.list;
    auto at = text.find("import 'snippets/one.nss'");
    REQUIRE(at != std::string::npos);
    text.replace(at, 0, "// ");
    text += "\r\n// my own menu\r\nimport 'mine/tools.nss'\r\n";
    config.list = text;
    REQUIRE(parseList(text).size() == 1); // "one" is off now: read back

    // The page turns on "three".
    auto plan = planSet(c, config, "three", true);
    REQUIRE(plan.error.empty());
    REQUIRE(plan.writes.size() == 2);
    const auto &list = plan.writes[1].text;
    CHECK(list.rfind(std::string(kListHeader), 0) == 0);
    CHECK(list.find("// import 'snippets/one.nss'") != std::string::npos);
    CHECK(list.find("\r\n// my own menu\r\nimport 'mine/tools.nss'\r\n") != std::string::npos);
    CHECK(list.find("// snippet three sha256=" + hashText(v1) + "\r\nimport 'snippets/three.nss'\r\n") != std::string::npos);
    auto entries = parseList(list);
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].id == "two");
    CHECK(entries[1].id == "three");
    CHECK(nss::NssDocument::fromText(list).diagnostics().empty());

    // Turning "two" off removes its lines only.
    config.list = list;
    config.copies["three"] = plan.writes[0].text;
    auto off = planSet(c, config, "two", false);
    REQUIRE(off.writes.size() == 1);
    CHECK(off.writes[0].text.find("snippets/two.nss") == std::string::npos);
    CHECK(off.writes[0].text.find("snippet two ") == std::string::npos);
    CHECK(off.writes[0].text.find("\r\n// my own menu\r\nimport 'mine/tools.nss'\r\n") != std::string::npos);
    CHECK(off.writes[0].text.find("// import 'snippets/one.nss'") != std::string::npos);

    // An older list's header is replaced with today's.
    auto old = updateList(std::string("// generated by Nilesoft Shell Pro, do not edit by hand\r\n// mine\r\n"), {});
    CHECK(old == std::string(kListHeader) + "\r\n// mine\r\n");
}

TEST_CASE("snippets: a hand edit that doesn't parse is left alone")
{
    const std::string v1 = "item(title='one' cmd='a')\n";
    auto c = catalogOf({ { "one", v1 } }, entry("one", v1));
    ConfigFiles config;
    config.list = std::string(kListHeader) + "\r\nimport 'snippets/one.nss'\r\nitem(title='x'\r\n";
    config.copies["one"] = toCrlf(v1);
    auto plan = planSet(c, config, "one", false);
    CHECK(plan.empty());
    CHECK(plan.error.find("settings-app-snippets.nss") != std::string::npos);
    CHECK(plan.error.find("Config Editor") != std::string::npos);
    CHECK(planRefresh(c, config).empty());
}
