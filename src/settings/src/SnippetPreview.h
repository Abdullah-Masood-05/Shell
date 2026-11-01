#pragma once

// The Snippets page's preview: what one snippet adds to the menu, drawn with the
// Appearance page's menu mock (ui/preview/menu-preview.slint). No Slint here
// (tests/test_snippet_preview.cpp); SnippetsController runs shell.exe and draws.
//
// How a preview is made:
//   1. a scratch folder, %TEMP%\NilesoftShellPro\snippet-preview\<id>, gets the snippet
//      as snippet.nss and a root.nss (scratchRoot()) that imports the user's theme and
//      image files (absolute paths) and then only the snippet;
//   2. `shell.exe -config:tree root.nss -context:<c> [-sample:<path>]` runs for each
//      context (file, folder, back, desktop, drive, taskbar); the sample of the file
//      context has the extension the snippet's find= asks for (sampleFor());
//   3. the snippet's items in each tree (itemsOf()) get their icon, position and
//      separators from their declaration in the snippet, since the tree has titles only;
//   4. compose() puts them between a few greyed items of Windows' own menu for that
//      context, so it's clear where they show and which ones are the snippet's.

#include "MenuMap.h"

#include "nss/NssDocument.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace snippetpreview {

// --- contexts --------------------------------------------------------------------------

enum class Context
{
    File,
    Folder,
    Back,
    Desktop,
    Drive,
    Taskbar,
};
inline constexpr Context kContexts[] = { Context::File,    Context::Folder, Context::Back,
                                         Context::Desktop, Context::Drive,  Context::Taskbar };

const char *contextName(Context c);  // "file", "folder", "back", ... (-context:)
const char *contextLabel(Context c); // "File", "Folder", "Background", ... (nsp::mark()ed)

// --- the scratch root ------------------------------------------------------------------

using ReadFile = std::function<std::optional<std::string>(const std::filesystem::path &)>;

// What the scratch root takes from the user's config.
struct ThemeSource
{
    std::vector<std::filesystem::path> imports; // files with a theme{} block or image definitions
    std::string inlineTheme;                    // the root's own theme{} blocks, as written
    std::optional<std::filesystem::path> lang;  // imports\lang\en.nss next to the root, when it exists
};

// The root's enabled top-level imports with a literal path (resolved against the
// root's folder) whose file `read` finds with a theme{} block or @name='<svg ...>'
// image definitions; the root's own theme{} blocks.
ThemeSource themeSource(const nss::NssDocument &root, const std::filesystem::path &rootDir, const ReadFile &read);

// root.nss for the scratch folder: the theme, images and language from `source`, then
// `import '<snippetFile>'` (a name in the same folder). LF line endings.
std::string scratchRoot(const ThemeSource &source, std::string_view snippetFile);

// --- the sample ------------------------------------------------------------------------

struct Sample
{
    std::string extension = ".txt"; // of the sample file for the File context
    bool fromSnippet = false;       // the snippet's find= (or where=) asked for it
};

// The first extension the snippet's find= values (or a where= comparing sel.file.ext
// or sel.ext with a string) name, else .txt.
Sample sampleFor(std::string_view snippetText);

// --- icons -----------------------------------------------------------------------------

enum class IconKind
{
    None,
    Glyph, // a character of an icon font
    Svg,   // an SVG template: @image.color1 and variables are filled in by svgFor()
    File,  // an icon file or a program's icon (the path may hold @sys.* variables)
};

enum class GlyphFont
{
    Shell, // Nilesoft.Shell (image=\uE0AA, image.glyph())
    Segoe, // Segoe Fluent Icons / MDL2 Assets (image.mdl(), image.fluent(), image.segoe())
};

// A colour an image= gives: one of the theme's image colours (image.color2) or a literal
// one (#f00, color.greenyellow). Neither: the theme's own.
struct IconColor
{
    int theme = 0;     // 1..3: theme.image.color1..3
    uint32_t argb = 0; // 0xAARRGGBB, when theme is 0

    bool given() const { return theme != 0 || argb != 0; }
    bool operator==(const IconColor &) const = default;
};

struct Icon
{
    IconKind kind = IconKind::None;
    std::string glyph; // UTF-8
    GlyphFont font = GlyphFont::Shell;
    std::string svg;
    std::string path;
    int index = 0;     // the icon in the file
    // Glyph: colors[0] is its colour. SVG: in place of @image.color1..3 (icon.copy(#f00, auto)).
    IconColor colors[3];
    // A file icon that may not be there (if(path.exists(x), x, [, image.color2])):
    // what shows when it isn't.
    std::shared_ptr<const Icon> fallback;

    bool operator==(const Icon &) const = default;
};

// #rgb / #rrggbb / #rrggbbaa, color.<name>, image.color1..3, rgb(r, g, b); auto and
// default are no colour. nullopt when it isn't a colour.
std::optional<IconColor> colorOf(std::string_view value);

// The colours an SVG icon is drawn with: the icon's own where it gives them, else the
// theme's `theme` (0xAARRGGBB, theme.image.color).
void iconColors(const Icon &icon, const uint32_t theme[3], uint32_t out[3]);

// A glyph's colour (0xAARRGGBB); 0 when the theme's icon colour applies.
uint32_t glyphColor(const Icon &icon, const uint32_t theme[3]);

// Image definitions, from images.nss and the snippet: @copy,copy_to_clipboard='<svg ...>'
// and $name=value variables (the value as written).
struct ImageDefs
{
    std::map<std::string, std::string> svgs; // name (lower-case) -> SVG template
    std::map<std::string, std::string> vars; // name (lower-case, no '$') -> value
};
void collectImageDefs(std::string_view text, ImageDefs &defs);

// The icon an image= value gives; `parent` is the enclosing menu's, for image=inherit
// and a bare `image`.
Icon iconOf(std::string_view value, const ImageDefs &defs, const Icon *parent = nullptr);

// The SVG with @image.color1..3 and the variables it uses filled in. `colors` are
// 0xAARRGGBB (theme.image.color); `light` decides @if(theme.islight, a, b).
std::string svgFor(std::string_view svg, const ImageDefs &defs, const uint32_t colors[3], bool light);

// A file icon's path with the @sys.* / @user.* / @app.* variables replaced from `vars`
// ("sys.prog" -> "C:\Program Files"). Quotes are removed.
std::string expandPath(std::string_view path, const std::map<std::string, std::string> &vars);

// --- the snippet's items ---------------------------------------------------------------

enum class Pos
{
    Middle,
    Top,
    Bottom,
};

// A title as the menu shows it: "Copy &lnk path	Ctrl+L" -> "Copy lnk path", the
// accelerator at byte 5 ("l"), "Ctrl+L" on the right. "&&" is a plain "&".
struct Title
{
    std::string label;
    int accel = -1;    // byte offset of the underlined character in label, or -1
    std::string keys;  // after a tab
};
Title splitTitle(std::string_view raw);

struct Item
{
    std::string kind;          // item | menu | separator
    std::string title;         // "" with dynamicTitle: it depends on the selection
    int accel = -1;            // the underlined character in title (byte offset), or -1
    std::string keys;          // keys=, or the title's text after a tab: shown on the right
    bool dynamicTitle = false;
    Icon icon;
    bool disabled = false;
    bool expanded = false;     // a menu whose items show in place of it
    Pos pos = Pos::Middle;
    bool sepBefore = false;
    bool sepAfter = false;
    std::vector<Item> children;
};

// The items of `shell` (the tree's Shell items) declared in `snippetFile`, with what
// their declarations in `snippet` say about icons, keys=, position and separators.
// Hidden items, items without a title and items that only show with Shift held
// (where=key.shift()) are left out.
std::vector<Item> itemsOf(const std::vector<menumap::Node> &shell, const std::filesystem::path &snippetFile,
                          const nss::NssDocument &snippet, const ImageDefs &defs);

// --- the menu shown --------------------------------------------------------------------

struct Row
{
    std::string label;
    int accel = -1;           // the underlined character in label (byte offset), or -1
    std::string keys;         // shown on the right
    Icon icon;
    bool separator = false;
    bool disabled = false;    // vis=disabled
    bool ghost = false;       // one of Windows' own items around the snippet's (faded)
    bool own = false;         // the snippet's
    bool groupFirst = false;  // the first and last rows of a run of the snippet's rows
    bool groupLast = false;   //   in the main menu (separators inside a run don't end it)
    bool placeholder = false; // a title that depends on the selection
    int sub = -1;             // index into Composed::subs
};

struct Menu
{
    std::vector<Row> rows;
};

struct Composed
{
    Menu main;
    std::vector<Menu> subs;
    int open = -1;  // the row whose submenu shows at first: the snippet's first menu
    int own = 0;    // the snippet's rows in the main menu
};

using Translate = std::function<std::string(const char *)>;

// The snippet's items between a few of Windows' own for `context` (ghost rows: at most
// two before the snippet's middle rows and one after them). Separators never lead,
// trail or double up. `tr` translates the frame's labels and the
// placeholder for a title that depends on the selection.
Composed compose(Context context, const std::vector<Item> &items, const Translate &tr);

} // namespace snippetpreview
