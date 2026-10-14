#include "AppearanceController.h"

#include "FontEnum.h"
#include "Registry.h"
#include "Text.h"

#include "nss/Literal.h"
#include "nss/OverridesWriter.h"
#include "nss/Text.h"
#include "nss/ThemeSchema.h"

#include <shellapi.h>

#include <algorithm>
#include <cmath>

namespace fs = std::filesystem;
using nss::Literal;
using nss::LiteralKind;

namespace {

constexpr const char *kPage = "Appearance";

slint::SharedString ss(std::string_view s) { return slint::SharedString(s); }

slint::Color argb(uint32_t c) { return slint::Color::from_argb_encoded(c); }

std::string hexOf(uint32_t c, bool alpha)
{
    char buf[16];
    if (alpha && (c >> 24) != 0xFF)
        snprintf(buf, sizeof buf, "#%06X%02X", c & 0xFFFFFF, c >> 24);
    else
        snprintf(buf, sizeof buf, "#%06X", c & 0xFFFFFF);
    return buf;
}

int alphaPercent(uint32_t c) { return int(std::lround((c >> 24) * 100.0 / 255.0)); }

bool isWindows11()
{
    using RtlGetVersionFn = LONG(WINAPI *)(PRTL_OSVERSIONINFOW);
    static const bool result = [] {
        auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
        RTL_OSVERSIONINFOW v{ sizeof v };
        return fn && fn(&v) == 0 && v.dwMajorVersion >= 10 && v.dwBuildNumber >= 22000;
    }();
    return result;
}

// The preview menus. Glyphs are Segoe Fluent Icons / MDL2 code points.
struct ItemSpec
{
    const char *label;
    const char *glyph;
    int sub = -1;
    bool disabled = false;
};
constexpr ItemSpec kSep{ nullptr, nullptr };

const std::vector<ItemSpec> kDesktop = {
    { "View", "", 0 },        { "Sort by", "", 1 },       { "Refresh", "" },
    kSep,
    { "Paste", "", -1, true }, { "Undo Rename", "" },
    kSep,
    { "Terminal", "", 2 },    { "File manage", "", 3 },   { "Develop", "", 4 },
    kSep,
    { "New", "", 5 },
    kSep,
    { "Display settings", "" }, { "Personalize", "" },
};
const std::vector<std::vector<ItemSpec>> kDesktopSubs = {
    { { "Large icons", "" }, { "Medium icons", "" }, { "Small icons", "" }, kSep,
      { "Auto arrange icons", "" }, { "Show desktop icons", "" } },
    { { "Name", "" }, { "Size", "" }, { "Item type", "" }, { "Date modified", "" } },
    { { "Command prompt", "" }, { "PowerShell", "" }, { "Windows Terminal", "" } },
    { { "Copy path", "" }, { "Take ownership", "" }, { "Attributes", "", 6 }, kSep,
      { "New folder", "" }, { "New file", "" } },
    { { "Visual Studio Code", "" }, { "Git Bash", "" }, { "Open in editor", "", -1, true } },
    { { "Folder", "" }, { "Text document", "" }, { "Shortcut", "" } },
    { { "Hidden", "" }, { "Read-only", "" } },
};
const std::vector<ItemSpec> kExplorer = {
    { "Open", "" },          { "Open with", "", 0 },
    kSep,
    { "Cut", "" },           { "Copy", "" },             { "Paste", "", -1, true },
    kSep,
    { "Copy path", "" },     { "Rename", "" },           { "Delete", "" },
    kSep,
    { "File manage", "", 1 }, { "Pin to Start", "" },
    kSep,
    { "Properties", "" },
};
const std::vector<std::vector<ItemSpec>> kExplorerSubs = {
    { { "Notepad", "" }, { "Word", "" }, kSep, { "Choose another app", "" } },
    { { "Copy path", "" }, { "Take ownership", "" }, { "Attributes", "" }, kSep,
      { "New folder", "" }, { "New file", "" } },
};

struct Metrics
{
    float scale = 1, top = 0, bottom = 0, row = 0, sep = 0;
};

Metrics metricsOf(const theme::Resolved &r)
{
    Metrics m;
    m.scale = float(r.scale);
    const float s = m.scale;
    m.top = float(r.borderPadding.t + r.borderSize) * s;
    m.bottom = float(r.borderPadding.b + r.borderSize) * s;
    float content = std::max(float(r.fontSize), r.imageEnabled ? float(r.imageSize) * s : 0.f);
    m.row = std::round(float(r.itemMargin.t + r.itemMargin.b + r.itemPadding.t + r.itemPadding.b) * s + content);
    m.sep = std::round(float(r.separatorMargin.t + r.separatorSize + r.separatorMargin.b) * s);
    return m;
}

PreviewMenu buildMenu(const std::vector<ItemSpec> &specs, const Metrics &m, int open)
{
    auto items = std::make_shared<slint::VectorModel<PreviewItem>>();
    float y = m.top;
    for (const auto &spec : specs) {
        PreviewItem it{};
        it.separator = spec.label == nullptr;
        it.label = it.separator ? "" : spec.label;
        it.glyph = spec.glyph ? spec.glyph : "";
        it.disabled = spec.disabled;
        it.sub = spec.sub;
        it.y = y;
        it.h = it.separator ? m.sep : m.row;
        y += it.h;
        items->push_back(it);
    }
    PreviewMenu menu{};
    menu.items = items;
    menu.height = y + m.bottom;
    menu.open = open;
    return menu;
}

std::shared_ptr<slint::VectorModel<PreviewMenu>> buildSubs(const std::vector<std::vector<ItemSpec>> &subs,
                                                           const Metrics &m)
{
    auto out = std::make_shared<slint::VectorModel<PreviewMenu>>();
    for (const auto &s : subs)
        out->push_back(buildMenu(s, m, -1));
    return out;
}

PreviewTheme toPreview(const theme::Resolved &r)
{
    const float s = float(r.scale);
    PreviewTheme p{};
    p.dark = r.dark;
    p.effect = r.effect;
    p.background = argb(r.background);
    p.tint = argb(r.tint);
    p.acrylic = argb(theme::acrylicLayer(r));
    p.gradient = r.gradient;
    p.gradient_radial = r.gradientRadial;
    p.gradient_angle = float(r.gradientAngle);
    p.gradient_0 = argb(theme::gradientAt(r, 0.0));
    p.gradient_1 = argb(theme::gradientAt(r, 0.5));
    p.gradient_2 = argb(theme::gradientAt(r, 1.0));
    p.border_size = float(r.borderSize) * s;
    p.border_color = argb(r.borderColor);
    p.radius = float(r.borderRadius) * s;
    p.pad_top = float(r.borderPadding.t + r.borderSize) * s;
    p.pad_bottom = float(r.borderPadding.b + r.borderSize) * s;
    p.pad_left = float(r.borderPadding.l + r.borderSize) * s;
    p.pad_right = float(r.borderPadding.r + r.borderSize) * s;
    p.shadow = r.shadow;
    p.shadow_size = float(r.shadowSize) * s;
    p.shadow_offset = float(r.shadowOffset) * s;
    p.shadow_color = argb(r.shadowColor);
    p.text = argb(r.text.nor);
    p.text_sel = argb(r.text.sel);
    p.text_dis = argb(r.text.norDis);
    p.back_sel = argb(r.back.sel);
    p.back_dis_sel = argb(r.back.selDis);
    p.item_radius = float(r.itemRadius) * s;
    p.item_pad_left = float(r.itemPadding.l) * s;
    p.item_pad_right = float(r.itemPadding.r) * s;
    p.item_mar_left = float(r.itemMargin.l) * s;
    p.item_mar_right = float(r.itemMargin.r) * s;
    p.item_mar_top = float(r.itemMargin.t) * s;
    p.item_mar_bottom = float(r.itemMargin.b) * s;
    p.sep_size = float(r.separatorSize) * s;
    p.sep_color = argb(r.separatorColor);
    p.sep_mar_left = float(r.separatorMargin.l) * s;
    p.sep_mar_right = float(r.separatorMargin.r) * s;
    p.sep_mar_top = float(r.separatorMargin.t) * s;
    p.font_family = ss(r.fontName);
    p.font_size = float(r.fontSize) * 0.8f;
    p.font_weight = r.fontWeight;
    p.italic = r.italic;
    p.show_images = r.imageEnabled;
    p.image_size = float(r.imageSize) * s;
    p.image_gap = float(r.imageGap) * s;
    p.icon = argb(r.imageColor[0]);
    p.icon_sel = argb(r.text.sel != r.text.nor ? r.text.sel : r.imageColor[0]);
    p.chevron = argb(r.symbol.nor);
    p.chevron_sel = argb(r.symbol.sel);
    p.min_width = 160.f * s;
    return p;
}

slint::Image toImage(const wallpaper::Pixels &px)
{
    if (px.empty())
        return {};
    slint::SharedPixelBuffer<slint::Rgba8Pixel> buf(px.width, px.height,
                                                    reinterpret_cast<const slint::Rgba8Pixel *>(px.rgba.data()));
    return slint::Image(buf);
}

std::vector<std::pair<std::string, std::string>> devSets(bool &saveRequested)
{
    std::vector<std::pair<std::string, std::string>> out;
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv)
        return out;
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--set") == 0 && i + 1 < argc) {
            auto kv = text::narrow(argv[++i]);
            if (auto eq = kv.find('='); eq != std::string::npos)
                out.emplace_back(kv.substr(0, eq), kv.substr(eq + 1));
        } else if (_wcsicmp(argv[i], L"--save") == 0)
            saveRequested = true;
    }
    LocalFree(argv);
    return out;
}

} // namespace

const std::vector<AppearanceController::FieldDef> &AppearanceController::fieldDefs()
{
    // The order matches the K global in ui/pages/appearance-state.slint.
    static const std::vector<FieldDef> defs = {
        { "theme.name", Kind::Enum, { "\"auto\"", "\"modern\"", "\"classic\"", "\"white\"", "\"black\"" } },
        { "theme.dark", Kind::Enum, { "auto", "true", "false" } },
        { "theme.view", Kind::Enum, { "auto", "view.compact", "view.small", "view.medium", "view.large", "view.wide" } },
        { "theme.background.color", Kind::Color, {} },
        { "theme.background.opacity", Kind::Number, {} },
        { "theme.background.effect", Kind::Enum, { "auto", "0", "1", "2", "3" } },
        { "theme.background.tintcolor", Kind::Color, {} },
        { "theme.font.name", Kind::Font, {} },
        { "theme.font.size", Kind::Number, {} },
        { "theme.font.weight", Kind::Number, {} },
        { "theme.border.radius", Kind::Step, {} },
        { "theme.item.radius", Kind::Step, {} },
        { "theme.border.size", Kind::Number, {} },
        { "theme.border.color", Kind::Color, {} },
        { "theme.shadow.enabled", Kind::Bool, {} },
        { "theme.shadow.size", Kind::Number, {} },
        { "theme.shadow.offset", Kind::Number, {} },
        { "theme.shadow.opacity", Kind::Number, {} },
        { "theme.item.padding", Kind::Box, {} },
        { "theme.item.margin", Kind::Box, {} },
        { "theme.separator.size", Kind::Number, {} },
        { "theme.separator.color", Kind::Color, {} },
        { "theme.separator.margin", Kind::Box, {} },
        { "theme.item.text.normal", Kind::Color, {} },
        { "theme.item.text.select", Kind::Color, {} },
        { "theme.item.text.normal.disabled", Kind::Color, {} },
        { "theme.item.back.select", Kind::Color, {} },
        { "theme.item.back.select.disabled", Kind::Color, {} },
        { "theme.image.size", Kind::Number, {} },
        { "theme.image.gap", Kind::Number, {} },
    };
    return defs;
}

AppearanceController::AppearanceController(slint::ComponentHandle<AppWindow> ui, SaveService &save)
    : ui_(std::move(ui)), save_(save), fields_(std::make_shared<slint::VectorModel<ThemeField>>()),
      presets_(ui_, save, [](const theme::Resolved &r) { return toPreview(r); })
{
    previewDark_ = !registry::appsUseLightTheme();
    system_ = systemInfo(previewDark_);
    const auto &st = ui_->global<AppearanceState>();
    st.set_preview_dark(previewDark_);
    st.set_fields(fields_);
    bindCallbacks();
    save_.onChanged([this] { rebuild(); });
    save_.onConfig([this] { configLoaded(); });
    rebuild(); // defaults until the config is read
}

theme::SystemInfo AppearanceController::systemInfo(bool dark) const
{
    theme::SystemInfo sys;
    sys.appsDark = dark;
    auto transparency = registry::readDword(
        HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"EnableTransparency");
    sys.transparency = !transparency || *transparency != 0;
    if (auto accent = registry::accentColor())
        sys.accent = 0xFF000000u | *accent;
    sys.win11 = isWindows11();
    return sys;
}

void AppearanceController::bindCallbacks()
{
    const auto &st = ui_->global<AppearanceState>();
    st.on_set_text([this](int i, slint::SharedString value) {
        if (i < 0 || i >= int(fieldDefs().size()))
            return;
        std::string v(nss::trim(std::string_view(value.data(), value.size())));
        const auto &def = fieldDefs()[size_t(i)];
        if (def.kind == Kind::Font)
            v = v.empty() ? std::string{} : nss::quoteString(v);
        else if (def.kind == Kind::Color && !v.empty() && v.front() != '#' && v.find('.') == std::string::npos)
            v = "#" + v;
        if (v.empty()) {
            reset(i);
            return;
        }
        apply(i, v);
    });
    st.on_set_number([this](int i, float value) {
        if (i < 0 || i >= int(fieldDefs().size()))
            return;
        const auto &def = fieldDefs()[size_t(i)];
        if (def.kind == Kind::Bool)
            apply(i, value > 0.5f ? "true" : "false");
        else
            apply(i, nss::formatNumber(std::round(double(value))));
    });
    st.on_set_box([this](int i, float h, float v) {
        apply(i, "[" + nss::formatNumber(std::round(double(h))) + ", " + nss::formatNumber(std::round(double(v))) + "]");
    });
    st.on_set_color([this](int i, slint::Color c) {
        if (i < 0 || i >= int(fieldDefs().size()))
            return;
        uint32_t rgba = (uint32_t(c.red()) << 24) | (uint32_t(c.green()) << 16) | (uint32_t(c.blue()) << 8) | c.alpha();
        apply(i, Literal::makeColor(rgba).text);
    });
    st.on_set_index([this](int i, int choice) {
        if (i < 0 || i >= int(fieldDefs().size()))
            return;
        const auto &def = fieldDefs()[size_t(i)];
        if (std::string_view(def.key) == "theme.name") {
            // The cards, shared with the first-run dialog.
            if (auto literal = theme::themeNameLiteral(choice); !literal.empty())
                apply(i, literal);
        } else if (std::string_view(def.key) == "theme.background.effect") {
            selectEffect(i, choice);
        } else if (choice >= 0 && choice < int(def.choices.size()))
            apply(i, def.choices[size_t(choice)]);
    });
    st.on_reset([this](int i) { reset(i); });
    st.on_set_preview_dark([this](bool dark) {
        previewDark_ = dark;
        system_ = systemInfo(dark);
        ui_->global<AppearanceState>().set_preview_dark(dark);
        rebuild();
    });
    st.on_filter_fonts([this](slint::SharedString q) {
        auto list = fonts::filter(fonts_, std::string_view(q.data(), q.size()));
        auto model = std::make_shared<slint::VectorModel<slint::SharedString>>();
        for (const auto &f : list)
            model->push_back(ss(f));
        ui_->global<AppearanceState>().set_fonts(model);
    });
    st.on_reload([this] { save_.reloadConfig(); });
}

void AppearanceController::start()
{
    // Wallpaper and fonts: once, off the UI thread.
    save_.worker().post([this] {
        auto images = std::make_shared<wallpaper::Images>(wallpaper::load());
        auto families = std::make_shared<std::vector<std::string>>(fonts::families());
        slint::invoke_from_event_loop([this, images, families] {
            fonts_ = std::move(*families);
            const auto &st = ui_->global<AppearanceState>();
            st.set_wallpaper(toImage(images->sharp));
            st.set_wallpaper_blurred(toImage(images->blurred));
            st.set_icon_font(fonts::installed(fonts_, "Segoe Fluent Icons") ? "Segoe Fluent Icons" : "Segoe MDL2 Assets");
            st.invoke_filter_fonts("");
        });
    });
    save_.reloadConfig();
}

void AppearanceController::configLoaded()
{
    const auto &st = ui_->global<AppearanceState>();
    if (!save_.graph()) {
        st.set_load_error(ss(save_.target().error));
        st.set_loaded(false);
        return;
    }
    st.set_load_error("");
    st.set_loaded(true);
    errors_.clear();
    rebuild();
    applyDevArgs();
}

theme::Values AppearanceController::currentValues() const
{
    theme::Values values;
    auto add = [&](const std::string &key) {
        if (values.count(key))
            return;
        if (auto text = save_.current(key)) {
            auto lit = Literal::parse(*text);
            if (lit.isLiteral())
                values[key] = std::move(lit);
        }
    };
    if (const auto *graph = save_.graph())
        for (const auto &[key, ev] : graph->values())
            add(key);
    for (const auto &[key, change] : save_.pending().items())
        add(key);
    return values;
}

void AppearanceController::apply(int index, const std::string &literal)
{
    const auto &def = fieldDefs()[size_t(index)];
    auto errors = save_.set(def.key, literal, kPage);
    if (!errors.empty()) {
        errors_[index] = errors.front();
        rebuild();
        return;
    }
    if (errors_.erase(index))
        rebuild(); // save_.set already rebuilt; refresh the cleared error
}

void AppearanceController::selectEffect(int index, int choice)
{
    const std::string opacityKey = "theme.background.opacity";
    const auto values = currentValues();
    auto find = [&](const char *key) -> const Literal * {
        auto it = values.find(key);
        return it == values.end() ? nullptr : &it->second;
    };
    const auto pick = theme::effectChoice(choice, find("theme.background.effect"), find(opacityKey.c_str()));
    if (pick.effect.empty())
        return;
    apply(index, pick.effect);
    if (!pick.clearOpacity)
        return;
    // A solid background (opacity 100) hides any effect. Drop this app's value, or,
    // when the user's own file has the solid opacity, put auto over it.
    const auto &defs = fieldDefs();
    const int opacity = int(std::find_if(defs.begin(), defs.end(), [&](const FieldDef &d) { return d.key == opacityKey; }) -
                            defs.begin());
    reset(opacity);
    const auto o = save_.origins(opacityKey);
    const auto own = o.file ? Literal::parse(o.file->value) : Literal{};
    if (own.kind == LiteralKind::Number && own.number >= 100)
        apply(opacity, "auto");
}

void AppearanceController::reset(int index)
{
    if (index < 0 || index >= int(fieldDefs().size()))
        return;
    const std::string key = fieldDefs()[size_t(index)].key;
    errors_.erase(index);
    if (save_.origins(key).app)
        save_.reset(key, kPage);  // drop the saved override on the next save
    else
        save_.forget(key);        // only a pending change: just forget it
}

void AppearanceController::rebuild()
{
    const auto values = currentValues();
    const auto r = theme::resolve(values, system_);
    const auto &defs = fieldDefs();

    auto lit = [&](const std::string &key) -> std::optional<Literal> {
        auto it = values.find(key);
        if (it == values.end())
            return std::nullopt;
        return it->second;
    };
    auto num = [&](const std::string &key, double fallback) {
        auto l = lit(key);
        return l && l->kind == LiteralKind::Number ? l->number : fallback;
    };

    std::vector<ThemeField> rows;
    rows.reserve(defs.size());
    for (size_t i = 0; i < defs.size(); ++i) {
        const auto &def = defs[i];
        const std::string key = def.key;
        ThemeField f{};
        f.key = ss(key);
        auto o = save_.origins(key);
        const auto *pending = save_.pending().find(key);
        auto text = save_.current(key);
        f.text = ss(text.value_or(""));
        f.pending = pending != nullptr;
        f.overridden = (pending && pending->literal) || (!pending && o.app);
        // An expression in the user's files, not overridden here: read-only.
        f.locked = !f.overridden && o.file && !o.file->literal;
        if (auto e = errors_.find(int(i)); e != errors_.end())
            f.error = ss(e->second);

        f.origin = ss(save_.originText(key));
        f.file_value = ss(o.file ? o.file->value : std::string{});
        if (o.file) {
            f.source_file = ss(text::narrow(o.file->file.wstring()));
            f.source_line = int(o.file->line);
        }

        auto l = lit(key);
        switch (def.kind) {
        case Kind::Enum: {
            int index = 0;
            if (l) {
                if (key == "theme.name") {
                    index = theme::themeIndex(*l, r.type);
                } else if (key == "theme.dark") {
                    if (l->isBool())
                        index = l->boolValue() ? 1 : 2;
                } else if (key == "theme.view") {
                    if (auto pad = theme::viewPadding(*l)) {
                        static const int pads[] = { 0, 2, 4, 8, 10 };
                        for (int k = 0; k < 5; ++k)
                            if (*pad == pads[k])
                                index = k + 1;
                    }
                } else if (key == "theme.background.effect") {
                    const Literal *e = l->kind == LiteralKind::Array && !l->items.empty() ? &l->items[0] : &*l;
                    if (e->kind == LiteralKind::Number && e->number >= 0 && e->number <= 3)
                        index = int(e->number) + 1;
                    else if (e->kind == LiteralKind::String || e->kind == LiteralKind::Keyword) {
                        auto w = nss::toLower(e->string);
                        index = w == "none" ? 1 : w == "transparent" ? 2 : w == "blur" ? 3 : w == "acrylic" ? 4 : 0;
                    }
                }
            }
            f.index = index;
            break;
        }
        case Kind::Color: {
            uint32_t c = 0;
            if (key == "theme.background.color")
                c = r.background | 0xFF000000u;
            else if (key == "theme.background.tintcolor")
                c = r.tint;
            else if (key == "theme.border.color")
                c = r.borderColor;
            else if (key == "theme.separator.color")
                c = r.separatorColor;
            else if (key == "theme.item.text.normal")
                c = r.text.nor;
            else if (key == "theme.item.text.select")
                c = r.text.sel;
            else if (key == "theme.item.text.normal.disabled")
                c = r.text.norDis;
            else if (key == "theme.item.back.select")
                c = r.back.sel;
            else if (key == "theme.item.back.select.disabled")
                c = r.back.selDis;
            if (l)
                if (auto own = theme::colorOf(*l, system_))
                    c = *own;
            f.color = argb(c);
            f.display = ss(l && l->kind == LiteralKind::Color ? l->text : hexOf(c, key != "theme.background.color"));
            break;
        }
        case Kind::Number: {
            double v = 0;
            // auto/default (or, for the background opacity, nothing at all): the core
            // picks the value, so the slider shows it as "Auto (≈N)" and writes nothing
            // until it is moved.
            const bool keyword = l && (l->kind == LiteralKind::Keyword || l->kind == LiteralKind::String);
            f.is_auto = keyword || (!l && key == "theme.background.opacity");
            if (key == "theme.background.opacity")
                v = theme::backgroundOpacityPercent(r);
            else if (key == "theme.font.size")
                v = r.fontSize;
            else if (key == "theme.font.weight")
                v = r.fontWeight / 100.0;
            else if (key == "theme.border.size")
                v = num(key, r.borderSize);
            else if (key == "theme.shadow.size")
                v = num(key, r.shadowSize);
            else if (key == "theme.shadow.offset")
                v = num(key, r.shadowOffset);
            else if (key == "theme.shadow.opacity")
                v = num(key, alphaPercent(r.shadowColor));
            else if (key == "theme.separator.size")
                v = r.separatorSize;
            else if (key == "theme.image.size")
                v = r.imageSize;
            else if (key == "theme.image.gap")
                v = r.imageGap;
            f.number = float(v);
            break;
        }
        case Kind::Step:
            f.number = float(std::clamp(num(key, (key == "theme.border.radius" ? r.borderRadius : r.itemRadius) / 4.0), 0.0, 4.0));
            break;
        case Kind::Bool:
            f.number = (l && l->isBool()) ? (l->boolValue() ? 1.f : 0.f) : (r.shadow ? 1.f : 0.f);
            break;
        case Kind::Box: {
            const theme::Box &b = key == "theme.item.padding" ? r.itemPadding
                                  : key == "theme.item.margin" ? r.itemMargin
                                                               : r.separatorMargin;
            f.number = float(b.l);
            f.number2 = float(b.t);
            break;
        }
        case Kind::Font:
            f.display = ss(l && l->kind == LiteralKind::String ? l->string : std::string{});
            break;
        }
        rows.push_back(f);
    }

    if (fields_->row_count() != rows.size()) {
        fields_ = std::make_shared<slint::VectorModel<ThemeField>>(rows);
        ui_->global<AppearanceState>().set_fields(fields_);
    } else {
        for (size_t i = 0; i < rows.size(); ++i)
            fields_->set_row_data(i, rows[i]);
    }

    // What the Effects controls mean in the core (ContextMenu.cpp; docs/settings-app/DESIGN.md).
    const auto &st = ui_->global<AppearanceState>();
    {
        const auto effectLit = lit("theme.background.effect");
        const Literal *first = effectLit && effectLit->kind == LiteralKind::Array && !effectLit->items.empty()
                                   ? &effectLit->items[0]
                                   : effectLit ? &*effectLit : nullptr;
        const bool forcedAuto = first && (first->kind == LiteralKind::Keyword || first->kind == LiteralKind::String) &&
                                nss::toLower(first->string) == "auto";
        const int asked = first && first->kind == LiteralKind::Number ? int(first->number) : -1;
        const bool arrayTint = effectLit && effectLit->kind == LiteralKind::Array && effectLit->items.size() >= 2;
        std::string effectNote;
        if (!system_.transparency && (asked == 2 || asked == 3))
            effectNote = "Windows Transparency effects is off, so Shell draws this menu solid. Turn it on in "
                         "Personalization > Colors, or choose Auto, which Shell applies even with it off.";
        else if (forcedAuto)
            effectNote = system_.win11 ? "Auto is acrylic on Windows 11, even with Windows Transparency effects off."
                                       : "Auto is blur on Windows 10, even with Windows Transparency effects off.";
        else if (!first)
            effectNote = "Not set: acrylic while Windows Transparency effects is on, none while it is off.";
        else if (asked == 3)
            effectNote = "The blurred desktop with the tint over it, as Auto gives on Windows 11. For a stronger "
                         "look, raise the tint's alpha.";
        st.set_effect_note(ss(effectNote));
        st.set_tint_note(ss(std::string("Its alpha is the strength: 40 is light, FF hides the blur. ") +
                            (arrayTint && !lit("theme.background.tintcolor")
                                 ? "Now from effect = [n, tint, opacity]; a change here is saved as background.tintcolor, which wins."
                                 : "Saved as background.tintcolor.")));
        st.set_opacity_note(ss(r.effect == 0
                                   ? std::string("No effect: the menu is solid, so opacity doesn't apply.")
                                   : std::string("The background colour over the effect: 0 % shows only the effect "
                                                 "and its tint, 100 % is solid and turns the effect off.")));
    }

    // Preview
    st.set_preview(toPreview(r));
    auto m = metricsOf(r);
    st.set_menu_desktop(buildMenu(kDesktop, m, 8));
    st.set_subs_desktop(buildSubs(kDesktopSubs, m));
    st.set_menu_explorer(buildMenu(kExplorer, m, 11));
    st.set_subs_explorer(buildSubs(kExplorerSubs, m));

    auto cards = std::make_shared<slint::VectorModel<PreviewTheme>>();
    for (const char *type : { "auto", "modern", "classic", "white", "black" }) {
        auto v = values;
        v["theme.name"] = Literal::makeString(type);
        cards->push_back(toPreview(theme::resolve(v, system_)));
    }
    st.set_card_themes(cards);

    presets_.rebuild(values, system_);
}

void AppearanceController::applyDevArgs()
{
    if (devApplied_)
        return;
    devApplied_ = true;
    bool saveRequested = false;
    auto sets = devSets(saveRequested);
    for (const auto &[key, value] : sets) {
        auto errors = save_.set(key, value, kPage);
        if (!errors.empty())
            OutputDebugStringA(("settings --set: " + errors.front() + "\n").c_str());
    }
    if (saveRequested && !sets.empty())
        save_.save();
}
