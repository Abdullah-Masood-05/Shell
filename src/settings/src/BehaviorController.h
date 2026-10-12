#pragma once

// The Behavior and Rules & Filters pages (ui/pages/behavior.slint, rules.slint):
// the settings{} block. Like AppearanceController, it reads the effective values
// from SaveService's config graph, shows where each comes from, and turns edits
// into pending changes that the shared footer saves.

#include "app.h"

#include "SaveService.h"

#include <map>
#include <string>
#include <vector>

class BehaviorController
{
public:
    BehaviorController(slint::ComponentHandle<AppWindow> ui, SaveService &save);

    enum class Kind
    {
        Bool,
        Number,
        Seconds,  // tip.time: 0..10 s, one decimal
        Pair,     // [background, text]
        Folder,   // a quoted path
        Enum,     // `choices` are the literals written
        Where,    // exclude.where: the switch plus a raw expression
        Names,    // exclude.process / exclude.window: a list of names
    };
    struct FieldDef
    {
        const char *key;
        Kind kind;
        std::vector<const char *> choices;
    };
    // The order matches the SK global in ui/pages/settings-state.slint.
    static const std::vector<FieldDef> &fieldDefs();

private:
    void bindCallbacks();
    void configLoaded();
    void rebuild();
    void apply(int index, const std::string &literal);
    void reset(int index);
    void pickFolder(int index);
    std::vector<std::string> names(int index) const;

    slint::ComponentHandle<AppWindow> ui_;
    SaveService &save_;
    std::map<int, std::string> errors_;
    std::shared_ptr<slint::VectorModel<ThemeField>> fields_;
};
