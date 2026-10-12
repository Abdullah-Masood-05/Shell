#pragma once

// The Appearance page (ui/pages/appearance.slint): reads the effective theme
// values from the config graph, shows each with where it comes from, sends
// edits to SaveService as pending changes, and drives the live preview through
// ThemeModel::resolve().
//
// Development switches (read from the command line, for scripted checks):
//   --set <key>=<literal>   put a pending change once the page has loaded (repeatable)
//   --save                  then save it

#include "app.h"

#include "SaveService.h"
#include "ThemeModel.h"
#include "Wallpaper.h"

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class AppearanceController
{
public:
    AppearanceController(slint::ComponentHandle<AppWindow> ui, SaveService &save);

    // Once the window exists: loads the wallpaper, fonts and the config.
    void start();

    // The field table (exposed for tests and the dev switches).
    enum class Kind
    {
        Enum,
        Color,
        Number,
        Step,  // the 0..4 radius step
        Bool,
        Box,   // [horizontal, vertical]
        Font,
    };
    struct FieldDef
    {
        const char *key;
        Kind kind;
        std::vector<const char *> choices; // Enum: the literal written for each index
    };
    static const std::vector<FieldDef> &fieldDefs();

private:
    void configLoaded();
    void rebuild();
    void bindCallbacks();
    void apply(int index, const std::string &literal);
    void reset(int index);
    void applyDevArgs();

    theme::Values currentValues() const;
    theme::SystemInfo systemInfo(bool dark) const;

    slint::ComponentHandle<AppWindow> ui_;
    SaveService &save_;
    std::vector<std::string> fonts_;
    std::map<int, std::string> errors_;
    theme::SystemInfo system_;
    bool previewDark_ = false;
    bool devApplied_ = false;
    std::shared_ptr<slint::VectorModel<ThemeField>> fields_;
};
