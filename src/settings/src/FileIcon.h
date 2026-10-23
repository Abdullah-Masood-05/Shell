#pragma once

// A program's or an icon file's icon as straight RGBA (Open with, the Snippets preview).

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fileicon {

// The icon `index` of `path` (an .exe, .dll or .ico; SHDefExtractIcon's index: negative
// for a resource id) at `size` px. nullopt when the file has none.
std::optional<std::vector<uint8_t>> extract(const std::wstring &path, int index, int size, uint32_t &w, uint32_t &h);

} // namespace fileicon
