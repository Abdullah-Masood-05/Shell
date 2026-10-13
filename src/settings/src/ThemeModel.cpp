#include "ThemeModel.h"

#include "nss/Text.h"

#include <algorithm>
#include <cmath>

using nss::Literal;
using nss::LiteralKind;

namespace theme {

namespace {

constexpr uint32_t alphaOf(uint32_t c) { return c >> 24; }
constexpr uint32_t withAlpha(uint32_t c, uint32_t a) { return (c & 0x00FFFFFFu) | (std::min<uint32_t>(a, 255) << 24); }
// Color::opacity(percent)
constexpr uint32_t withOpacity(uint32_t c, int percent) { return withAlpha(c, uint32_t(percent * 255 / 100)); }
uint8_t percentToByte(double pct) { return uint8_t(std::clamp(pct, 0.0, 100.0) * 255 / 100); }

uint32_t mix(uint32_t a, uint32_t b, double t)
{
    auto ch = [&](int shift) {
        double x = double((a >> shift) & 0xFF), y = double((b >> shift) & 0xFF);
        return uint32_t(std::lround(x + (y - x) * t)) << shift;
    };
    return (a & 0xFF000000u) | ch(16) | ch(8) | ch(0);
}

// The Windows 11 system colours Theme::Default reads with GetSysColor.
constexpr uint32_t sysMenu = 0xFFF0F0F0;        // COLOR_MENU
constexpr uint32_t sysWindowFrame = 0xFF646464; // COLOR_WINDOWFRAME
constexpr uint32_t sysMenuText = 0xFF000000;    // COLOR_MENUTEXT
constexpr uint32_t sysGrayText = 0xFF6D6D6D;    // COLOR_GRAYTEXT
constexpr uint32_t sysHighlight = 0xFF0078D7;   // COLOR_HIGHLIGHT
constexpr uint32_t classicSelect = 0xFF0078D4;  // Color::Swap(0xFF0078D4)

void symbolsAsText(Resolved &th) { th.symbol = th.text; }

Resolved makeDefault(bool dark)
{
    Resolved th;
    th.dark = dark;
    th.background = sysMenu;
    th.borderColor = withOpacity(sysWindowFrame, 50);
    th.borderRadius = 0;
    th.borderSize = 1;
    th.borderPadding = { 0, 4, 0, 4 };
    th.shadowColor = withOpacity(0xFF000000, 25);
    th.shadowSize = 3;
    th.shadowOffset = 2;
    th.separatorColor = withAlpha(th.borderColor, 255);
    th.separatorSize = 1;
    th.separatorMargin = { 8, 3, 8, 3 };
    th.fontSize = 12;
    th.text = { sysMenuText, sysMenuText, sysGrayText, sysGrayText };
    th.back = { th.background, sysHighlight, th.background, th.background };
    th.itemPadding = { 10, 3, 10, 3 };
    th.itemMargin = { 4, 0, 4, 0 };
    th.itemRadius = 0;
    th.imageSize = 16;
    th.imageGap = 10;
    if (dark) {
        th.imageColor[0] = 0xFFE0DFDF;
        th.imageColor[1] = 0xFF4CC2FF;
        th.imageColor[2] = 0x00000000;
    } else {
        th.imageColor[0] = 0xFF555555;
        th.imageColor[1] = 0xFF0078D4;
        th.imageColor[2] = 0xFFFAFAFA;
    }
    symbolsAsText(th);
    return th;
}

// Theme::Modern
Resolved makeModern(bool dark, bool transparent)
{
    auto th = makeDefault(dark);
    th.borderRadius = 8;
    th.borderSize = 0;
    th.borderPadding = { 0, 4, 0, 4 };
    th.separatorSize = 1;
    th.separatorMargin = { 0, 4, 0, 4 };
    th.itemPadding = { 10, 4, 10, 4 };
    th.itemMargin = { 4, 0, 4, 0 };
    th.itemRadius = 4;
    th.borderColor = withOpacity(th.borderColor, 10);
    th.shadowSize = 3;
    th.shadowOffset = 2;
    th.shadowColor = 0x80000000;
    if (dark) {
        th.shadowColor = withAlpha(th.shadowColor, 0x20);
        th.borderColor = 0xFF2C2C2C;
        th.background = 0xFF2C2C2C;
        th.text = { 0xFFFFFFFF, 0xFFFFFFFF, 0x80FFFFFF, 0x80FFFFFF };
        if (transparent) {
            th.borderSize = 1;
            th.borderColor = withAlpha(th.background, 0x40);
            th.background = withAlpha(th.background, 0);
            th.tint = withAlpha(th.background, 0x40);
            th.back = { 0x00000000, 0x10FFFFFF, 0x00000000, 0x05FFFFFF };
            th.separatorColor = 0x15FFFFFF;
        } else {
            th.separatorColor = 0xFF3D3D3D;
            th.back = { 0xFF2C2C2C, 0xFF383838, 0xFF2C2C2C, 0xFF2C2C2C };
        }
    } else {
        th.shadowColor = withAlpha(th.shadowColor, 0x10);
        th.borderColor = 0xFFF9F9F9;
        th.separatorColor = 0xFFEAEAEA;
        th.text = { 0xFF000000, 0xFF000000, 0x80000000, 0x80000000 };
        if (transparent) {
            th.background = withAlpha(th.background, 0);
            th.tint = withAlpha(th.background, 0xCF);
            th.back = { 0x00000000, 0x08000000, 0x00000000, 0x05000000 };
            th.separatorColor = 0x15000000;
        } else {
            th.background = 0xFFF9F9F9;
            th.back = { 0xFFF9F9F9, 0xFFF0F0F0, 0xFFF9F9F9, 0xFFF9F9F9 };
        }
    }
    symbolsAsText(th);
    return th;
}

// The transparent variant shared by Theme::Light/Dark(false) on Windows 11.
void classicTransparent(Resolved &th)
{
    th.borderSize = 1;
    th.borderColor = withAlpha(th.background, 0x40);
    th.background = withAlpha(th.background, 0);
    th.tint = withAlpha(th.background, 0x40);
    th.back.nor = 0;
    th.back.norDis = 0;
    th.back.sel = withAlpha(th.back.sel, 0xAA);
    th.back.selDis = withAlpha(th.back.selDis, 0x80);
    th.separatorColor = 0x15FFFFFF;
}

void classicMetrics(Resolved &th)
{
    th.borderPadding.l = 4;
    th.borderPadding.r = 4;
    th.borderRadius = radiusPx(2);
    th.itemRadius = radiusPx(1);
    th.itemPadding.t = 0;
    th.itemPadding.b = 0;
    th.itemMargin = { 0, 4, 0, 4 };
    th.shadowSize = 3;
}

// Theme::Light(false) / Theme::Dark(false), Windows 11 branch.
Resolved makeClassic(bool dark, bool transparent)
{
    auto th = makeDefault(dark);
    if (dark) {
        th.borderColor = withAlpha(th.borderColor, 255);
        th.separatorColor = 0xFF3E3E3E;
        th.borderColor = 0xFF3E3E3E;
        th.background = 0xFF2B2B2B;
        th.text = { 0xFFFFFFFF, 0xFFFFFFFF, 0xFF6D6D6D, 0xFF6D6D6D };
        th.back = { 0xFF2B2B2B, classicSelect, 0xFF2B2B2B, 0xFF353535 };
    } else {
        th.separatorColor = 0xFFD7D7D7;
        th.borderColor = 0xFFE5E5E5;
        th.background = 0xFFF9F9F9;
        th.text = { 0xFF000000, 0xFFFFFFFF, 0xFF9F9F9F, 0xFF9F9F9F };
        th.back = { 0xFFF9F9F9, classicSelect, 0xFFF9F9F9, 0xFFF0F0F0 };
    }
    classicMetrics(th);
    if (transparent)
        classicTransparent(th);
    th.shadowColor = dark ? withOpacity(th.shadowColor, 25) : withAlpha(th.shadowColor, 0x10);
    symbolsAsText(th);
    return th;
}

// Auto on Windows 11: the classic table, then the colours the core reads from the
// "Menu" visual style (approximated with what Windows 11 draws).
Resolved makeAuto(bool dark, bool transparent)
{
    auto th = makeClassic(dark, false);
    if (dark) {
        th.text = { 0xFFFFFFFF, 0xFFFFFFFF, 0xFF787878, 0xFF787878 };
        th.background = 0xFF2C2C2C;
        th.back = { 0xFF2C2C2C, 0xFF383838, 0xFF2C2C2C, 0xFF333333 };
        th.separatorColor = 0xFF404040;
    } else {
        th.text = { 0xFF1A1A1A, 0xFF1A1A1A, 0xFFA0A0A0, 0xFFA0A0A0 };
        th.background = 0xFFF9F9F9;
        th.back = { 0xFFF9F9F9, 0xFFECECEC, 0xFFF9F9F9, 0xFFF2F2F2 };
        th.separatorColor = 0xFFDADADA;
    }
    th.borderColor = th.separatorColor;
    if (transparent) {
        th.borderSize = 1;
        th.borderColor = withAlpha(th.background, 0x40);
        th.tint = withAlpha(th.background, 0x40);
        th.background = withAlpha(th.background, 0);
        th.back.sel = withOpacity(th.back.sel, 50);
        th.back.selDis = withOpacity(th.back.selDis, 50);
        th.back.norDis = withOpacity(th.back.norDis, 25);
        th.back.nor = 0;
    } else {
        th.back.nor = th.background;
    }
    symbolsAsText(th);
    return th;
}

// Theme::White
Resolved makeWhite(bool transparent)
{
    auto th = makeModern(false, false);
    th.background = 0xFFFFFFFF;
    th.text = { 0xFF000000, 0xFF000000, 0xFF808080, 0xFF808080 };
    th.back = { 0xFFFFFFFF, 0xFFF0F0F0, 0xFFFFFFFF, 0xFFFFFFFF };
    if (transparent) {
        th.borderSize = 1;
        th.borderColor = withAlpha(th.background, 0x40);
        th.background = withAlpha(th.background, 0);
        th.tint = withAlpha(th.background, 0xFF);
        th.back.nor = 0;
        th.back.norDis = 0;
        th.back.sel = withAlpha(th.back.sel, 0xAA);
        th.back.selDis = withAlpha(th.back.selDis, 0x80);
        th.separatorColor = withAlpha(th.separatorColor, 0);
    } else {
        th.separatorColor = th.background;
    }
    th.shadowColor = withAlpha(th.shadowColor, 0x10);
    th.separatorSize = 3;
    symbolsAsText(th);
    return th;
}

// Theme::Black
Resolved makeBlack(bool transparent)
{
    auto th = makeModern(true, false);
    th.background = 0xFF000000;
    th.borderColor = withOpacity(0xFFFFFFFF, 5);
    th.text = { 0xFFFFFFFF, 0xFFFFFFFF, 0xFF808080, 0xFF808080 };
    th.back = { 0xFF000000, 0xFF202020, 0xFF000000, 0xFF000000 };
    if (transparent) {
        th.borderSize = 1;
        th.borderColor = withAlpha(th.background, 0x40);
        th.background = withAlpha(th.background, 0);
        th.tint = withAlpha(th.background, 0xFF);
        th.back.nor = 0;
        th.back.norDis = 0;
        th.back.sel = 0x10FFFFFF;
        th.back.selDis = 0x05FFFFFF;
        th.separatorColor = withAlpha(th.separatorColor, 0);
    } else {
        th.separatorColor = th.background;
    }
    th.shadowColor = withOpacity(th.shadowColor, 25);
    th.separatorSize = 3;
    symbolsAsText(th);
    return th;
}

const Literal *get(const Values &values, const char *key)
{
    auto it = values.find(key);
    if (it == values.end() || !it->second.isLiteral())
        return nullptr;
    return &it->second;
}

bool isAutoKeyword(const Literal &v)
{
    return v.kind == LiteralKind::Keyword && (v.string == "auto" || v.string == "default");
}

std::optional<double> number(const Values &values, const char *key)
{
    if (auto v = get(values, key); v && v->kind == LiteralKind::Number)
        return v->number;
    return std::nullopt;
}

std::optional<bool> boolean(const Values &values, const char *key)
{
    if (auto v = get(values, key); v && v->isBool())
        return v->boolValue();
    return std::nullopt;
}

std::optional<uint32_t> color(const Values &values, const char *key, const SystemInfo &sys)
{
    if (auto v = get(values, key))
        return colorOf(*v, sys);
    return std::nullopt;
}

// Parser get_rect: n (0..400) for all sides, [h, v] or [l, t, r, b].
void applyBox(const Values &values, const std::string &key, Box &box)
{
    if (auto v = get(values, key.c_str())) {
        if (v->kind == LiteralKind::Number) {
            if (v->number >= 0 && v->number <= 400) {
                int n = int(v->number);
                box = { n, n, n, n };
            }
        } else if (v->kind == LiteralKind::Array) {
            auto at = [&](size_t i) -> std::optional<int> {
                if (i < v->items.size() && v->items[i].kind == LiteralKind::Number)
                    return int(v->items[i].number);
                return std::nullopt;
            };
            if (v->items.size() == 2) {
                if (auto h = at(0))
                    box.l = box.r = *h;
                if (auto vv = at(1))
                    box.t = box.b = *vv;
            } else {
                if (auto x = at(0))
                    box.l = *x;
                if (auto x = at(1))
                    box.t = *x;
                if (auto x = at(2))
                    box.r = *x;
                if (auto x = at(3))
                    box.b = *x;
            }
        }
    }
    if (auto n = number(values, (key + ".left").c_str()))
        box.l = int(*n);
    if (auto n = number(values, (key + ".top").c_str()))
        box.t = int(*n);
    if (auto n = number(values, (key + ".right").c_str()))
        box.r = int(*n);
    if (auto n = number(values, (key + ".bottom").c_str()))
        box.b = int(*n);
}

// eval_state: key sets all four, then .normal/.select/.normal.disabled/.select.disabled.
void applyStates(const Values &values, const std::string &key, States &s, const SystemInfo &sys)
{
    if (auto c = color(values, key.c_str(), sys))
        s = { *c, *c, *c, *c };
    if (auto c = color(values, (key + ".normal").c_str(), sys))
        s.nor = *c;
    if (auto c = color(values, (key + ".select").c_str(), sys))
        s.sel = *c;
    if (auto c = color(values, (key + ".normal.disabled").c_str(), sys))
        s.norDis = *c;
    if (auto c = color(values, (key + ".select.disabled").c_str(), sys))
        s.selDis = *c;
}

} // namespace

int radiusPx(int step)
{
    switch (step) {
    case 0: return 0;
    case 1: return 4;
    case 2: return 8;
    case 3: return 12;
    case 4: return 16;
    default: return 4;
    }
}

std::optional<int> viewPadding(const Literal &v)
{
    std::string word;
    if (v.kind == LiteralKind::Number) {
        int n = int(v.number);
        if (n < 0)
            return std::nullopt;
        return std::min(n, 48);
    }
    if (v.kind == LiteralKind::Name && v.string.rfind("view.", 0) == 0)
        word = v.string.substr(5);
    else if (v.kind == LiteralKind::String || v.kind == LiteralKind::Keyword)
        word = nss::toLower(v.string);
    if (word == "compact")
        return 0;
    if (word == "small")
        return 2;
    if (word == "medium")
        return 4;
    if (word == "large")
        return 8;
    if (word == "wide")
        return 10;
    return std::nullopt;
}

std::optional<uint32_t> colorOf(const Literal &v, const SystemInfo &sys)
{
    switch (v.kind) {
    case LiteralKind::Color:
        return (v.rgba >> 8) | ((v.rgba & 0xFF) << 24);
    case LiteralKind::Keyword:
        if (v.string == "none")
            return 0x00000000u;
        return std::nullopt;
    case LiteralKind::Name: {
        const std::string &n = v.string;
        if (n.rfind("color.", 0) != 0)
            return std::nullopt;
        auto name = n.substr(6);
        uint32_t accent = sys.accent | 0xFF000000u;
        if (name == "accent")
            return accent;
        for (int i = 1; i <= 3; ++i) {
            if (name == "accent_light" + std::to_string(i))
                return mix(accent, 0xFFFFFFFF, 0.2 * i);
            if (name == "accent_dark" + std::to_string(i))
                return mix(accent, 0xFF000000, 0.2 * i);
        }
        if (name == "white")
            return 0xFFFFFFFF;
        if (name == "black")
            return 0xFF000000;
        if (name == "transparent")
            return 0x00000000u;
        return std::nullopt;
    }
    default:
        return std::nullopt;
    }
}

double defaultFontSize(const SystemInfo &sys) { return sys.win11 ? 14.0 : 12.0; }

Resolved resolve(const Values &values, const SystemInfo &sys)
{
    // Dark mode: the Windows app mode unless theme.dark says otherwise.
    bool dark = sys.appsDark;
    if (auto d = boolean(values, "theme.dark"))
        dark = *d;

    // theme.name
    std::string type = "auto";
    if (auto v = get(values, "theme.name")) {
        if (v->kind == LiteralKind::String || v->kind == LiteralKind::Keyword) {
            auto w = nss::toLower(nss::trim(v->string));
            if (w == "modern" || w == "white" || w == "black" || w == "classic")
                type = w;
        } else if (v->kind == LiteralKind::Number) {
            switch (int(v->number)) { // ThemeType
            case 3: type = "white"; break;
            case 4: type = "black"; break;
            case 5: type = "classic"; break;
            case 9: type = "modern"; break;
            default: break;
            }
        }
    }

    // theme.background.effect
    bool transparency = sys.transparency;
    int effect = transparency ? 3 : 0;
    std::optional<uint32_t> tint;
    if (auto v = get(values, "theme.background.effect")) {
        auto one = [&](const Literal &o) -> bool {
            if (isAutoKeyword(o)) {
                effect = -1;
                return true;
            }
            if (o.kind == LiteralKind::Number) {
                effect = int(o.number);
                if (effect > 5)
                    effect = 0;
                return true;
            }
            if (o.kind == LiteralKind::String || o.kind == LiteralKind::Keyword) {
                auto w = nss::toLower(nss::trim(o.string));
                if (w == "transparent")
                    effect = 1;
                else if (w == "blur")
                    effect = 2;
                else if (w == "acrylic")
                    effect = 3;
                else if (w == "none")
                    effect = 0;
                else if (w == "auto")
                    effect = -1;
                return true;
            }
            return false;
        };
        if (!one(*v) && v->kind == LiteralKind::Array && !v->items.empty() && one(v->items[0])) {
            if (v->items.size() >= 2)
                tint = colorOf(v->items[1], sys);
            if (tint && v->items.size() == 3 && v->items[2].kind == LiteralKind::Number)
                *tint = withAlpha(*tint, percentToByte(v->items[2].number));
        }
        if (effect == -1) {
            transparency = true;
            effect = sys.win11 ? 3 : 2;
        }
    }
    if (effect > 3)
        effect = 3; // mica / tabbed: drawn like acrylic here
    if (effect == 0)
        transparency = false;
    else if (effect >= 2) {
        if (transparency) {
            if (auto c = color(values, "theme.background.tintcolor", sys))
                tint = *c;
        } else
            effect = 0;
    }

    Resolved th;
    if (type == "white")
        th = makeWhite(effect != 0);
    else if (type == "black")
        th = makeBlack(effect != 0);
    else if (type == "modern") {
        th = makeModern(dark, effect != 0);
        if (transparency && effect == 3 && !dark)
            th.tint = 0x00FFFFFF;
    } else if (type == "classic")
        th = makeClassic(dark, transparency);
    else
        th = makeAuto(dark, transparency);
    th.type = type;
    th.dark = dark;
    th.transparency = transparency;

    // theme.background
    if (auto c = color(values, "theme.background.color", sys))
        th.background = *c;
    else if (transparency)
        th.background = withAlpha(th.background, 0);
    if (auto n = number(values, "theme.background.opacity"))
        th.background = withAlpha(th.background, percentToByte(*n));
    else if (transparency)
        th.background = withAlpha(th.background, 0);
    th.effect = effect;
    if (tint)
        th.tint = *tint;
    if (effect == 0)
        th.background = withAlpha(th.background, 255);
    const uint32_t backgroundOpacity = alphaOf(th.background);

    // theme.view
    if (auto v = get(values, "theme.view")) {
        if (auto pad = viewPadding(*v)) {
            th.itemPadding.t = *pad;
            th.itemPadding.b = *pad;
        }
    }
    th.back.nor = th.background;

    // theme.border
    if (auto n = number(values, "theme.border.size"))
        th.borderSize = std::clamp(int(*n), 0, 10);
    if (th.borderSize > 0) {
        if (auto e = boolean(values, "theme.border.enabled"); e && !*e)
            th.borderSize = 0;
        if (th.borderSize > 0) {
            if (auto c = color(values, "theme.border.color", sys))
                th.borderColor = *c;
            if (auto n = number(values, "theme.border.opacity"))
                th.borderColor = withAlpha(th.borderColor, percentToByte(*n));
            if (alphaOf(th.borderColor) == 0)
                th.borderSize = 0;
        }
    }
    if (auto n = number(values, "theme.border.radius"))
        th.borderRadius = radiusPx(int(*n));
    applyBox(values, "theme.border.padding", th.borderPadding);

    // theme.shadow
    if (auto n = number(values, "theme.shadow.size"))
        th.shadowSize = std::clamp(int(*n), 0, 30);
    if (th.shadowSize > 0) {
        if (auto e = boolean(values, "theme.shadow.enabled"); e && !*e)
            th.shadowSize = 0;
        if (th.shadowSize > 0) {
            if (auto c = color(values, "theme.shadow.color", sys))
                th.shadowColor = *c;
            if (auto n = number(values, "theme.shadow.opacity"))
                th.shadowColor = withAlpha(th.shadowColor, percentToByte(*n));
            if (alphaOf(th.shadowColor) == 0)
                th.shadowSize = 0;
            else if (auto o = number(values, "theme.shadow.offset"))
                th.shadowOffset = std::clamp(int(*o), 0, 30);
        }
    }
    th.shadow = th.shadowSize > 0 && alphaOf(th.shadowColor) > 0;

    // theme.item
    applyStates(values, "theme.item.text", th.text, sys);
    applyStates(values, "theme.item.back", th.back, sys);
    if (auto n = number(values, "theme.item.opacity"))
        th.itemOpacity = std::clamp(int(*n), 0, 100);
    if (auto n = number(values, "theme.item.radius"))
        th.itemRadius = radiusPx(int(*n));
    applyBox(values, "theme.item.padding", th.itemPadding);
    applyBox(values, "theme.item.margin", th.itemMargin);

    // theme.image
    if (auto e = boolean(values, "theme.image.enabled"))
        th.imageEnabled = *e;
    if (th.imageEnabled) {
        if (auto n = number(values, "theme.image.gap"))
            th.imageGap = std::max(0, int(*n));
        if (auto n = number(values, "theme.image.size"))
            th.imageSize = std::clamp(int(*n), 8, 64);
        if (auto v = get(values, "theme.image.color")) {
            if (v->kind == LiteralKind::Array) {
                for (size_t i = 0; i < 3 && i < v->items.size(); ++i)
                    if (auto c = colorOf(v->items[i], sys))
                        th.imageColor[i] = *c;
            } else if (auto c = colorOf(*v, sys))
                th.imageColor[0] = *c;
        }
    }

    // theme.separator
    if (auto n = number(values, "theme.separator.size"))
        th.separatorSize = std::clamp(int(*n), 0, 40);
    if (auto c = color(values, "theme.separator.color", sys))
        th.separatorColor = *c;
    else if (backgroundOpacity < 0xFF && alphaOf(th.separatorColor) == 0xFF)
        th.separatorColor = withAlpha(th.separatorColor, backgroundOpacity + 5);
    if (auto n = number(values, "theme.separator.opacity"))
        th.separatorColor = withAlpha(th.separatorColor, percentToByte(*n));
    applyBox(values, "theme.separator.margin", th.separatorMargin);

    symbolsAsText(th);
    applyStates(values, "theme.symbol", th.symbol, sys);
    applyStates(values, "theme.symbol.chevron", th.symbol, sys);

    // theme.font: a size different from the default rescales the whole menu.
    const double defaultSize = defaultFontSize(sys);
    th.fontSize = defaultSize;
    if (auto v = get(values, "theme.font.name"); v && v->kind == LiteralKind::String)
        th.fontName = std::string(nss::trim(v->string));
    if (auto n = number(values, "theme.font.size"))
        th.fontSize = std::clamp(*n, 6.0, 200.0);
    if (auto n = number(values, "theme.font.weight"); n && *n >= 1 && *n <= 9)
        th.fontWeight = int(*n) * 100;
    if (auto b = boolean(values, "theme.font.italic"))
        th.italic = *b;
    th.scale = th.fontSize / defaultSize;
    return th;
}

std::string themeNameLiteral(int index)
{
    if (index < 0 || index >= int(std::size(kThemeNames)))
        return {};
    return std::string("\"") + kThemeNames[index] + "\"";
}

int themeIndex(const nss::Literal &name, const std::string &resolvedType)
{
    std::string word;
    if (name.kind == nss::LiteralKind::String || name.kind == nss::LiteralKind::Keyword)
        word = nss::toLower(nss::trim(name.string));
    else if (name.kind == nss::LiteralKind::Number)
        word = resolvedType;
    for (int i = 0; i < int(std::size(kThemeNames)); ++i)
        if (word == kThemeNames[i])
            return i;
    return 0;
}

} // namespace theme
