// Theme presets (src/ThemePresets.cpp): every embedded preset parses with nsscore
// and passes ThemeSchema; Apply stages the right pending changes; the generated
// imports/settings-app.nss holds the preset's keys; and the real
// `shell.exe -config:check` accepts each preset applied to a seeded config
// (skipped when src/bin has no shell.exe).

#include "doctest.h"

#include "PendingChanges.h"
#include "ThemePresets.h"

#include "nss/ConfigGraph.h"
#include "nss/ConfigStore.h"
#include "nss/Literal.h"
#include "nss/NssDocument.h"
#include "nss/OverridesWriter.h"
#include "nss/Text.h"
#include "nss/ThemeSchema.h"

#include <windows.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

namespace fs = std::filesystem;

namespace {

fs::path binDir() { return fs::path(SETTINGS_SOURCE_BIN).lexically_normal(); }
fs::path shellExe() { return binDir() / L"shell.exe"; }

fs::path uniqueTemp(const wchar_t *prefix)
{
    static std::atomic<int> n{ 0 };
    auto p = fs::temp_directory_path() /
             (std::wstring(prefix) + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++n));
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

struct TempDir
{
    fs::path path = uniqueTemp(L"settings-presets-");
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

struct TempAppData
{
    fs::path path = uniqueTemp(L"settings-presets-appdata-");
    std::wstring previous;
    TempAppData()
    {
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

void writeFile(const fs::path &p, std::string_view bytes)
{
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), std::streamsize(bytes.size()));
}

// What settings-app.nss holds after a save.
nss::Overrides overridesIn(const fs::path &root)
{
    auto doc = nss::NssDocument::load(root.parent_path() / L"imports" / L"settings-app.nss");
    return doc ? nss::OverridesWriter::read(*doc) : nss::Overrides{};
}

// The saved value of `key` as SaveService::set() sees it: this app's, else the files'.
struct GraphView
{
    nss::ConfigGraph graph;
    fs::path overrides;

    std::optional<std::string> saved(const std::string &key) const
    {
        const auto *ev = graph.find(key);
        if (!ev)
            return std::nullopt;
        return ev->effective().value;
    }
    std::vector<std::string> appKeys() const
    {
        std::vector<std::string> out;
        for (const auto &[key, ev] : graph.values())
            for (const auto &o : ev.chain)
                if (nss::pathKey(o.file) == nss::pathKey(overrides))
                    out.push_back(key);
        return out;
    }
};

GraphView view(const fs::path &root, const fs::path &appDir = {})
{
    nss::GraphOptions options;
    options.appDir = appDir;
    return { nss::ConfigGraph::load(root, options),
             (root.parent_path() / L"imports" / L"settings-app.nss").lexically_normal() };
}

} // namespace

TEST_CASE("presets: every embedded preset parses with nsscore and passes ThemeSchema")
{
    const auto &list = presets::embedded();
    REQUIRE(list.size() >= 3);
    std::set<std::string> names;
    for (const auto &p : list) {
        INFO("preset ", p.id);
        for (const auto &e : p.errors)
            MESSAGE(p.id, ": ", e);
        CHECK(p.errors.empty());
        CHECK_FALSE(p.name.empty());
        CHECK_FALSE(p.about.empty());
        CHECK_FALSE(p.source.empty());
        CHECK_FALSE(p.tags.empty());
        CHECK(p.tags.size() <= 3);
        CHECK(names.insert(p.name).second); // names are unique

        // The block is real nss: the lossless document reproduces it and has no diagnostics.
        auto doc = nss::NssDocument::fromText(p.text);
        CHECK(doc.diagnostics().empty());
        CHECK(doc.text() == p.text);
        for (const auto &[key, literal] : p.values) {
            INFO(key, " = ", literal);
            CHECK(nss::ThemeSchema::isKnown(key));
            CHECK(nss::Literal::parse(literal).isLiteral());
            CHECK(nss::ThemeSchema::validate(key, literal).empty());
        }
    }
    // Nothing was dropped at run time: the gallery shows every embedded preset.
    CHECK(presets::all().size() == list.size());
}

TEST_CASE("presets: parse rejects what doesn't belong in a preset")
{
    auto ok = presets::parse("x", "// Preset: X\n// Tags: One, Two \n// About: a\n// Source: b\ntheme\n{\n\tname = \"modern\"\n\tbackground { color = #101010 }\n}\n");
    CHECK(ok.errors.empty());
    CHECK(ok.name == "X");
    CHECK(ok.tags == std::vector<std::string>{ "One", "Two" });
    CHECK(ok.about == "a");
    CHECK(ok.source == "b");
    CHECK(ok.values.at("theme.background.color") == "#101010");

    CHECK_FALSE(presets::parse("x", "// Preset: X\ntheme { nonsense = 1 }\n").errors.empty());       // unknown key
    CHECK_FALSE(presets::parse("x", "// Preset: X\ntheme { item.radius = 9 }\n").errors.empty());  // out of range
    CHECK_FALSE(presets::parse("x", "// Preset: X\ntheme { name = sys.is11 ? 1 : 2 }\n").errors.empty()); // expression
    CHECK_FALSE(presets::parse("x", "// Preset: X\ntheme { name = \"modern\" }\nitem(title='x')\n").errors.empty()); // a menu
    CHECK_FALSE(presets::parse("x", "theme { name = \"modern\" }\n").errors.empty());                // no name line
    CHECK_FALSE(presets::parse("x", "// Preset: X\n// Tags: a, b, c, d\ntheme { name = \"modern\" }\n").errors.empty()); // four tags
}

TEST_CASE("presets: apply stages the preset and replaces the page's theme changes")
{
    const auto preset = presets::parse(
        "t", "// Preset: T\ntheme\n{\n\tname = \"modern\"\n\tdark = true\n\tbackground.color = #202020\n}\n");
    REQUIRE(preset.errors.empty());

    PendingChanges pending;
    REQUIRE(pending.set("theme.font.size", "20", "Appearance").empty());     // a pending theme change
    REQUIRE(pending.set("settings.showdelay", "150", "Behavior").empty());   // another page's change
    const std::vector<std::string> appKeys = { "theme.border.radius", "theme.dark", "settings.tip.enabled" };
    auto saved = [](const std::string &key) -> std::optional<std::string> {
        if (key == "theme.dark")
            return "true";           // already this value: nothing to stage
        if (key == "theme.name")
            return "\"classic\"";
        return std::nullopt;
    };
    REQUIRE(presets::stage(pending, preset, appKeys, saved).empty());

    const auto *name = pending.find("theme.name");
    REQUIRE(name);
    CHECK(name->literal == "\"modern\"");
    CHECK(pending.find("theme.background.color")->literal == "#202020");
    CHECK(pending.find("theme.dark") == nullptr);                 // the saved value already
    CHECK(pending.find("theme.font.size") == nullptr);            // forgotten
    REQUIRE(pending.find("theme.border.radius"));
    CHECK_FALSE(pending.find("theme.border.radius")->literal);    // this app's override reset
    CHECK(pending.find("settings.showdelay")->literal == "150");  // other pages untouched
    CHECK(pending.find("settings.tip.enabled") == nullptr);

    // A bad value changes nothing.
    presets::Preset bad = preset;
    bad.values["theme.item.radius"] = "9";
    auto before = pending.items().size();
    CHECK_FALSE(presets::stage(pending, bad, appKeys, saved).empty());
    CHECK(pending.items().size() == before);

    // inUse: every preset key has its value.
    auto current = [&](const std::string &key) -> std::optional<std::string> {
        if (const auto *p = pending.find(key); p && p->literal)
            return p->literal;
        return saved(key);
    };
    CHECK(presets::inUse(preset, current));
    CHECK_FALSE(presets::inUse(presets::all().front(), [](const std::string &) { return std::nullopt; }));
}

TEST_CASE("presets: apply -> pending -> the generated settings-app.nss holds the preset's keys")
{
    TempDir dir;
    const auto root = dir.path / L"shell.nss";
    // An earlier save from this app.
    writeFile(dir.path / L"imports" / L"settings-app.nss",
              "// generated by Nilesoft Shell Settings, do not edit\n\ntheme\n{\n\titem.radius = 4\n}\n\nsettings\n{\n\tshowdelay = 150\n}\n");
    writeFile(root, "theme\n{\n\tname = \"classic\"\n\tfont.size = 12\n}\nimport 'imports/settings-app.nss'\n");

    for (const auto &preset : presets::all()) {
        INFO("preset ", preset.id);
        auto v = view(root);
        PendingChanges pending;
        REQUIRE(presets::stage(pending, preset, v.appKeys(), [&](const std::string &k) { return v.saved(k); }).empty());
        auto plan = planSave(root, pending);
        REQUIRE_MESSAGE(plan.ok, plan.error);
        // Every preset key, nothing else from the theme, and the settings key kept.
        for (const auto &[key, literal] : preset.values) {
            INFO(key);
            REQUIRE(plan.after.count(key) == 1);
            CHECK(sameValue(plan.after.at(key), literal));
        }
        for (const auto &[key, literal] : plan.after)
            if (presets::isThemeKey(key))
                CHECK_MESSAGE(preset.values.count(key) == 1, key);
        CHECK(plan.after.at("settings.showdelay") == "150");
        // The text that would be written parses back to the same overrides.
        REQUIRE_FALSE(plan.writes.empty());
        auto generated = nss::NssDocument::fromText(plan.writes.front().text);
        CHECK(generated.diagnostics().empty());
        CHECK(nss::OverridesWriter::read(generated) == plan.after);
    }
}

TEST_CASE("presets: exportBlock round-trips through parse")
{
    nss::Overrides values = { { "theme.name", "\"modern\"" },
                              { "theme.background.color", "#1E1E1E" },
                              { "theme.item.radius", "3" },
                              { "settings.showdelay", "150" } }; // not a theme key: left out
    auto block = presets::exportBlock(values, "\n");
    CHECK(block.rfind("theme\n{\n", 0) == 0);
    CHECK(block.find("showdelay") == std::string::npos);
    CHECK(block.find("\tname = \"modern\"\n") < block.find("\tbackground.color = #1E1E1E\n"));
    auto back = presets::parse("e", "// Preset: E\n" + block);
    CHECK(back.errors.empty());
    values.erase("settings.showdelay");
    CHECK(back.values == values);
}

TEST_CASE("presets: each preset saved over a seeded config passes shell.exe -config:check (real shell.exe)")
{
    if (!fs::is_regular_file(shellExe()) || !fs::is_regular_file(binDir() / L"shell.dll")) {
        MESSAGE("shell.exe/shell.dll not in src/bin; skipped");
        return;
    }
    TempAppData appData;
    REQUIRE(runShell(L"-silent -config:init"));
    const auto root = appData.path / L"Nilesoft" / L"Shell" / L"shell.nss";
    REQUIRE(fs::is_regular_file(root));

    // Applied one after another, like clicking through the gallery and saving each.
    for (const auto &preset : presets::all()) {
        INFO("preset ", preset.id);
        auto v = view(root, binDir());
        PendingChanges pending;
        REQUIRE(presets::stage(pending, preset, v.appKeys(), [&](const std::string &k) { return v.saved(k); }).empty());
        if (pending.empty())
            continue; // already in effect
        auto plan = planSave(root, pending);
        REQUIRE_MESSAGE(plan.ok, plan.error);
        nss::SaveOptions options;
        options.reason = pending.describe();
        options.validator = nss::makeShellCheckValidator(shellExe());
        options.verify = options.validator;
        nss::ConfigStore store(root);
        auto saved = store.save(plan.writes, options);
        REQUIRE_MESSAGE(saved.ok, (saved.error + " " + saved.check.message));
        CHECK_FALSE(saved.rolledBack);
        CHECK(nss::makeShellCheckValidator(shellExe())(root).ok);

        // The graph reads the preset back, from settings-app.nss, and only its theme keys are there.
        auto after = view(root, binDir());
        for (const auto &[key, literal] : preset.values) {
            INFO(key);
            const auto *ev = after.graph.find(key);
            REQUIRE(ev);
            // From settings-app.nss, unless the user's files already had this value.
            CHECK(sameValue(ev->effective().value, literal));
            if (nss::pathKey(ev->effective().file) != nss::pathKey(after.overrides))
                CHECK(pending.find(key) == nullptr);
        }
        for (const auto &[key, value] : overridesIn(root))
            if (presets::isThemeKey(key))
                CHECK_MESSAGE(preset.values.count(key) == 1, key);
    }
}

TEST_CASE("presets: the active card and what Apply would replace")
{
    auto a = presets::parse("a", "// Preset: A\ntheme\n{\n\titem.radius = 0\n}\n");
    auto b = presets::parse("b", "// Preset: B\ntheme\n{\n\titem.radius = 0\n\tshadow.enabled = false\n}\n");
    REQUIRE(a.errors.empty());
    REQUIRE(b.errors.empty());
    const std::vector<presets::Preset> list = { a, b };

    std::map<std::string, std::string> now;
    auto current = [&](const std::string &key) -> std::optional<std::string> {
        auto it = now.find(key);
        return it == now.end() ? std::nullopt : std::optional<std::string>(it->second);
    };
    CHECK(presets::activeIndex(list, current) == -1);
    now["theme.item.radius"] = "0";
    CHECK(presets::activeIndex(list, current) == 0);
    now["theme.shadow.enabled"] = "false";
    CHECK(presets::activeIndex(list, current) == 1); // both in use: the more specific one
    auto c = presets::parse("c", "// Preset: C\ntheme\n{\n\tshadow.enabled = false\n}\n");
    REQUIRE(c.errors.empty());
    const std::vector<presets::Preset> list2 = { b, c };
    CHECK(presets::activeIndex(list2, current) == 0);
    CHECK(presets::activeIndex(list2, current, { { "theme.shadow.enabled", "false" } }) == 0); // a tie on own keys: more keys
    now.erase("theme.item.radius");
    CHECK(presets::activeIndex(list2, current, { { "theme.shadow.enabled", "false" } }) == 1);
    now["theme.item.radius"] = "0";
    // The one applied here wins over a bigger one the user's files happen to match.
    CHECK(presets::activeIndex(list2, current) == 0);
    CHECK(presets::activeIndex(list2, current, { { "theme.shadow.enabled", "false" }, { "theme.font.size", "9" } }) == 0);
    auto d = presets::parse("d", "// Preset: D\ntheme\n{\n\tborder.enabled = false\n}\n");
    REQUIRE(d.errors.empty());
    now["theme.border.enabled"] = "false";
    const std::vector<presets::Preset> list3 = { b, d };
    CHECK(presets::activeIndex(list3, current) == 0);
    CHECK(presets::activeIndex(list3, current, { { "theme.border.enabled", "false" } }) == 1);

    // This app's overrides: those of the active preset don't count, anything else does.
    nss::Overrides overrides = { { "theme.item.radius", "0" }, { "theme.shadow.enabled", "false" } };
    CHECK(presets::replacedCount(overrides, &list[1]) == 0);
    CHECK(presets::replacedCount(overrides, &list[0]) == 1);
    CHECK(presets::replacedCount(overrides, nullptr) == 2);
    overrides["theme.font.size"] = "16";
    overrides["settings.showdelay"] = "100"; // not a theme key
    CHECK(presets::replacedCount(overrides, &list[1]) == 1);
}

TEST_CASE("presets: export copies the file with its credit, CRLF")
{
    for (const auto &preset : presets::all()) {
        INFO("preset ", preset.id);
        auto text = presets::exportText(preset);
        CHECK(text.find("// Source: " + preset.source + "\r\n") != std::string::npos);
        CHECK(text.find("theme\r\n") != std::string::npos);
        CHECK(text.find('\n') == text.find("\r\n") + 1);
        // Pasted into a .nss file it is the same preset again.
        auto back = presets::parse(preset.id, text);
        CHECK(back.errors.empty());
        CHECK(back.values == preset.values);
    }
}
