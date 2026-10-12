#pragma once

// Literal values: the part of the language the app can read and write.
//
//   number    50, -1.5, 0x10
//   string    "modern" (escapes decoded), 'plain' (no @ or %)
//   color     #rgb #rgba #rrggbb #rrggbbaa (and #rgbaa, which the core accepts)
//   keyword   true false yes no auto default none
//   name      color.accent, view.compact (two-segment color.* / view.* idents)
//   array     [3, #202020, 50], nested arrays allowed, items must be literals
//
// Anything else is an Expression: locked in the UI and kept verbatim.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nss
{
	enum class LiteralKind
	{
		Number,
		String,
		Color,
		Keyword,
		Name,
		Array,
		Expression,
	};

	struct Literal
	{
		LiteralKind kind = LiteralKind::Expression;
		std::string text;        // source text, trimmed
		double number = 0;       // Number
		std::string string;      // String: decoded value; Keyword/Name: lower-case word
		uint32_t rgba = 0;       // Color: 0xRRGGBBAA
		std::vector<Literal> items; // Array

		bool isLiteral() const { return kind != LiteralKind::Expression; }
		bool isBool() const;
		bool boolValue() const;  // true/yes or a non-zero number

		static Literal parse(std::string_view text);

		static Literal makeNumber(double value);
		static Literal makeString(std::string_view value);
		static Literal makeColor(uint32_t rgba, bool forceAlpha = false);
		static Literal makeKeyword(std::string_view word);
		static Literal makeName(std::string_view dotted);
		static Literal makeArray(std::vector<Literal> items);
	};

	std::string formatNumber(double value);
	std::string quoteString(std::string_view value); // "..." with \ escapes
	std::optional<uint32_t> parseColor(std::string_view hashHex); // "#..." -> 0xRRGGBBAA
}
