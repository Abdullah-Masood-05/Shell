#pragma once

// Text decoding and encoding for .nss files.
//
// Detection follows the core's Encoding::GetType (src/shared/System/Text/
// Encoding.h, used by Lexer::load_File): UTF-16 LE/BE with a BOM, UTF-8 with
// a BOM, BOM-less UTF-16 recognised by its zero bytes, valid UTF-8, and the
// ANSI code page as the fallback. UTF-32 is refused, as the core does.
//
// Inside the library all text is UTF-8. Unpaired UTF-16 surrogates are kept
// as WTF-8 so a UTF-16 file always encodes back to the same bytes.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace nss
{
	enum class Encoding
	{
		Utf8,
		Utf16LE,
		Utf16BE,
		Ansi,
	};

	enum class Eol
	{
		CRLF,
		LF,
		CR,
	};

	struct TextFormat
	{
		Encoding encoding = Encoding::Utf8;
		bool bom = false;
		Eol eol = Eol::CRLF;  // the most common line ending, used for new lines
		bool mixedEol = false;

		bool operator==(const TextFormat &) const = default;
	};

	struct DecodedText
	{
		bool ok = false;
		std::string error;
		std::string text;  // UTF-8 (WTF-8 for lone surrogates), BOM removed
		TextFormat format;
		bool lossless = true;  // encode(text, format) gives back the input bytes
	};

	// The default for files the app creates: UTF-8 with a BOM and CRLF, like
	// the shipped configuration.
	TextFormat defaultFormat();

	DecodedText decode(std::string_view bytes);
	std::string encode(std::string_view utf8, const TextFormat &format);

	std::string_view eolString(Eol eol);
	Eol detectEol(std::string_view text, bool *mixed = nullptr);
	const char *encodingName(Encoding encoding);

	std::wstring widen(std::string_view utf8);
	std::string narrow(std::wstring_view wide);

	bool readFile(const std::filesystem::path &path, std::string &bytes, std::string *error = nullptr);

	// ASCII-only helpers; .nss identifiers are matched case-insensitively.
	std::string toLower(std::string_view s);
	bool iequals(std::string_view a, std::string_view b);
	std::string_view trim(std::string_view s);
	std::string pathKey(const std::filesystem::path &path);  // normalized, lower-case, for comparisons
}
