#pragma once

// NssDocument: a lossless syntax tree over one .nss file.
//
// The document keeps the decoded text verbatim; nodes only point into it, so
// serialize() reproduces the input byte-for-byte until an edit is made. Every
// edit is a minimal text splice followed by a reparse, so untouched text
// (comments, spacing, odd formatting) is never rewritten.
//
// Node kinds:
//   Block     theme { ... }, settings { ... }, background { ... }; path is the
//             full lower-case dotted path ("theme.background").
//   Assign    key = value inside theme/settings, or theme.x.y = value at the
//             top level; path is the full lower-case dotted path.
//   Import    import [lang|loc] <expr>; disabled imports ("// import '...'")
//             are listed too, with enabled == false.
//   MenuDecl  menu/item/separator/modify/remove(...) with its properties and,
//             for menus, child declarations.
//   Opaque    anything else ($variables, @images, lang keys, ...), verbatim.

#include "nss/NssLexer.h"
#include "nss/Text.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nss
{
	struct Span
	{
		size_t begin = 0;
		size_t end = 0;

		size_t size() const { return end - begin; }
		bool empty() const { return end <= begin; }
		bool operator==(const Span &) const = default;
	};

	enum class NodeKind
	{
		Block,
		Assign,
		Import,
		MenuDecl,
		Opaque,
	};

	enum class MenuKind
	{
		None,
		Menu,
		Item,
		Separator,
		Modify,
		Remove,
	};

	enum class ImportKind
	{
		Config,  // import 'file'
		Lang,    // import lang 'file' / import loc 'file'
	};

	struct MenuProp
	{
		std::string name;  // lower-case, e.g. "title", "cmd-line"
		Span nameSpan;
		Span valueSpan;    // empty for flags such as `admin` or `image`
		bool hasValue = false;
	};

	struct Node
	{
		NodeKind kind = NodeKind::Opaque;
		Span span;           // the whole node, without leading trivia
		size_t line = 0;     // 1-based line where the node starts
		std::string path;    // Block/Assign: full dotted path; MenuDecl: keyword; Opaque: "$name" for variables
		Span keySpan;        // Assign/MenuDecl/Opaque variable: the key or keyword
		Span valueSpan;      // Assign: value; Import: path expression; Opaque variable: value
		Span bodySpan;       // Block/MenuDecl: from '{' to '}' inclusive (empty when absent)

		// Import
		bool enabled = true;
		ImportKind importKind = ImportKind::Config;
		std::string context; // enclosing block path ("theme"), empty at the top level
		Span keywordSpan;    // the `import` keyword (inside the comment when disabled)

		// MenuDecl
		MenuKind menuKind = MenuKind::None;
		std::vector<MenuProp> props;
		Span propsSpan;      // from '(' to ')' inclusive

		std::vector<Node> children;
	};

	enum class ParseMode
	{
		Config,    // a root or a top-level import
		Settings,  // imported inside theme{}/settings{}: key = value lines
		Menu,      // imported inside menu{}: declarations
		Lang,      // import lang: name = value lines
	};

	struct ParseContext
	{
		ParseMode mode = ParseMode::Config;
		std::string prefix;  // Settings: the enclosing block path, e.g. "theme"
	};

	struct Diagnostic
	{
		size_t line = 0;
		std::string message;
	};

	class NssDocument
	{
	public:
		NssDocument() = default;

		// `text` is UTF-8; `format` decides how serialize() encodes it.
		static NssDocument fromText(std::string text, TextFormat format = defaultFormat(), ParseContext context = {});
		static std::optional<NssDocument> fromBytes(std::string_view bytes, ParseContext context = {}, std::string *error = nullptr);
		static std::optional<NssDocument> load(const std::filesystem::path &path, ParseContext context = {}, std::string *error = nullptr);

		const std::string &text() const { return _text; }
		std::string serialize() const;  // encoded with format(): encoding, BOM
		const TextFormat &format() const { return _format; }
		void setFormat(const TextFormat &format) { _format = format; }
		std::string_view eol() const { return eolString(_format.eol); }
		const ParseContext &context() const { return _context; }
		const std::filesystem::path &path() const { return _path; }
		void setPath(const std::filesystem::path &path) { _path = path; }
		bool modified() const { return _modified; }
		bool lossless() const { return _lossless; }

		const std::vector<Node> &nodes() const { return _nodes; }
		const std::vector<Token> &tokens() const { return _tokens; }
		const std::vector<Diagnostic> &diagnostics() const { return _diagnostics; }
		std::string_view slice(Span span) const;
		size_t lineOf(size_t offset) const;

		// Depth-first walk; return false from the callback to skip children.
		void visit(const std::function<bool(const Node &)> &fn) const;

		std::vector<const Node *> assigns(std::string_view path = {}) const; // all, or those with `path`
		const Node *findAssign(std::string_view path) const;  // the last one (the one that wins)
		const Node *findBlock(std::string_view path) const;   // the last block with a body
		std::vector<const Node *> imports() const;            // top-level imports, enabled or not

		// Edits. Each returns false (and changes nothing) when it cannot apply.
		bool setValue(std::string_view path, std::string_view literal, bool force = false);
		// The spacing around '=' is the block's last value's, else `sep`.
		bool insertAssign(std::string_view blockPath, std::string_view key, std::string_view literal, std::string_view sep = "=");
		bool removeAssign(std::string_view path);
		bool toggleImport(size_t index, bool enabled);
		bool removeImport(size_t index);
		bool appendImport(std::string_view path, ImportKind kind = ImportKind::Config);
		void replace(Span span, std::string_view text);  // raw splice, then reparse
		void setText(std::string text);

	private:
		void reparse();
		Span lineSpanOf(Span span, bool includeEol) const;  // full lines covering span
		bool ownsLines(Span span) const;                    // nothing else on those lines
		std::string indentOf(size_t offset) const;

		std::string _text;
		TextFormat _format = defaultFormat();
		ParseContext _context;
		std::filesystem::path _path;
		bool _modified = false;
		bool _lossless = true;
		std::vector<Token> _tokens;
		std::vector<Node> _nodes;
		std::vector<Diagnostic> _diagnostics;
		std::vector<size_t> _lineStarts;
	};

	// Quotes a path for an import statement: 'path' when it is safe in an
	// interpolated string, otherwise "path" with escapes.
	std::string quoteImportPath(std::string_view path);
}
