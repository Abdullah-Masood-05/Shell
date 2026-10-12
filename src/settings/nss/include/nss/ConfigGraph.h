#pragma once

// ConfigGraph: a root shell.nss plus everything it imports, and the table of
// effective theme/settings values.
//
// Imports are followed like Parser::load_import: the path expression is
// evaluated, a relative result is rooted at the importing file's folder, and
// the imported file is parsed in the importing context (top level, inside a
// theme{}/settings{} block, or inside a menu). Only simple expressions are
// evaluated: string literals, '+' concatenation, $variables defined earlier
// at the top level, app.dir and app.cfg, and @var / %ENV% inside
// 'interpolated' strings. Anything else is reported as unresolved.
//
// Later assignments win, key by key, in parse order (Parser.cpp:603-973).

#include "nss/NssDocument.h"

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace nss
{
	struct GraphOptions
	{
		std::filesystem::path appDir;  // app.dir; defaults to the root's folder
		std::map<std::string, std::string> variables;  // extra variables (lower-case names)
		bool followLang = true;        // load `import lang` files too
		size_t maxDepth = 32;          // Parser::MaxImportDepth
	};

	struct GraphFile
	{
		std::filesystem::path path;
		std::shared_ptr<NssDocument> doc;  // null when the file could not be loaded
		ParseContext context;
		int parent = -1;          // index of the importing file
		size_t importLine = 0;    // line of the import statement in the parent
		std::string error;
	};

	struct UnresolvedImport
	{
		std::filesystem::path file;  // the importing file
		size_t line = 0;
		std::string expression;
		std::string reason;
	};

	struct ValueOrigin
	{
		std::string value;           // the value text, verbatim
		std::filesystem::path file;
		size_t line = 0;
		bool literal = false;        // false: an expression, locked
		std::string writtenAs;       // the path as written, e.g. "theme.background" for an alias
	};

	struct EffectiveValue
	{
		std::string key;                  // canonical key (ThemeSchema::canonical)
		std::vector<ValueOrigin> chain;   // every assignment, in order; back() wins

		const ValueOrigin &effective() const { return chain.back(); }
		// The assignments that `chain[index]` is overridden by.
		std::vector<ValueOrigin> overriddenBy(size_t index) const;
	};

	class ConfigGraph
	{
	public:
		static ConfigGraph load(const std::filesystem::path &root, const GraphOptions &options = {});

		const std::filesystem::path &root() const { return _root; }
		const std::vector<GraphFile> &files() const { return _files; }
		const std::vector<UnresolvedImport> &unresolved() const { return _unresolved; }
		const std::map<std::string, EffectiveValue> &values() const { return _values; }
		const EffectiveValue *find(std::string_view key) const;
		const GraphFile *file(const std::filesystem::path &path) const;

		// Evaluates a simple import expression; nullopt when it is not simple.
		static std::optional<std::string> evaluate(std::string_view expression,
												   const std::map<std::string, std::string> &variables,
												   std::string *reason = nullptr);

	private:
		void loadFile(const std::filesystem::path &path, const ParseContext &context, int parent, size_t line, size_t depth);
		void walk(size_t fileIndex, const std::vector<Node> &nodes, size_t depth, bool topLevel);
		std::filesystem::path resolve(const std::filesystem::path &from, const std::string &value) const;

		std::filesystem::path _root;
		GraphOptions _options;
		std::map<std::string, std::string> _vars;
		std::vector<GraphFile> _files;
		std::vector<UnresolvedImport> _unresolved;
		std::map<std::string, EffectiveValue> _values;
		std::vector<std::string> _stack;  // pathKeys being loaded (cycle guard)
	};
}
