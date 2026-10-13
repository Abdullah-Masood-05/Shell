#include "TestUtil.h"

using namespace nss;

namespace
{
	bool valid(std::string_view key, std::string_view value)
	{
		return ThemeSchema::validate(key, value).empty();
	}
}

TEST_CASE("schema: key tables and aliases")
{
	CHECK(ThemeSchema::keys().size() > 100);
	CHECK(ThemeSchema::isKnown("theme.background.opacity"));
	CHECK(ThemeSchema::isKnown("Theme.Font.Weight"));
	CHECK(ThemeSchema::isKnown("settings.modify.remove.duplicate"));
	CHECK(ThemeSchema::isKnown("theme.symbol.chevron.select.disabled"));
	CHECK(ThemeSchema::isKnown("theme.item.padding.left"));
	CHECK_FALSE(ThemeSchema::isKnown("theme.background.blur"));
	CHECK_FALSE(ThemeSchema::isKnown("settings.modify.remove"));

	CHECK(ThemeSchema::canonical("theme") == "theme.name");
	CHECK(ThemeSchema::canonical("theme.background") == "theme.background.color");
	CHECK(ThemeSchema::canonical("theme.shadow") == "theme.shadow.enabled");
	CHECK(ThemeSchema::canonical("theme.border") == "theme.border.size");
	CHECK(ThemeSchema::canonical("theme.image.align") == "theme.image.display");
	CHECK(ThemeSchema::canonical("theme.tip.width") == "settings.tip.width");
	CHECK(ThemeSchema::canonical("theme.tip") == "settings.tip.enabled");
	CHECK(ThemeSchema::canonical("settings") == "settings.priority");
	CHECK(ThemeSchema::canonical("settings.exclude") == "settings.exclude.where");
	CHECK(ThemeSchema::canonical("settings.winx") == "settings.winx.replace");
	CHECK(ThemeSchema::isKnown("settings.winx.debug"));
	CHECK(ThemeSchema::order("theme.name") < ThemeSchema::order("settings.priority"));
	CHECK(ThemeSchema::order("bogus") == ThemeSchema::keys().size());
}

TEST_CASE("schema: ranges")
{
	CHECK(valid("theme.background.opacity", "0"));
	CHECK(valid("theme.background.opacity", "100"));
	CHECK_FALSE(valid("theme.background.opacity", "101"));
	CHECK_FALSE(valid("theme.background.opacity", "-1"));
	CHECK(valid("theme.item.opacity", "auto"));

	CHECK(valid("theme.item.radius", "4"));
	CHECK_FALSE(valid("theme.item.radius", "5"));
	CHECK_FALSE(valid("theme.item.radius", "1.5"));

	CHECK(valid("theme.font.size", "6"));
	CHECK(valid("theme.font.size", "48"));
	CHECK_FALSE(valid("theme.font.size", "5"));
	CHECK(valid("theme.font.weight", "1"));
	CHECK(valid("theme.font.weight", "9"));
	CHECK_FALSE(valid("theme.font.weight", "10"));
	CHECK_FALSE(valid("theme.font.weight", "0"));

	CHECK(valid("theme.border.size", "10"));
	CHECK_FALSE(valid("theme.border.size", "11"));
	CHECK_FALSE(valid("theme.border", "11"));  // alias of border.size
	CHECK(valid("theme.shadow.size", "30"));
	CHECK_FALSE(valid("theme.shadow.size", "31"));
	CHECK(valid("theme.separator.size", "40"));
	CHECK_FALSE(valid("theme.separator.size", "41"));
	CHECK(valid("settings.showdelay", "4000"));
	CHECK_FALSE(valid("settings.showdelay", "4001"));
	CHECK(valid("theme.layout.popup.align", "-20"));
	CHECK_FALSE(valid("theme.layout.popup.align", "21"));

	auto errors = ThemeSchema::validate("theme.item.radius", "9");
	REQUIRE(errors.size() == 1);
	CHECK(errors[0] == "theme.item.radius: 9 is out of range (0..4)");
}

TEST_CASE("schema: effect, enums, colors, bools, boxes")
{
	for(auto v : { "0", "1", "2", "3", "auto", "[3, #202020, 50]", "[3, color.accent]", "[2]" })
		CHECK_MESSAGE(valid("theme.background.effect", v), v);
	for(auto v : { "4", "-1", "[5, #000, 50]", "[3, 12, 50]", "[3, #000, 101]", "[3, #000, 50, 1]", "\"blur\"" })
		CHECK_FALSE_MESSAGE(valid("theme.background.effect", v), v);

	for(auto v : { "\"auto\"", "\"classic\"", "\"white\"", "\"black\"", "\"modern\"", "auto", "'modern'" })
		CHECK_MESSAGE(valid("theme.name", v), v);
	CHECK_FALSE(valid("theme.name", "\"fancy\""));
	CHECK(valid("theme", "\"black\""));

	for(auto v : { "view.compact", "view.small", "view.medium", "view.large", "view.wide", "auto" })
		CHECK_MESSAGE(valid("theme.view", v), v);
	CHECK_FALSE(valid("theme.view", "view.huge"));
	CHECK_FALSE(valid("theme.view", "color.compact"));
	CHECK(ThemeSchema::validate("theme.view", "view.huge")[0] ==
		  "theme.view: expected one of auto, view.compact, view.small, view.medium, view.large, view.wide");

	CHECK(valid("theme.background.color", "#1e1e1e"));
	CHECK(valid("theme.background.color", "#1e1e1e80"));
	CHECK(valid("theme.background.color", "color.accent"));
	CHECK(valid("theme.background.color", "default"));
	CHECK_FALSE(valid("theme.background.color", "12"));
	CHECK_FALSE(valid("theme.background.color", "\"red\""));

	CHECK(valid("theme.dark", "true"));
	CHECK(valid("theme.dark", "no"));
	CHECK(valid("theme.dark", "default"));
	CHECK(valid("theme.dark", "1"));
	CHECK_FALSE(valid("theme.dark", "2"));
	CHECK_FALSE(valid("theme.dark", "\"yes\""));

	CHECK(valid("theme.item.padding", "4"));
	CHECK(valid("theme.item.padding", "[4, 2]"));
	CHECK(valid("theme.item.padding", "[1, 2, 3, 4]"));
	CHECK_FALSE(valid("theme.item.padding", "[1, 2, 3]"));

	CHECK(valid("settings.tip.primary", "[#000, #fff]"));
	CHECK(valid("settings.tip.default", "[default, default]"));
	CHECK_FALSE(valid("settings.tip.primary", "[#000, #fff, #f00]"));
	CHECK(valid("theme.image.color", "[#fff, #0af, #f00]"));

	CHECK(valid("theme.font.name", "\"Segoe UI\""));
	CHECK_FALSE(valid("theme.font.name", "12"));
}

TEST_CASE("schema: expressions are accepted, unknown keys are not")
{
	CHECK(valid("theme.dark", "if(sys.is11, true, false)"));
	CHECK(valid("theme.background.opacity", "@(50 + 10)"));
	CHECK_FALSE(valid("theme.background.opacity", ""));
	auto e = ThemeSchema::validate("theme.nope", "1");
	REQUIRE(e.size() == 1);
	CHECK(e[0] == "theme.nope: unknown key");
}
