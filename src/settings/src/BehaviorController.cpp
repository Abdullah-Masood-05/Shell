#include "BehaviorController.h"

#include "ExcludeRules.h"
#include "Registry.h"
#include "Text.h"
#include "ThemeModel.h"

#include "nss/Literal.h"
#include "nss/Text.h"

#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <cmath>

using nss::Literal;
using nss::LiteralKind;

namespace {

constexpr const char *kBehavior = "Behavior";
constexpr const char *kRules = "Rules";

slint::SharedString ss(std::string_view s) { return slint::SharedString(s); }
std::string str(const slint::SharedString &s) { return std::string(s.data(), s.size()); }
slint::Color argb(uint32_t c) { return slint::Color::from_argb_encoded(c); }

std::string hexOf(const slint::Color &c)
{
    char buf[16];
    if (c.alpha() != 255)
        snprintf(buf, sizeof buf, "#%02X%02X%02X%02X", c.red(), c.green(), c.blue(), c.alpha());
    else
        snprintf(buf, sizeof buf, "#%02X%02X%02X", c.red(), c.green(), c.blue());
    return buf;
}

std::string knownFolder(const KNOWNFOLDERID &id)
{
    PWSTR raw = nullptr;
    std::string out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &raw)) && raw)
        out = text::narrow(raw);
    CoTaskMemFree(raw);
    return out;
}

// A string literal for a path: plain double quotes, backslashes escaped.
std::string quotePath(std::string_view path) { return nss::quoteString(path); }

} // namespace

const std::vector<BehaviorController::FieldDef> &BehaviorController::fieldDefs()
{
    static const std::vector<FieldDef> defs = {
        { "settings.priority", Kind::Bool, {} },
        { "settings.showdelay", Kind::Number, {} },
        { "settings.tip.enabled", Kind::Bool, {} },
        { "settings.tip.time", Kind::Seconds, {} },
        { "settings.tip.width", Kind::Number, {} },
        { "settings.tip.default", Kind::Pair, {} },
        { "settings.screenshot.enabled", Kind::Bool, {} },
        { "settings.screenshot.directory", Kind::Folder, {} },
        { "settings.modify.remove.duplicate", Kind::Bool, {} },
        { "settings.modify.remove.disabled", Kind::Bool, {} },
        { "settings.modify.remove.separator", Kind::Bool, {} },
        { "settings.modify.image", Kind::Enum, { "0", "1", "2" } },
        { "settings.new.image", Kind::Bool, {} },
        { "settings.exclude.where", Kind::Where, {} },
        { "settings.exclude.process", Kind::Names, {} },
        { "settings.exclude.window", Kind::Names, {} },
    };
    return defs;
}

BehaviorController::BehaviorController(slint::ComponentHandle<AppWindow> ui, SaveService &save)
    : ui_(std::move(ui)), save_(save), fields_(std::make_shared<slint::VectorModel<ThemeField>>())
{
    const auto &st = ui_->global<SettingsState>();
    st.set_fields(fields_);
    UINT delay = 400;
    if (SystemParametersInfoW(SPI_GETMENUSHOWDELAY, 0, &delay, 0))
        st.set_windows_showdelay(int(delay));
    st.set_default_screenshot_dir(ss(knownFolder(FOLDERID_Screenshots)));
    bindCallbacks();
    save_.onChanged([this] { rebuild(); });
    save_.onConfig([this] { configLoaded(); });
    rebuild();
}

void BehaviorController::bindCallbacks()
{
    const auto &st = ui_->global<SettingsState>();
    auto valid = [](int i) { return i >= 0 && i < int(fieldDefs().size()); };

    st.on_set_bool([this, valid](int i, bool on) {
        if (valid(i))
            apply(i, on ? "true" : "false");
    });
    st.on_set_number([this, valid](int i, float value) {
        if (!valid(i))
            return;
        if (fieldDefs()[size_t(i)].kind == Kind::Seconds)
            apply(i, nss::formatNumber(std::round(double(value) * 10) / 10));
        else
            apply(i, nss::formatNumber(std::round(double(value))));
    });
    st.on_set_text([this, valid](int i, slint::SharedString value) {
        if (!valid(i))
            return;
        auto v = std::string(nss::trim(str(value)));
        if (v.empty())
            reset(i);
        else
            apply(i, v);
    });
    st.on_set_string([this, valid](int i, slint::SharedString value) {
        if (!valid(i))
            return;
        auto v = std::string(nss::trim(str(value)));
        if (v.empty())
            reset(i);
        else
            apply(i, quotePath(v));
    });
    st.on_set_pair([this, valid](int i, slint::Color first, slint::Color second) {
        if (valid(i))
            apply(i, "[" + hexOf(first) + ", " + hexOf(second) + "]");
    });
    st.on_set_index([this, valid](int i, int choice) {
        if (!valid(i))
            return;
        const auto &def = fieldDefs()[size_t(i)];
        if (choice >= 0 && choice < int(def.choices.size()))
            apply(i, def.choices[size_t(choice)]);
    });
    st.on_reset([this, valid](int i) {
        if (valid(i))
            reset(i);
    });
    st.on_pick_folder([this, valid](int i) {
        if (valid(i))
            pickFolder(i);
    });
    st.on_add_name([this, valid](int i, slint::SharedString value) {
        if (!valid(i) || fieldDefs()[size_t(i)].kind != Kind::Names)
            return false;
        const bool process = std::string_view(fieldDefs()[size_t(i)].key) == "settings.exclude.process";
        auto name = process ? rules::processName(str(value)) : rules::windowClass(str(value));
        auto list = names(i);
        if (!rules::addName(list, name))
            return false;
        apply(i, rules::formatNames(list));
        return true;
    });
    st.on_remove_name([this, valid](int i, int position) {
        if (!valid(i))
            return;
        auto list = names(i);
        if (position < 0 || position >= int(list.size()))
            return;
        list.erase(list.begin() + position);
        const auto key = std::string(fieldDefs()[size_t(i)].key);
        // Nothing left and nothing in the user's files: drop the key instead of writing "".
        if (list.empty() && !save_.origins(key).file)
            reset(i);
        else
            apply(i, rules::formatNames(list));
    });
    st.on_set_explorer_only([this](bool on) {
        const int i = ui_->global<SK>().get_where();
        const std::string key = fieldDefs()[size_t(i)].key;
        if (on) {
            apply(i, std::string(rules::kExplorerOnly));
            return;
        }
        // Off: the user's files decide when they have no condition; else override with false.
        auto o = save_.origins(key);
        if (!o.file || rules::classifyWhere(o.file->value) == rules::Where::Off)
            reset(i);
        else
            apply(i, "false");
    });
    st.on_reload([this] { save_.reloadConfig(); });
}

void BehaviorController::configLoaded()
{
    const auto &st = ui_->global<SettingsState>();
    st.set_load_error(ss(save_.graph() ? std::string{} : save_.target().error));
    st.set_loaded(save_.graph() != nullptr);
    errors_.clear();
    rebuild();
}

std::vector<std::string> BehaviorController::names(int index) const
{
    if (auto text = save_.current(fieldDefs()[size_t(index)].key))
        if (auto list = rules::parseNames(*text))
            return *list;
    return {};
}

void BehaviorController::apply(int index, const std::string &literal)
{
    const auto &def = fieldDefs()[size_t(index)];
    const bool rules = def.kind == Kind::Where || def.kind == Kind::Names;
    auto errors = save_.set(def.key, literal, rules ? kRules : kBehavior);
    if (!errors.empty()) {
        errors_[index] = errors.front();
        rebuild();
        return;
    }
    if (errors_.erase(index))
        rebuild();
}

void BehaviorController::reset(int index)
{
    const auto &def = fieldDefs()[size_t(index)];
    const bool rules = def.kind == Kind::Where || def.kind == Kind::Names;
    errors_.erase(index);
    if (save_.origins(def.key).app)
        save_.reset(def.key, rules ? kRules : kBehavior);
    else
        save_.forget(def.key);
}

void BehaviorController::pickFolder(int index)
{
    IFileOpenDialog *dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
        return;
    DWORD flags = 0;
    dlg->GetOptions(&flags);
    dlg->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(L"Folder for menu screenshots");
    // Start in the folder in effect.
    std::string start = knownFolder(FOLDERID_Screenshots);
    if (auto text = save_.current(fieldDefs()[size_t(index)].key)) {
        auto lit = Literal::parse(*text);
        if (lit.kind == LiteralKind::String && !lit.string.empty())
            start = lit.string;
    }
    IShellItem *folder = nullptr;
    if (!start.empty() && SUCCEEDED(SHCreateItemFromParsingName(text::widen(start).c_str(), nullptr,
                                                                IID_PPV_ARGS(&folder)))) {
        dlg->SetFolder(folder);
        folder->Release();
    }
    HWND owner = ui_->window().win32_hwnd();
    std::string picked;
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem *item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                picked = text::narrow(path);
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
    if (!picked.empty())
        apply(index, quotePath(picked));
}

void BehaviorController::rebuild()
{
    const auto &defs = fieldDefs();

    // The tip's default colours are the menu's text and background.
    theme::Values themeValues;
    if (const auto *graph = save_.graph())
        for (const auto &[key, ev] : graph->values())
            if (key.rfind("theme.", 0) == 0)
                if (auto text = save_.current(key)) {
                    auto lit = Literal::parse(*text);
                    if (lit.isLiteral())
                        themeValues[key] = std::move(lit);
                }
    theme::SystemInfo sys;
    sys.appsDark = !registry::appsUseLightTheme();
    if (auto accent = registry::accentColor())
        sys.accent = 0xFF000000u | *accent;
    const auto menu = theme::resolve(themeValues, sys);

    std::vector<ThemeField> rows;
    rows.reserve(defs.size());
    int whereKind = 0;
    std::vector<std::string> processes, windows;
    for (size_t i = 0; i < defs.size(); ++i) {
        const auto &def = defs[i];
        const std::string key = def.key;
        ThemeField f{};
        f.key = ss(key);
        auto o = save_.origins(key);
        const auto *pending = save_.pending().find(key);
        auto text = save_.current(key);
        auto lit = text ? Literal::parse(*text) : Literal{};
        const bool has = text.has_value();
        f.text = ss(text.value_or(""));
        f.pending = pending != nullptr;
        f.overridden = (pending && pending->literal) || (!pending && o.app);
        f.locked = !f.overridden && o.file && !o.file->literal;
        f.origin = ss(save_.originText(key));
        f.file_value = ss(o.file ? o.file->value : std::string{});
        if (o.file) {
            f.source_file = ss(text::narrow(o.file->file.wstring()));
            f.source_line = int(o.file->line);
        }
        if (auto e = errors_.find(int(i)); e != errors_.end())
            f.error = ss(e->second);

        switch (def.kind) {
        case Kind::Bool: {
            // Built-in defaults (ContextMenu.h, Main.cpp): priority on, new.image on, the rest off.
            bool on = key == "settings.priority" || key == "settings.new.image";
            if (has && (lit.isBool() || lit.kind == LiteralKind::Number))
                on = lit.boolValue();
            f.number = on ? 1.f : 0.f;
            break;
        }
        case Kind::Number: {
            double v = key == "settings.tip.width" ? 640 : ui_->global<SettingsState>().get_windows_showdelay();
            if (has && lit.kind == LiteralKind::Number)
                v = lit.number;
            f.number = float(v);
            f.index = has ? 1 : 0; // showdelay: 1 when the file or this app sets it
            break;
        }
        case Kind::Seconds:
            f.number = has && lit.kind == LiteralKind::Number ? float(lit.number) : 1.f;
            break;
        case Kind::Pair: {
            uint32_t back = (menu.background & 0x00FFFFFFu) | 0xFF000000u, fore = menu.text.nor;
            if (has && lit.kind == LiteralKind::Array && lit.items.size() >= 2) {
                if (auto c = theme::colorOf(lit.items[0], sys))
                    back = *c;
                if (auto c = theme::colorOf(lit.items[1], sys))
                    fore = *c;
            }
            f.color = argb(back);
            f.color2 = argb(fore);
            f.display = ss(hexOf(f.color));
            f.display2 = ss(hexOf(f.color2));
            break;
        }
        case Kind::Folder:
            f.display = ss(has && lit.kind == LiteralKind::String ? lit.string : std::string{});
            break;
        case Kind::Enum: {
            int index = 1; // modify.image defaults to 1
            if (has && lit.kind == LiteralKind::Number)
                index = std::clamp(int(lit.number), 0, 2);
            else if (has && lit.isBool())
                index = lit.boolValue() ? 1 : 0;
            f.index = index;
            break;
        }
        case Kind::Where: {
            auto kind = has ? rules::classifyWhere(*text) : rules::Where::Off;
            whereKind = kind == rules::Where::Off ? 0 : kind == rules::Where::ExplorerOnly ? 1 : 2;
            f.index = whereKind;
            f.display = ss(kind == rules::Where::Custom ? *text : std::string{});
            // A custom condition from the files is shown, not locked: the raw box edits it.
            f.locked = false;
            break;
        }
        case Kind::Names: {
            auto list = has ? rules::parseNames(*text) : std::optional<std::vector<std::string>>(std::vector<std::string>{});
            f.locked = !list.has_value();
            f.display = ss(list ? std::string{} : *text);
            (key == "settings.exclude.process" ? processes : windows) = list.value_or(std::vector<std::string>{});
            break;
        }
        }
        rows.push_back(f);
    }

    if (fields_->row_count() != rows.size()) {
        fields_ = std::make_shared<slint::VectorModel<ThemeField>>(rows);
        ui_->global<SettingsState>().set_fields(fields_);
    } else {
        for (size_t i = 0; i < rows.size(); ++i)
            fields_->set_row_data(i, rows[i]);
    }

    auto toModel = [](const std::vector<std::string> &list) {
        auto m = std::make_shared<slint::VectorModel<slint::SharedString>>();
        for (const auto &s : list)
            m->push_back(ss(s));
        return m;
    };
    const auto &st = ui_->global<SettingsState>();
    st.set_where_kind(whereKind);
    st.set_processes(toModel(processes));
    st.set_windows(toModel(windows));
}
