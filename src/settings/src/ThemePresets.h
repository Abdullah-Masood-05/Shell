#pragma once

// ThemePresets: the example themes of the original Nilesoft Shell project, as
// one-click presets for the Appearance page.
//
// Each preset is a file in src/settings/presets/themes/ (embedded at build time
// by cmake/ThemePresets.cmake): a few `// Key: value` header lines, then the
// theme{} block verbatim from where it was published (or, for the Shell 1.x
// colour templates, ported one value for one). The block is parsed with
// nsscore (NssDocument) and every key is checked with ThemeSchema, so a preset
// that the core would reject never reaches the UI (tests/test_presets.cpp).
//
//   // Preset: Dark acrylic
//   // Tags: Modern, Acrylic, Dark      (up to three, shown under the name)
//   // About: one line for the card
//   // Source: where it comes from (the credit line)
//   theme
//   {
//       ...
//   }
//
// No Slint here: the parsing, staging and export are unit-tested.

#include "PendingChanges.h"

#include "nss/OverridesWriter.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace presets {

struct Preset
{
    std::string id;          // the file name without extension, e.g. "03-acrylic"
    std::string name;        // "Modern dark"
    std::vector<std::string> tags; // "Modern", "Acrylic"
    std::string about;       // one line
    std::string source;      // the credit line
    std::string text;        // the whole file, as embedded
    nss::Overrides values;   // canonical key -> literal text, in file order's last-wins sense
    std::vector<std::string> errors; // parse and schema errors; empty when usable
};

// Parses one preset file. `id` is kept as given.
Preset parse(std::string id, std::string_view text);

// The embedded presets that parsed and validated, in file-name order. Built once.
const std::vector<Preset> &all();
// Every embedded preset, including any with errors (for the tests).
const std::vector<Preset> &embedded();

// Whether `key` (canonical) is one a preset replaces: every theme.* key.
bool isThemeKey(std::string_view key);

// A key's value text, or nullopt (the saved value, or the effective one).
using ValueFn = std::function<std::optional<std::string>(const std::string &)>;

// Stages `preset` into `pending` as the new look:
//   - each preset key is set (or dropped when it already is the saved value: `saved`);
//   - every other theme key this app overrides (`appKeys`, saved in settings-app.nss)
//     is reset, so the user's own files apply again under the preset;
//   - every other pending theme change is forgotten.
// Settings keys and pending changes of other pages are left alone. Returns the
// schema errors (and changes nothing) if any preset value is rejected.
std::vector<std::string> stage(PendingChanges &pending, const Preset &preset, const std::vector<std::string> &appKeys,
                               const ValueFn &saved, std::string_view page = "Appearance");

// True when every preset key has that value now (`current`: the effective text).
bool inUse(const Preset &preset, const ValueFn &current);

// The preset in use: of those inUse(), the one with the most keys this app sets
// (`overrides`: saved or pending, canonical key -> literal), so the one applied
// here wins over one the user's files happen to match; then the one with the most
// keys; then the first. -1 when none is.
int activeIndex(const std::vector<Preset> &list, const ValueFn &current, const nss::Overrides &overrides = {});

// How many of this app's theme overrides (`overrides`: saved or pending, canonical
// key -> literal) Apply would replace. Those the active preset itself set don't
// count: going from one preset to another loses nothing.
int replacedCount(const nss::Overrides &overrides, const Preset *active);

// A theme{} block with `values` (canonical theme.* keys; others are skipped), in
// schema order, ready to paste into a .nss file or to save as a preset.
std::string exportBlock(const nss::Overrides &values, std::string_view eol = "\r\n");

// The preset file as published (header comments with the credit, then theme{}),
// with `eol` line ends: what "Export to clipboard" copies.
std::string exportText(const Preset &preset, std::string_view eol = "\r\n");

} // namespace presets
