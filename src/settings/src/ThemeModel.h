#pragma once

// ThemeModel: an approximation of how shell.dll turns theme{} values into the
// colours and metrics of a menu, for the Appearance preview.
//
// resolve() follows ContextMenu.cpp (the theme section of the menu set-up): pick
// the base theme from theme.name and the dark mode, apply the background effect,
// then the individual theme.* keys on top. The base tables are a copy of
// Theme::Default/Modern/Light/Dark/White/Black in src/dll/src/Include/Theme.h,
// for Windows 11. Only literal values are used; expressions keep the default.
//
// Colours are 0xAARRGGBB. Lengths are in DIPs at 100 % scaling, before the font
// scale (Resolved::scale) is applied.

#include "nss/Literal.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace theme {

using Values = std::map<std::string, nss::Literal>; // canonical key -> literal

struct SystemInfo
{
    bool appsDark = false;      // AppsUseLightTheme == 0
    bool transparency = true;   // Personalize\EnableTransparency
    uint32_t accent = 0xFF0078D4;
    bool win11 = true;
};

struct Box
{
    int l = 0, t = 0, r = 0, b = 0;
    bool operator==(const Box &) const = default;
};

struct States
{
    uint32_t nor = 0, sel = 0, norDis = 0, selDis = 0;
};

// One colour stop of theme.background.gradient: offset 0..1, colour with its opacity.
struct GradientStop
{
    double offset = 0;
    uint32_t color = 0xFF000000;
};

struct Resolved
{
    std::string type = "auto"; // auto | modern | classic | white | black
    bool dark = false;
    bool transparency = false; // the "enableTransparency" path of the core
    int effect = 0;            // 0 none, 1 transparent, 2 blur, 3 acrylic
    // The background colour with theme.background.opacity as its alpha: the core
    // paints it over the effect, so 0 shows only the effect and 0xFF is solid (and
    // turns the blur/acrylic window off).
    uint32_t background = 0xFFF9F9F9;
    // The blur/acrylic tint the core hands to DWM (AccentPolicy.GradientColor): the
    // theme's default, then effect = [n, tint, opacity], then background.tintcolor.
    uint32_t tint = 0;

    // theme.background.gradient, drawn over the background (ContextMenu.cpp): a
    // linear gradient along `gradientAngle` (CSS degrees: 0 is bottom to top, 90
    // left to right), or a radial one from the centre.
    bool gradient = false;
    bool gradientRadial = false;
    double gradientAngle = 0;
    std::vector<GradientStop> gradientStops;

    int borderSize = 1;
    uint32_t borderColor = 0;
    int borderRadius = 8;      // px
    Box borderPadding;

    bool shadow = true;
    int shadowSize = 3;
    int shadowOffset = 2;
    uint32_t shadowColor = 0x40000000;

    States text;
    States back;
    States symbol;
    int itemRadius = 4;        // px
    int itemOpacity = 100;
    Box itemPadding;
    Box itemMargin;

    int separatorSize = 1;
    uint32_t separatorColor = 0;
    Box separatorMargin;

    std::string fontName;      // empty: the menu font (Segoe UI)
    double fontSize = 14;      // the core's lfHeight
    int fontWeight = 400;
    bool italic = false;
    double scale = 1.0;        // fontSize / default size: the core rescales every metric

    bool imageEnabled = true;
    int imageSize = 16;
    int imageGap = 10;
    uint32_t imageColor[3] = { 0, 0, 0 };
};

// The 0..4 radius step -> px (Theme::radius).
int radiusPx(int step);
// theme.view value -> vertical item padding; nullopt for auto.
std::optional<int> viewPadding(const nss::Literal &value);

// A colour literal (hex, color.* names, none) as 0xAARRGGBB; nullopt for
// auto/default and anything that is not a colour.
std::optional<uint32_t> colorOf(const nss::Literal &value, const SystemInfo &sys);

Resolved resolve(const Values &values, const SystemInfo &sys);

// What the preview draws for the acrylic material under the tint (0 when the
// effect isn't acrylic). DWM's legacy acrylic (ACCENT_ENABLE_ACRYLICBLURBEHIND,
// which the core uses) is the blurred backdrop, a faint layer of the tint's own
// colour, then the tint at its alpha. The faint layer fades out as the tint's
// alpha grows, so a light tint (#2C2C2C40) stays see-through, as in the real menu.
uint32_t acrylicLayer(const Resolved &r);

// theme.background.opacity as the core applies it, in percent (0..100).
int backgroundOpacityPercent(const Resolved &r);

// The Effect choice of the Appearance page: 0 auto, 1 none, 2 transparent,
// 3 blur, 4 acrylic. `effect` and `opacity` are the literals in effect now (null
// when unset). Returns what to write:
//   effect        the literal for theme.background.effect. An existing
//                 [n, tint, opacity] array keeps its tint and opacity for blur and
//                 acrylic (only n changes), since the tint lives in it.
//   clearOpacity  the background opacity is a number >= 100, which paints the
//                 background solid over the effect (the core then doesn't even
//                 create the blur window), so it has to go back to auto for the
//                 chosen effect to show. Otherwise opacity is left alone: an
//                 unset or auto opacity stays so, and 0 is never written.
struct EffectChoice
{
    std::string effect;
    bool clearOpacity = false;
};
EffectChoice effectChoice(int choice, const nss::Literal *effect, const nss::Literal *opacity);

// The gradient's colour at `t` (0..1) between its stops; transparent without one.
uint32_t gradientAt(const Resolved &r, double t);

// The default font size (lfHeight) the core uses on this Windows version.
double defaultFontSize(const SystemInfo &sys);

// The theme cards of the Appearance page and the first-run dialog, in order.
inline constexpr const char *kThemeNames[] = { "auto", "modern", "classic", "white", "black" };
// The theme.name literal a card writes ("\"modern\""); "" for an index out of range.
std::string themeNameLiteral(int index);
// The card for a theme.name value: its index in kThemeNames, 0 (auto) when unknown.
// A number (the core's legacy form) goes by `resolvedType`, resolve()'s Resolved::type.
int themeIndex(const nss::Literal &name, const std::string &resolvedType = "auto");

} // namespace theme
