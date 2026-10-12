#pragma once

// Installed font families (EnumFontFamiliesExW), for the Appearance font picker.

#include <string>
#include <string_view>
#include <vector>

namespace fonts {

// Family names, UTF-8, sorted case-insensitively, without vertical (@) faces.
std::vector<std::string> families();

bool installed(const std::vector<std::string> &families, std::string_view name);

// The families whose name contains `query` (case-insensitive); all when empty.
std::vector<std::string> filter(const std::vector<std::string> &families, std::string_view query);

} // namespace fonts
