#pragma once

// Lossless tokenizer for .nss text.
//
// Every token owns the whitespace and comments in front of it (its leading
// trivia), and the final Eof token owns whatever trails the last token, so
// concatenating [triviaBegin, end) over all tokens gives back the input.
//
// The rules mirror the core (src/dll/src/Parser/Lexer.cpp, Parser.cpp):
//  - comments are // to end of line and /* */; they are trivia.
//  - "double quoted" strings take \ escapes and cannot span lines.
//  - 'single quoted' and `backtick` strings are interpolated: they may span
//    lines, have no escapes, and may embed @expr, @(...), @{...}, @"..."
//    (which can hold nested quotes) and /* */ comments.
//  - numbers: 12, 1.5, .5, 0x1F. Signs are separate operator tokens.
//  - colors: '#' followed by hex digits.
//  - identifiers: letters, digits and '_', with '.' joining segments when the
//    next char starts a segment: app.dir, theme.background.opacity.
//  - \uXXXX is a character token, $name a variable, @ an image/verbatim
//    marker.

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace nss
{
	enum class TokenKind
	{
		Ident,
		Number,
		Color,
		String,   // "..."
		IString,  // '...' or `...`
		Char,     // \uXXXX
		Variable, // $name
		At,       // @
		Operator, // + - * / % == != < > <= >= && || & | ^ << >> ! ~ += -=
		Dot,
		Assign,   // =
		Colon,
		Question,
		Comma,
		LParen,
		RParen,
		LBrace,
		RBrace,
		LBracket,
		RBracket,
		Unknown,
		Eof,
	};

	struct Token
	{
		TokenKind kind = TokenKind::Unknown;
		size_t triviaBegin = 0; // start of the leading whitespace/comments
		size_t begin = 0;       // start of the token text
		size_t end = 0;         // one past the token text
		size_t line = 1;        // 1-based line of `begin`
		bool error = false;     // unterminated string/comment, bad escape, ...

		size_t size() const { return end - begin; }
		bool hasTrivia() const { return triviaBegin != begin; }
	};

	struct LexError
	{
		size_t offset = 0;
		size_t line = 0;
		std::string message;
	};

	class NssLexer
	{
	public:
		static std::vector<Token> tokenize(std::string_view src, std::vector<LexError> *errors = nullptr);

		static const char *kindName(TokenKind kind);
		static bool isIdentStart(char c);
		static bool isIdentChar(char c);
	};
}
