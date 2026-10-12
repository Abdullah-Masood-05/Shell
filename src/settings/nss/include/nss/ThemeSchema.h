#pragma once

// The theme{} and settings{} key tables, copied from Parser::parse_theme and
// Parser::parse_settings (src/dll/src/Parser/Parser.cpp), with value types
// and ranges from the docs and the design notes.
//
// Keys are full lower-case dotted paths: "theme.background.opacity",
// "settings.tip.enabled". A few paths are aliases for another key, because
// the core assigns a block's own value to one of its children:
//   theme = x            -> theme.name
//   theme.background = x -> theme.background.color
//   theme.shadow = x     -> theme.shadow.enabled   (and so on)
// theme.tip.* and settings.tip.* write the same settings, so theme.tip.* is
// folded into settings.tip.*.

#include "nss/Literal.h"

#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace nss
{
	enum class ValueType
	{
		Bool,
		Number,
		Color,
		String,
		Enum,       // one of `values`; strings, keywords or view.* names
		Effect,     // 0..3, or [3, tint, opacity]
		Box,        // padding/margin: n, [v, h] or [l, t, r, b]
		ColorPair,  // tip styles: [background, text]
		ColorList,  // image.color: color or [c1, c2, c3]
		Any,        // expressions and composite values; not checked
	};

	struct KeyInfo
	{
		std::string key;
		ValueType type = ValueType::Any;
		double min = -std::numeric_limits<double>::infinity();
		double max = std::numeric_limits<double>::infinity();
		std::vector<std::string> values;  // Enum
		bool integer = false;
	};

	class ThemeSchema
	{
	public:
		static const std::vector<KeyInfo> &keys();
		static const KeyInfo *find(std::string_view key);  // resolves aliases
		static std::string canonical(std::string_view key);
		static bool isKnown(std::string_view key) { return find(key) != nullptr; }
		static size_t order(std::string_view key);  // position in the table; unknown keys sort last
		static const char *typeName(ValueType type);

		// Errors for `value` under `key`; empty when it is acceptable.
		// Expressions are accepted: they are locked, not validated.
		static std::vector<std::string> validate(std::string_view key, std::string_view value);
		static std::vector<std::string> validate(std::string_view key, const Literal &value);
	};
}
