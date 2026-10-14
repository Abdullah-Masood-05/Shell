#include "doctest.h"

#include "PendingChanges.h"
#include "ThemeModel.h"

using namespace theme;
using nss::Literal;

namespace {

Values make(std::initializer_list<std::pair<const char *, const char *>> kv)
{
    Values v;
    for (auto &[k, t] : kv)
        v[k] = Literal::parse(t);
    return v;
}

SystemInfo sys(bool dark, bool transparency = false)
{
    SystemInfo s;
    s.appsDark = dark;
    s.transparency = transparency;
    s.accent = 0xFF3366CC;
    s.win11 = true;
    return s;
}

constexpr uint32_t alpha(uint32_t c) { return c >> 24; }

} // namespace

TEST_CASE("theme model: radius steps and view padding")
{
    CHECK(radiusPx(0) == 0);
    CHECK(radiusPx(1) == 4);
    CHECK(radiusPx(2) == 8);
    CHECK(radiusPx(3) == 12);
    CHECK(radiusPx(4) == 16);
    CHECK(radiusPx(9) == 4);
    CHECK(viewPadding(Literal::parse("view.compact")) == 0);
    CHECK(viewPadding(Literal::parse("view.small")) == 2);
    CHECK(viewPadding(Literal::parse("view.medium")) == 4);
    CHECK(viewPadding(Literal::parse("view.large")) == 8);
    CHECK(viewPadding(Literal::parse("view.wide")) == 10);
    CHECK_FALSE(viewPadding(Literal::parse("auto")).has_value());
}

TEST_CASE("theme model: colours")
{
    auto s = sys(false);
    CHECK(colorOf(Literal::parse("#20232A"), s) == 0xFF20232Au);
    CHECK(colorOf(Literal::parse("#20232A80"), s) == 0x8020232Au);
    CHECK(colorOf(Literal::parse("#fff"), s) == 0xFFFFFFFFu);
    CHECK(colorOf(Literal::parse("color.accent"), s) == 0xFF3366CCu);
    CHECK(colorOf(Literal::parse("none"), s) == 0u);
    CHECK_FALSE(colorOf(Literal::parse("auto"), s).has_value());
    CHECK_FALSE(colorOf(Literal::parse("50"), s).has_value());
}

TEST_CASE("theme model: modern, opaque, follows the Windows mode")
{
    auto dark = resolve(make({ { "theme.name", "\"modern\"" } }), sys(true));
    CHECK(dark.type == "modern");
    CHECK(dark.dark);
    CHECK(dark.effect == 0);
    CHECK(dark.background == 0xFF2C2C2Cu);
    CHECK(dark.back.sel == 0xFF383838u);
    CHECK(dark.text.nor == 0xFFFFFFFFu);
    CHECK(dark.separatorColor == 0xFF3D3D3Du);
    CHECK(dark.borderRadius == 8);
    CHECK(dark.itemRadius == 4);
    CHECK(dark.itemPadding == Box{ 10, 4, 10, 4 });
    CHECK(dark.fontSize == 14);
    CHECK(dark.scale == doctest::Approx(1.0));

    auto light = resolve(make({ { "theme.name", "\"modern\"" } }), sys(false));
    CHECK_FALSE(light.dark);
    CHECK(light.background == 0xFFF9F9F9u);
    CHECK(light.back.sel == 0xFFF0F0F0u);
    CHECK(light.text.nor == 0xFF000000u);

    // theme.dark wins over the Windows mode.
    auto forced = resolve(make({ { "theme.name", "\"modern\"" }, { "theme.dark", "false" } }), sys(true));
    CHECK_FALSE(forced.dark);
    CHECK(forced.background == 0xFFF9F9F9u);
    auto autoDark = resolve(make({ { "theme.name", "\"modern\"" }, { "theme.dark", "auto" } }), sys(true));
    CHECK(autoDark.dark);
}

TEST_CASE("theme model: effects")
{
    // System transparency on and no effect key: acrylic, background cleared.
    auto acrylic = resolve(make({ { "theme.name", "\"modern\"" } }), sys(true, true));
    CHECK(acrylic.effect == 3);
    CHECK(acrylic.transparency);
    CHECK(alpha(acrylic.background) == 0);
    CHECK(acrylic.tint == 0x402C2C2Cu);
    CHECK(acrylic.borderSize == 1);

    // effect = auto forces transparency even when Windows has it off.
    auto forced = resolve(make({ { "theme.name", "\"modern\"" }, { "theme.background.effect", "auto" } }), sys(false, false));
    CHECK(forced.effect == 3);
    CHECK(forced.tint == 0x00FFFFFFu); // modern light acrylic

    // effect = 0: opaque, opacity ignored.
    auto none = resolve(make({ { "theme.background.effect", "0" }, { "theme.background.opacity", "40" } }), sys(true, true));
    CHECK(none.effect == 0);
    CHECK(alpha(none.background) == 0xFF);

    // Transparent with a colour and an opacity.
    auto clear = resolve(make({ { "theme.background.effect", "1" },
                                { "theme.background.color", "#20232A" },
                                { "theme.background.opacity", "50" } }),
                         sys(true, true));
    CHECK(clear.effect == 1);
    CHECK(clear.background == 0x7F20232Au);

    // [3, tint, opacity] and the tintcolor key.
    auto tinted = resolve(make({ { "theme.background.effect", "[3, #FF0000, 50]" } }), sys(true, true));
    CHECK(tinted.effect == 3);
    CHECK(tinted.tint == 0x7FFF0000u);
    auto tintKey = resolve(make({ { "theme.background.effect", "2" }, { "theme.background.tintcolor", "#00FF0040" } }), sys(true, true));
    CHECK(tintKey.effect == 2);
    CHECK(tintKey.tint == 0x4000FF00u);

    // Blur without system transparency falls back to none.
    auto blurOff = resolve(make({ { "theme.background.effect", "2" } }), sys(true, false));
    CHECK(blurOff.effect == 0);
}

TEST_CASE("theme model: white, black, classic")
{
    auto white = resolve(make({ { "theme.name", "\"white\"" } }), sys(true));
    CHECK(white.background == 0xFFFFFFFFu);
    CHECK(white.separatorSize == 3);
    CHECK(white.text.nor == 0xFF000000u);
    auto black = resolve(make({ { "theme.name", "\"black\"" } }), sys(false));
    CHECK(black.background == 0xFF000000u);
    CHECK(black.back.sel == 0xFF202020u);
    auto classic = resolve(make({ { "theme.name", "\"classic\"" } }), sys(false));
    CHECK(classic.back.sel == 0xFF0078D4u);
    CHECK(classic.text.sel == 0xFFFFFFFFu);
    CHECK(classic.itemMargin == Box{ 0, 4, 0, 4 });
    CHECK(classic.itemPadding.t == 0);
    auto byNumber = resolve(make({ { "theme.name", "4" } }), sys(false));
    CHECK(byNumber.type == "black");
}

TEST_CASE("theme model: individual keys")
{
    auto r = resolve(make({ { "theme.name", "\"modern\"" },
                            { "theme.view", "view.wide" },
                            { "theme.border.radius", "3" },
                            { "theme.item.radius", "0" },
                            { "theme.border.size", "2" },
                            { "theme.border.color", "color.accent" },
                            { "theme.shadow.enabled", "false" },
                            { "theme.item.text.select", "#FF0000" },
                            { "theme.item.back", "#111111" },
                            { "theme.item.back.select", "#222222" },
                            { "theme.separator.size", "5" },
                            { "theme.font.size", "21" },
                            { "theme.font.weight", "7" },
                            { "theme.font.name", "\"Consolas\"" },
                            { "theme.image.gap", "4" } }),
                     sys(true));
    CHECK(r.itemPadding.t == 10);
    CHECK(r.itemPadding.b == 10);
    CHECK(r.borderRadius == 12);
    CHECK(r.itemRadius == 0);
    CHECK(r.borderSize == 2);
    CHECK(r.borderColor == 0xFF3366CCu);
    CHECK_FALSE(r.shadow);
    CHECK(r.text.sel == 0xFFFF0000u);
    CHECK(r.text.nor == 0xFFFFFFFFu);
    CHECK(r.back.norDis == 0xFF111111u);
    CHECK(r.back.sel == 0xFF222222u);
    CHECK(r.symbol.sel == 0xFFFF0000u); // chevrons follow the text colours
    CHECK(r.separatorSize == 5);
    CHECK(r.fontSize == 21);
    CHECK(r.scale == doctest::Approx(1.5));
    CHECK(r.fontWeight == 700);
    CHECK(r.fontName == "Consolas");
    CHECK(r.imageGap == 4);

    // Padding after view: item.padding wins; boxes take n, [h, v] and [l, t, r, b].
    auto p = resolve(make({ { "theme.view", "view.compact" }, { "theme.item.padding", "[12, 6]" }, { "theme.item.margin", "[1, 2, 3, 4]" },
                            { "theme.separator.margin", "7" } }),
                     sys(false));
    CHECK(p.itemPadding == Box{ 12, 6, 12, 6 });
    CHECK(p.itemMargin == Box{ 1, 2, 3, 4 });
    CHECK(p.separatorMargin == Box{ 7, 7, 7, 7 });

    // Out-of-range values are clamped like the core does.
    auto c = resolve(make({ { "theme.border.size", "40" }, { "theme.font.size", "2" } }), sys(false));
    CHECK(c.borderSize == 10);
    CHECK(c.fontSize == 6);

    // A transparent border colour hides the border.
    auto hidden = resolve(make({ { "theme.border.size", "2" }, { "theme.border.opacity", "0" } }), sys(false));
    CHECK(hidden.borderSize == 0);

    // Expressions are not evaluated: the default stays.
    Values expr;
    expr["theme.border.radius"] = Literal::parse("if(sys.is11, 2, 0)");
    CHECK(resolve(expr, sys(false)).borderRadius == resolve({}, sys(false)).borderRadius);
}

TEST_CASE("theme model: separator alpha follows a translucent background")
{
    auto r = resolve(make({ { "theme.name", "\"classic\"" }, { "theme.background.effect", "1" }, { "theme.background.opacity", "50" } }),
                     sys(false, true));
    CHECK(alpha(r.background) == 127);
    // The classic transparent table already has a translucent separator.
    CHECK(alpha(r.separatorColor) == 0x15);
    auto opaqueSep = resolve(make({ { "theme.name", "\"classic\"" }, { "theme.background.effect", "1" },
                                    { "theme.background.opacity", "50" } }),
                             sys(false, false));
    // Effect 1 without system transparency keeps the opaque classic table, then
    // the separator takes the background's opacity + 5.
    CHECK(alpha(opaqueSep.separatorColor) == 127 + 5);
}

TEST_CASE("theme model: background gradient")
{
    // The "Gradient" example (discussion #174): bottom to top, accent dark3 -> accent -> dark3 at 90 %.
    auto g = resolve(make({ { "theme.background.opacity", "0" },
                            { "theme.background.gradient.enabled", "1" },
                            { "theme.background.gradient.linear", "[0, 100, 0, 0]" },
                            { "theme.background.gradient.stop",
                              "[[0, color.accent_dark3, 90], [.5, color.accent, 90], [1, color.accent_dark3, 90]]" } }),
                     sys(true, true));
    REQUIRE(g.gradient);
    CHECK_FALSE(g.gradientRadial);
    CHECK(g.gradientAngle == doctest::Approx(0.0)); // from (0, 100 %) up to (0, 0)
    REQUIRE(g.gradientStops.size() == 3);
    CHECK(alpha(g.gradientStops[1].color) == 229);   // 90 %
    CHECK((gradientAt(g, 0.5) & 0xFFFFFFu) == 0x3366CCu); // the accent in the middle
    CHECK(gradientAt(g, 0.0) == g.gradientStops[0].color);
    CHECK(gradientAt(g, 0.25) != gradientAt(g, 0.0));

    // Left to right.
    auto lr = resolve(make({ { "theme.background.gradient.enabled", "true" },
                             { "theme.background.gradient.linear", "[0, 0, 100, 0]" },
                             { "theme.background.gradient.stop", "[[0, #FF0000], [1, #0000FF]]" } }),
                      sys(true, true));
    REQUIRE(lr.gradient);
    CHECK(lr.gradientAngle == doctest::Approx(90.0));
    CHECK(gradientAt(lr, 0) == 0xFFFF0000u);
    CHECK(gradientAt(lr, 1) == 0xFF0000FFu);

    // Off without enabled, without a shape, or with the single-stop form the core ignores.
    CHECK_FALSE(resolve(make({ { "theme.background.gradient.linear", "[0, 0, 100, 0]" },
                               { "theme.background.gradient.stop", "[[0, #FF0000]]" } }),
                        sys(true, true)).gradient);
    CHECK_FALSE(resolve(make({ { "theme.background.gradient.enabled", "1" },
                               { "theme.background.gradient.stop", "[[0, #FF0000]]" } }),
                        sys(true, true)).gradient);
    CHECK_FALSE(resolve(make({ { "theme.background.gradient.enabled", "1" },
                               { "theme.background.gradient.linear", "[0, 0, 100, 0]" },
                               { "theme.background.gradient.stop", "[0.5, #FF0000, 20]" } }),
                        sys(true, true)).gradient);
    CHECK(gradientAt(Resolved{}, 0.5) == 0u);
}

namespace {

// What the Effect chips stage (AppearanceController::selectEffect without the
// SaveService): the effect literal, and opacity only when it has to go.
PendingChanges choose(int choice, const Values &now)
{
    auto at = [&](const char *key) -> const Literal * {
        auto it = now.find(key);
        return it == now.end() ? nullptr : &it->second;
    };
    auto pick = effectChoice(choice, at("theme.background.effect"), at("theme.background.opacity"));
    PendingChanges p;
    REQUIRE(p.set("theme.background.effect", pick.effect, "Appearance").empty());
    if (pick.clearOpacity)
        REQUIRE(p.set("theme.background.opacity", "auto", "Appearance").empty());
    return p;
}

} // namespace

TEST_CASE("theme model: acrylic, opacity and auto (the core's semantics)")
{
    // The report: theme.nss has background { color=auto opacity=auto effect=auto },
    // the app then wrote opacity = 0, effect = 3. On Windows 11 with Transparency
    // effects on, that is exactly what auto already gave: acrylic, the theme's
    // tint (#2C2C2C40 dark) and a fully clear background colour over it.
    const auto user = make({ { "theme.name", "\"modern\"" },
                             { "theme.dark", "auto" },
                             { "theme.background.color", "auto" },
                             { "theme.background.opacity", "auto" },
                             { "theme.background.effect", "auto" } });
    auto saved = user;
    saved["theme.background.opacity"] = Literal::parse("0");
    saved["theme.background.effect"] = Literal::parse("3");
    const auto a = resolve(user, sys(true, true));
    const auto b = resolve(saved, sys(true, true));
    CHECK(a.effect == 3);
    CHECK(b.effect == 3);
    CHECK(a.tint == 0x402C2C2Cu);
    CHECK(b.tint == a.tint);
    CHECK(b.background == a.background);
    CHECK(backgroundOpacityPercent(a) == 0); // auto resolves to 0 % under an effect

    // With Transparency effects off, auto still forces acrylic; an explicit 3 is dropped.
    CHECK(resolve(user, sys(true, false)).effect == 3);
    CHECK(resolve(saved, sys(true, false)).effect == 0);
    CHECK(backgroundOpacityPercent(resolve(saved, sys(true, false))) == 100);

    // Opacity is the background colour over the effect: 100 % is solid.
    auto solid = saved;
    solid["theme.background.opacity"] = Literal::parse("100");
    CHECK(alpha(resolve(solid, sys(true, true)).background) == 0xFF);
}

TEST_CASE("theme model: the Effect chips leave an auto opacity alone")
{
    // Unset, auto and a user's 0 stay as they are: only the effect is staged.
    for (const char *opacity : { "", "auto", "0", "40" }) {
        Values now = make({ { "theme.background.effect", "auto" } });
        if (*opacity)
            now["theme.background.opacity"] = Literal::parse(opacity);
        auto p = choose(4, now);
        CHECK_MESSAGE(p.items().size() == 1, opacity);
        REQUIRE(p.find("theme.background.effect"));
        CHECK(*p.find("theme.background.effect")->literal == "3");
        CHECK_MESSAGE(p.find("theme.background.opacity") == nullptr, opacity);
    }
    // Every choice maps to the core's numbers.
    CHECK(effectChoice(0, nullptr, nullptr).effect == "auto");
    CHECK(effectChoice(1, nullptr, nullptr).effect == "0");
    CHECK(effectChoice(2, nullptr, nullptr).effect == "1");
    CHECK(effectChoice(3, nullptr, nullptr).effect == "2");
    CHECK(effectChoice(4, nullptr, nullptr).effect == "3");
    CHECK(effectChoice(5, nullptr, nullptr).effect.empty());
}

TEST_CASE("theme model: choosing acrylic writes what the core reads")
{
    // A solid background (a 1.x preset's opacity = 100) would hide the acrylic: it goes back to auto.
    auto p = choose(4, make({ { "theme.background.effect", "0" }, { "theme.background.opacity", "100" } }));
    REQUIRE(p.find("theme.background.opacity"));
    CHECK(*p.find("theme.background.opacity")->literal == "auto");
    // ...but not for None, where opacity doesn't apply.
    CHECK(choose(1, make({ { "theme.background.opacity", "100" } })).find("theme.background.opacity") == nullptr);

    // An [n, tint, opacity] array keeps its tint for blur and acrylic.
    const auto arr = make({ { "theme.background.effect", "[2, color.accent, 50]" } });
    auto q = choose(4, arr);
    CHECK(*q.find("theme.background.effect")->literal == "[3, color.accent, 50]");
    auto acrylic = resolve(make({ { "theme.background.effect", "[3, color.accent, 50]" } }), sys(true, true));
    CHECK(acrylic.effect == 3);
    CHECK(acrylic.tint == 0x7F3366CCu);
    // None and auto write the plain value (the array's first item must be a number).
    CHECK(*choose(0, arr).find("theme.background.effect")->literal == "auto");
    CHECK(*choose(1, arr).find("theme.background.effect")->literal == "0");
}

TEST_CASE("theme model: the tint round-trips through background.tintcolor")
{
    // The colour picker's value -> the literal written -> what resolve() (and the core) reads.
    const auto lit = Literal::makeColor(0x2C2C2CB0);
    CHECK(lit.text == "#2C2C2CB0");
    PendingChanges p;
    REQUIRE(p.set("theme.background.tintcolor", lit.text, "Appearance").empty());
    auto r = resolve(make({ { "theme.background.effect", "3" }, { "theme.background.tintcolor", "#2C2C2CB0" } }),
                     sys(true, true));
    CHECK(r.tint == 0xB02C2C2Cu);
    // Without alpha the tint is opaque (FF), which hides the blur.
    CHECK(resolve(make({ { "theme.background.effect", "3" }, { "theme.background.tintcolor", "#2C2C2C" } }),
                  sys(true, true)).tint == 0xFF2C2C2Cu);
    // tintcolor wins over the array's tint, as in the core.
    auto both = resolve(make({ { "theme.background.effect", "[3, #FF0000, 50]" },
                               { "theme.background.tintcolor", "#00FF0040" } }),
                        sys(true, true));
    CHECK(both.tint == 0x4000FF00u);
}

TEST_CASE("theme model: the preview's acrylic layers")
{
    // Dark acrylic, default tint: blurred backdrop, a faint tint-coloured layer, the 25 % tint.
    auto dark = resolve(make({ { "theme.background.effect", "3" } }), sys(true, true));
    CHECK(acrylicLayer(dark) == 0x132C2C2Cu); // (255 - 0x40) * 10 %
    CHECK(alpha(dark.background) == 0);       // nothing over it
    // An opaque tint leaves no room for the layer; blur and none have none.
    auto opaque = resolve(make({ { "theme.background.effect", "3" }, { "theme.background.tintcolor", "#202020" } }),
                          sys(true, true));
    CHECK(acrylicLayer(opaque) == 0x00202020u);
    CHECK(acrylicLayer(resolve(make({ { "theme.background.effect", "2" } }), sys(true, true))) == 0u);
    CHECK(acrylicLayer(resolve(make({ { "theme.background.effect", "0" } }), sys(true, true))) == 0u);
    // Light modern acrylic: the clear white tint the core sets.
    auto light = resolve(make({ { "theme.name", "\"modern\"" }, { "theme.background.effect", "3" } }), sys(false, true));
    CHECK(light.tint == 0x00FFFFFFu);
    CHECK(acrylicLayer(light) == 0x1AFFFFFFu);
}
