#pragma once

// The theme presets gallery (ui/components/preset-gallery.slint, PresetsState):
// the example themes of src/ThemePresets.h as cards, each a mini menu drawn with
// the preset over the user's own files.
//
//   Apply      stages the preset's theme keys as pending changes in one step and
//              resets this app's other theme overrides (SaveService); the footer's
//              Save / Discard / Ctrl+S then work as for any edit. The page asks
//              first when that replaces settings changed here (PresetsState.replaced).
//   Export     copies a preset file (with its credit) to the clipboard.
//   Export current look   copies the effective theme{} block to the clipboard.
//
// Owned by AppearanceController, which calls rebuild() whenever the theme values
// or the preview's light/dark mode change.

#include "app.h"

#include "SaveService.h"
#include "ThemeModel.h"

#include <functional>
#include <memory>
#include <string>

class PresetsController
{
public:
    // `preview`: a resolved theme as the page's preview structure.
    using PreviewFn = std::function<PreviewTheme(const theme::Resolved &)>;

    PresetsController(slint::ComponentHandle<AppWindow> ui, SaveService &save, PreviewFn preview);

    // `values`: the effective theme values (saved and pending); `system`: the
    // Windows settings the preview resolves against.
    void rebuild(const theme::Values &values, const theme::SystemInfo &system);

private:
    void apply(int index);
    void exportPreset(int index);
    void exportCurrent();
    void toast(const std::string &kind, const std::string &title, const std::string &text);

    slint::ComponentHandle<AppWindow> ui_;
    SaveService &save_;
    PreviewFn preview_;
    theme::Values current_;
    std::shared_ptr<slint::VectorModel<PresetCard>> cards_;
    slint::Timer toastTimer_;
};
