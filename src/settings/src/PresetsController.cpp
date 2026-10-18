#include "PresetsController.h"

#include "Text.h"
#include "ThemePresets.h"
#include "Tr.h"

#include "nss/Literal.h"

#include <chrono>
#include <cstring>

using namespace std::chrono_literals;
using nss::Literal;

namespace {

constexpr const char *kPage = "Appearance";

slint::SharedString ss(std::string_view s) { return slint::SharedString(s); }

bool copyToClipboard(const std::string &utf8)
{
    const std::wstring wide = text::widen(utf8);
    if (!OpenClipboard(nullptr))
        return false;
    bool ok = false;
    EmptyClipboard();
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (wide.size() + 1) * sizeof(wchar_t))) {
        if (auto *dst = static_cast<wchar_t *>(GlobalLock(mem))) {
            memcpy(dst, wide.c_str(), (wide.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(mem);
            ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
        }
        if (!ok)
            GlobalFree(mem);
    }
    CloseClipboard();
    return ok;
}

std::string joinTags(const std::vector<std::string> &tags)
{
    std::string out;
    for (const auto &t : tags)
        out += (out.empty() ? "" : " • ") + t;
    return out;
}

} // namespace

PresetsController::PresetsController(slint::ComponentHandle<AppWindow> ui, SaveService &save, PreviewFn preview)
    : ui_(std::move(ui)), save_(save), preview_(std::move(preview)),
      cards_(std::make_shared<slint::VectorModel<PresetCard>>())
{
    const auto &ps = ui_->global<PresetsState>();
    ps.set_presets(cards_);
    ps.on_apply([this](int index) { apply(index); });
    ps.on_export_preset([this](int index) { exportPreset(index); });
    ps.on_export_current([this] { exportCurrent(); });
    nsp::onLanguageChanged([this] {
        // The cards' text only; the thumbnails stay as they are.
        const auto &list = presets::all();
        for (size_t i = 0; i < cards_->row_count() && i < list.size(); ++i) {
            auto c = *cards_->row_data(i);
            c.name = ss(presets::displayName(list[i]));
            c.tags = ss(joinTags(presets::displayTags(list[i])));
            c.about = ss(presets::displayAbout(list[i]));
            cards_->set_row_data(i, c);
        }
    });
}

void PresetsController::rebuild(const theme::Values &values, const theme::SystemInfo &system)
{
    current_ = values;

    // Each thumbnail: the preset over the user's own files, which is what Apply
    // leaves in effect (this app's other theme overrides are reset). On the side,
    // this app's theme overrides as they will be saved, for the confirmation.
    theme::Values files;
    nss::Overrides overrides;
    for (const auto &[key, lit] : values) {
        const auto o = save_.origins(key);
        if (o.file && o.file->literal) {
            auto own = Literal::parse(o.file->value);
            if (own.isLiteral())
                files[key] = std::move(own);
        }
        if (presets::isThemeKey(key)) {
            const auto *pending = save_.pending().find(key);
            if (pending && pending->literal)
                overrides[key] = *pending->literal;
            else if (!pending && o.app)
                overrides[key] = o.app->value;
        }
    }

    const auto &list = presets::all();
    const int active =
        presets::activeIndex(list, [this](const std::string &key) { return save_.current(key); }, overrides);

    std::vector<PresetCard> cards;
    cards.reserve(list.size());
    for (size_t i = 0; i < list.size(); ++i) {
        const auto &preset = list[i];
        auto v = files;
        for (const auto &[key, text] : preset.values)
            v[key] = Literal::parse(text);
        PresetCard c{};
        c.id = ss(preset.id);
        c.name = ss(presets::displayName(preset));
        c.tags = ss(joinTags(presets::displayTags(preset)));
        c.about = ss(presets::displayAbout(preset));
        c.source = ss(preset.source);
        c.t = preview_(theme::resolve(v, system));
        c.active = int(i) == active;
        cards.push_back(c);
    }
    if (cards_->row_count() != cards.size()) {
        cards_ = std::make_shared<slint::VectorModel<PresetCard>>(cards);
        ui_->global<PresetsState>().set_presets(cards_);
    } else {
        for (size_t i = 0; i < cards.size(); ++i)
            cards_->set_row_data(i, cards[i]);
    }
    const auto &ps = ui_->global<PresetsState>();
    ps.set_active(active);
    ps.set_replaced(presets::replacedCount(overrides, active >= 0 ? &list[size_t(active)] : nullptr));
    ps.set_ready(save_.graph() != nullptr);
}

void PresetsController::apply(int index)
{
    const auto &list = presets::all();
    if (index < 0 || index >= int(list.size()) || !save_.graph())
        return;
    const auto &preset = list[size_t(index)];
    if (save_.saving()) {
        toast("info", nsp::tr("Saving"), nsp::trf("Apply {} again once the save is done.", presets::displayName(preset)));
        return;
    }
    std::vector<std::string> appKeys;
    for (const auto &[key, ev] : save_.graph()->values())
        if (presets::isThemeKey(key) && save_.origins(key).app)
            appKeys.push_back(key);
    auto saved = [this](const std::string &key) -> std::optional<std::string> {
        const auto o = save_.origins(key);
        const auto *origin = o.app ? o.app : o.file;
        return origin ? std::optional<std::string>(origin->value) : std::nullopt;
    };
    std::vector<std::string> errors;
    save_.edit([&](PendingChanges &pending) { errors = presets::stage(pending, preset, appKeys, saved, kPage); });
    if (!errors.empty()) {
        toast("error", nsp::trf("Couldn't apply {}", presets::displayName(preset)), errors.front());
        return;
    }
    bool staged = false;
    for (const auto &[key, change] : save_.pending().items())
        staged = staged || presets::isThemeKey(key);
    toast("ok", nsp::trf("{} is in the preview", presets::displayName(preset)),
          staged ? nsp::tr("Save (Ctrl+S) to use it in your menus, or Discard to go back.") : nsp::tr("It is your saved look already."));
}

void PresetsController::exportPreset(int index)
{
    const auto &list = presets::all();
    if (index < 0 || index >= int(list.size()))
        return;
    const auto &preset = list[size_t(index)];
    if (copyToClipboard(presets::exportText(preset)))
        toast("ok", nsp::trf("Copied {}", presets::displayName(preset)), nsp::tr("Paste the theme{} block into a .nss file of your own."));
    else
        toast("error", nsp::tr("Couldn't copy"), nsp::tr("Another app is holding the clipboard. Try again."));
}

void PresetsController::exportCurrent()
{
    nss::Overrides block;
    for (const auto &[key, lit] : current_)
        if (presets::isThemeKey(key))
            block[key] = lit.text;
    if (block.empty()) {
        toast("info", nsp::tr("Nothing to copy"), nsp::tr("Every theme setting has its built-in default."));
        return;
    }
    if (copyToClipboard(presets::exportBlock(block)))
        toast("ok", nsp::tr("Copied your current look"), nsp::tr("A theme{} block with every theme setting in effect, saved or not."));
    else
        toast("error", nsp::tr("Couldn't copy"), nsp::tr("Another app is holding the clipboard. Try again."));
}

void PresetsController::toast(const std::string &kind, const std::string &title, const std::string &body)
{
    const auto &app = ui_->global<AppState>();
    app.set_toast_kind(ss(kind));
    app.set_toast_title(ss(title));
    app.set_toast_text(ss(body));
    app.set_toast_shown(true);
    toastTimer_.start(slint::TimerMode::SingleShot, kind == "error" ? 9s : 5s,
                      [this] { ui_->global<AppState>().set_toast_shown(false); });
}
