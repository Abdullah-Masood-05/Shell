#include "doctest.h"

#include "PendingChanges.h"

#include "nss/NssDocument.h"
#include "nss/Text.h"

#include <windows.h>

#include <atomic>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {

struct TempDir
{
    fs::path path;
    TempDir()
    {
        static std::atomic<int> n{ 0 };
        path = fs::temp_directory_path() /
               (L"settings-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++n));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void writeFile(const fs::path &p, std::string_view bytes)
{
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), std::streamsize(bytes.size()));
}

std::string readText(const fs::path &p)
{
    std::string bytes;
    nss::readFile(p, bytes);
    return nss::decode(bytes).text;
}

// What ConfigStore would write, without the transaction around it.
void commit(const SavePlan &plan)
{
    for (const auto &w : plan.writes)
        writeFile(w.path, nss::encode(w.text, w.format.value_or(nss::defaultFormat())));
}

} // namespace

TEST_CASE("pending: validation and canonical keys")
{
    PendingChanges p;
    CHECK_FALSE(p.set("theme.background.opacity", "150", "Appearance").empty());
    CHECK_FALSE(p.set("theme.item.radius", "2.5").empty());
    CHECK_FALSE(p.set("theme.background.color", "if(theme.isdark, #000, #fff)").empty()); // expressions: no
    CHECK_FALSE(p.set("theme.nonsense", "1").empty());
    CHECK(p.empty());

    CHECK(p.set("theme.background.opacity", "50", "Appearance").empty());
    CHECK(p.set("theme.background", "#20232A", "Appearance").empty()); // alias
    CHECK(p.set("theme.view", "view.compact", "Appearance").empty());
    CHECK(p.set("theme.font.name", "\"Segoe UI Variable\"", "Appearance").empty());
    CHECK(p.count() == 4);
    REQUIRE(p.find("theme.background.color"));
    CHECK(p.find("theme.background.color")->literal == "#20232A");

    p.set("theme.background.opacity", "60", "Appearance"); // the later edit replaces the earlier one
    CHECK(p.count() == 4);
    CHECK(p.find("theme.background.opacity")->literal == "60");

    p.forget("theme.view");
    CHECK(p.count() == 3);
    p.discard();
    CHECK(p.empty());
}

TEST_CASE("pending: mapping onto the existing overrides")
{
    nss::Overrides existing{
        { "theme.border.size", "2" },
        { "theme.item.radius", "1" },
        { "theme.background", "#000000" }, // an alias in an old file
    };
    PendingChanges p;
    REQUIRE(p.set("theme.background.opacity", "50", "Appearance").empty());
    REQUIRE(p.set("theme.background.color", "#20232A", "Appearance").empty());
    p.reset("theme.border.size", "Appearance");
    p.reset("theme.shadow.size", "Appearance"); // not there: nothing to remove

    auto out = p.apply(existing);
    nss::Overrides expected{
        { "theme.item.radius", "1" },
        { "theme.background.color", "#20232A" },
        { "theme.background.opacity", "50" },
    };
    CHECK(out == expected);
    CHECK(p.describe() == "Appearance: background.color, background.opacity, border.size (reset), shadow.size (reset)");

    PendingChanges many;
    for (auto key : { "theme.border.size", "theme.shadow.size", "theme.image.gap", "theme.image.size", "theme.separator.size" })
        REQUIRE(many.set(key, "3", "Appearance").empty());
    CHECK(many.describe() == "Appearance: border.size, image.gap, image.size, separator.size and 1 more");
}

TEST_CASE("pending: planSave writes settings-app.nss and imports it last")
{
    TempDir dir;
    const auto root = dir.path / "shell.nss";
    writeFile(root, "\xEF\xBB\xBF" "settings { priority = 1 }\r\nimport 'imports/theme.nss'\r\n");
    writeFile(dir.path / "imports" / "theme.nss", "theme { name = \"modern\" }\r\n");

    PendingChanges p;
    REQUIRE(p.set("theme.background.color", "#20232A", "Appearance").empty());
    REQUIRE(p.set("theme.item.radius", "3", "Appearance").empty());

    auto plan = planSave(root, p);
    REQUIRE(plan.ok);
    CHECK(plan.before.empty());
    CHECK(plan.rootChanged);
    REQUIRE(plan.writes.size() == 2);
    CHECK(plan.writes[0].path == plan.overridesPath); // imports first, the root last
    CHECK(plan.writes[1].path == root);
    CHECK(plan.overridesPath == (dir.path / "imports" / "settings-app.nss").lexically_normal());
    CHECK(plan.writes[0].text ==
          "// Nilesoft Shell Pro keeps this file. You can edit it; the app keeps your changes.\r\n\r\ntheme\r\n{\r\n"
          "\tbackground.color = #20232A\r\n\titem.radius = 3\r\n}\r\n");
    CHECK(plan.writes[1].text.find("import 'imports/settings-app.nss'") != std::string::npos);
    CHECK(plan.writes[1].format->bom); // the root keeps its BOM
    commit(plan);

    // The same changes again: nothing to write.
    auto again = planSave(root, p);
    REQUIRE(again.ok);
    CHECK(again.writes.empty());
    CHECK_FALSE(again.rootChanged);
    CHECK(again.before.size() == 2);

    // A reset removes the key from the generated file; the root is left alone.
    PendingChanges reset;
    reset.reset("theme.item.radius", "Appearance");
    auto plan3 = planSave(root, reset);
    REQUIRE(plan3.ok);
    REQUIRE(plan3.writes.size() == 1);
    CHECK(plan3.writes[0].text.find("item.radius") == std::string::npos);
    CHECK(plan3.writes[0].text.find("background.color = #20232A") != std::string::npos);
    commit(plan3);
    CHECK(readText(root).find("import 'imports/settings-app.nss'") != std::string::npos);
    auto doc = nss::NssDocument::load(dir.path / "imports" / "settings-app.nss");
    REQUIRE(doc);
    CHECK(doc->diagnostics().empty());

    // A missing root is an error, not a crash.
    auto missing = planSave(dir.path / "nope.nss", p);
    CHECK_FALSE(missing.ok);
    CHECK_FALSE(missing.error.empty());
}

TEST_CASE("settings-app.nss: hand edit survives an app save")
{
    TempDir dir;
    const auto root = dir.path / "shell.nss";
    writeFile(root, "settings { priority = 1 }\r\n");

    // The app writes the file.
    PendingChanges first;
    REQUIRE(first.set("theme.background.color", "#20232A", "Appearance").empty());
    REQUIRE(first.set("theme.item.radius", "3", "Appearance").empty());
    auto plan = planSave(root, first);
    REQUIRE(plan.ok);
    commit(plan);
    const auto file = plan.overridesPath;

    // The user edits it in the Config Editor: a comment, a statement the page
    // doesn't know, and a value the page knows (item.radius 3 -> 6).
    auto text = readText(file);
    REQUIRE(text.find("\titem.radius = 3\r\n") != std::string::npos);
    text.replace(text.find("\titem.radius = 3"), 16, "\t// rounder corners\r\n\titem.radius = 6");
    text += "\r\n$my_accent = #FF8800\r\nsettings { tip.enabled = false }\r\n";
    writeFile(file, text);

    // Then a page saves another value.
    PendingChanges second;
    REQUIRE(second.set("theme.background.opacity", "80", "Appearance").empty());
    auto plan2 = planSave(root, second);
    REQUIRE_MESSAGE(plan2.ok, plan2.error);
    // The hand-changed value is read back, not overwritten with an old one.
    CHECK(plan2.before.at("theme.item.radius") == "6");
    CHECK(plan2.after.at("theme.item.radius") == "6");
    CHECK(plan2.before.at("settings.tip.enabled") == "false");
    REQUIRE(plan2.writes.size() == 1);
    const auto &out = plan2.writes[0].text;
    CHECK(out.rfind(std::string(nss::OverridesWriter::header), 0) == 0);
    CHECK(out.find("\t// rounder corners\r\n\titem.radius = 6\r\n") != std::string::npos);
    CHECK(out.find("\r\n$my_accent = #FF8800\r\n") != std::string::npos);
    CHECK(out.find("settings { tip.enabled = false }") != std::string::npos);
    CHECK(out.find("background.color = #20232A") != std::string::npos);
    CHECK(out.find("\tbackground.opacity = 80\r\n}") != std::string::npos); // at the end of theme{}
    auto doc = nss::NssDocument::fromText(out);
    CHECK(doc.diagnostics().empty());
    commit(plan2);

    // Resetting a value removes only its line.
    PendingChanges third;
    third.reset("theme.background.color", "Appearance");
    auto plan3 = planSave(root, third);
    REQUIRE(plan3.ok);
    REQUIRE(plan3.writes.size() == 1);
    auto expected = out;
    expected.erase(expected.find("\tbackground.color = #20232A\r\n"), 29);
    CHECK(plan3.writes[0].text == expected);

    // A file from an older version gets today's header; nothing else changes.
    writeFile(file, "// generated by Nilesoft Shell Pro, do not edit\r\n// mine\r\ntheme\r\n{\r\n\titem.radius = 6\r\n}\r\n");
    PendingChanges fourth;
    REQUIRE(fourth.set("theme.item.radius", "2", "Appearance").empty());
    auto plan4 = planSave(root, fourth);
    REQUIRE(plan4.ok);
    REQUIRE(plan4.writes.size() == 1);
    CHECK(plan4.writes[0].text == std::string(nss::OverridesWriter::header) + "\r\n// mine\r\ntheme\r\n{\r\n\titem.radius = 2\r\n}\r\n");
}

TEST_CASE("settings-app.nss: a hand edit that doesn't parse is left alone")
{
    TempDir dir;
    const auto root = dir.path / "shell.nss";
    writeFile(root, "settings { priority = 1 }\r\nimport 'imports/settings-app.nss'\r\n");
    const auto file = dir.path / "imports" / "settings-app.nss";
    const std::string broken = "theme\r\n{\r\n\titem.radius = 6\r\n\tbackground.color = (\r\n";
    writeFile(file, broken);
    REQUIRE_FALSE(nss::NssDocument::fromText(broken).diagnostics().empty());

    PendingChanges p;
    REQUIRE(p.set("theme.item.radius", "3", "Appearance").empty());
    auto plan = planSave(root, p);
    CHECK_FALSE(plan.ok);
    CHECK(plan.writes.empty());
    CHECK(plan.error.find("settings-app.nss") != std::string::npos);
    CHECK(plan.error.find("Config Editor") != std::string::npos);
    CHECK(readText(file) == broken);
}
