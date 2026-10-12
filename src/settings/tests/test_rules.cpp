// ExcludeRules: how the Rules & Filters page reads and writes settings.exclude{}.

#include "doctest.h"

#include "ExcludeRules.h"
#include "PendingChanges.h"

using namespace rules;
using Names = std::vector<std::string>;

TEST_CASE("rules: name lists")
{
    CHECK(parseNames("'notepad'") == Names{ "notepad" });
    CHECK(parseNames("\"notepad\"") == Names{ "notepad" });
    CHECK(parseNames("['notepad', \"code\", 'NOTEPAD']") == Names{ "notepad", "code" }); // duplicates dropped
    CHECK(parseNames("\"\"") == Names{});
    CHECK(parseNames("[]") == Names{});
    CHECK_FALSE(parseNames("process.name").has_value());        // an expression
    CHECK_FALSE(parseNames("['a', 1]").has_value());            // not all strings
    CHECK_FALSE(parseNames("if(sys.is11, 'a', 'b')").has_value());

    CHECK(formatNames({}) == "\"\"");
    CHECK(formatNames({ "notepad" }) == "\"notepad\"");
    CHECK(formatNames({ "notepad", "code" }) == "[\"notepad\", \"code\"]");
    for (const auto &list : { Names{}, Names{ "a" }, Names{ "a", "b c", "d\\e" } })
        CHECK(parseNames(formatNames(list)) == list);
}

TEST_CASE("rules: process and window names")
{
    CHECK(processName("notepad") == "notepad");
    CHECK(processName("  Notepad.EXE ") == "Notepad");
    CHECK(processName("C:\\Windows\\System32\\notepad.exe") == "notepad");
    CHECK(processName("\"C:\\Program Files\\App\\app.exe\"") == "app");
    CHECK(processName("D:/tools/my.tool.exe") == "my.tool");
    CHECK(processName("   ").empty());
    CHECK(windowClass("  CabinetWClass ") == "CabinetWClass");

    Names list{ "notepad" };
    CHECK_FALSE(addName(list, "NotePad"));
    CHECK_FALSE(addName(list, ""));
    CHECK(addName(list, "code"));
    CHECK(list == Names{ "notepad", "code" });
}

TEST_CASE("rules: where")
{
    CHECK(classifyWhere("") == Where::Off);
    CHECK(classifyWhere("false") == Where::Off);
    CHECK(classifyWhere("0") == Where::Off);
    CHECK(classifyWhere("!process.is_explorer") == Where::ExplorerOnly);
    CHECK(classifyWhere("  ! process.is_explorer ") == Where::ExplorerOnly);
    CHECK(classifyWhere("true") == Where::Custom);
    CHECK(classifyWhere("process.name == \"notepad\"") == Where::Custom);
}

TEST_CASE("rules: pending changes take expressions only where the key does")
{
    PendingChanges p;
    CHECK(p.set("settings.exclude.where", "!process.is_explorer", "Rules").empty());
    CHECK(p.set("settings.exclude.process", "[\"notepad\", \"code\"]", "Rules").empty());
    CHECK(p.set("settings.exclude.window", "if(sys.is11, 'a', 'b')", "Rules").empty());
    CHECK_FALSE(p.set("settings.showdelay", "sys.ver", "Behavior").empty());   // a Number key
    CHECK_FALSE(p.set("settings.showdelay", "5000", "Behavior").empty());      // out of range
    CHECK_FALSE(p.set("settings.exclude.where", "a ||\n b", "Rules").empty()); // one line only
    CHECK_FALSE(p.set("settings.exclude.where", "  ", "Rules").empty());
    CHECK(p.set("settings.showdelay", "200", "Behavior").empty());
    CHECK(p.set("settings.tip.default", "[#202020, #FFFFFF]", "Behavior").empty());
    CHECK(p.count() == 5);
    CHECK(p.describe() == "Behavior: showdelay, tip.default; Rules: exclude.process, exclude.where, exclude.window");
}
