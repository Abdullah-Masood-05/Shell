#include "nss/ConfigGraph.h"
#include "nss/Literal.h"
#include "nss/ThemeSchema.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>

namespace fs = std::filesystem;

namespace nss
{
	namespace
	{
		std::optional<std::string> environment(const std::string &name)
		{
			auto w = widen(name);
			DWORD n = ::GetEnvironmentVariableW(w.c_str(), nullptr, 0);
			if(n == 0)
				return std::nullopt;
			std::wstring value(n, L'\0');
			n = ::GetEnvironmentVariableW(w.c_str(), value.data(), n);
			value.resize(n);
			return narrow(value);
		}

		// Body of a '...' string, with @name / @@ / %ENV% handled the way
		// Parser::parse_interpolated_string does for the simple cases.
		std::optional<std::string> interpolate(std::string_view body, const std::map<std::string, std::string> &vars,
											   std::string *reason)
		{
			std::string out;
			for(size_t i = 0; i < body.size();)
			{
				char c = body[i];
				if(c == '/' && i + 1 < body.size() && body[i + 1] == '*')
				{
					auto e = body.find("*/", i + 2);
					i = e == std::string_view::npos ? body.size() : e + 2;
					continue;
				}
				if(c == '@')
				{
					if(i + 1 < body.size() && body[i + 1] == '@')
					{
						out += '@';
						i += 2;
						continue;
					}
					if(i + 1 < body.size() && NssLexer::isIdentStart(body[i + 1]))
					{
						size_t e = i + 1;
						while(e < body.size() && (NssLexer::isIdentChar(body[e]) ||
												  (body[e] == '.' && e + 1 < body.size() && NssLexer::isIdentStart(body[e + 1]))))
							e++;
						if(e < body.size() && (body[e] == '(' || body[e] == '['))
						{
							if(reason) *reason = "function call in string";
							return std::nullopt;
						}
						auto name = toLower(body.substr(i + 1, e - i - 1));
						auto it = vars.find(name);
						if(it == vars.end())
						{
							if(reason) *reason = "unknown identifier '" + name + "'";
							return std::nullopt;
						}
						out += it->second;
						i = e;
						continue;
					}
					if(i + 1 < body.size() && (body[i + 1] == '(' || body[i + 1] == '{' || body[i + 1] == '"'))
					{
						if(reason) *reason = "expression in string";
						return std::nullopt;
					}
					out += c;
					i++;
					continue;
				}
				if(c == '%')
				{
					size_t e = i + 1;
					while(e < body.size() && (NssLexer::isIdentChar(body[e]) || body[e] == '(' || body[e] == ')'))
						e++;
					if(e < body.size() && body[e] == '%' && e > i + 1)
					{
						if(auto v = environment(std::string(body.substr(i + 1, e - i - 1))))
						{
							out += *v;
							i = e + 1;
							continue;
						}
						out.append(body.substr(i, e + 1 - i));
						i = e + 1;
						continue;
					}
					out += c;
					i++;
					continue;
				}
				out += c;
				i++;
			}
			return out;
		}
	}

	std::vector<ValueOrigin> EffectiveValue::overriddenBy(size_t index) const
	{
		if(index + 1 >= chain.size())
			return {};
		return std::vector<ValueOrigin>(chain.begin() + ptrdiff_t(index) + 1, chain.end());
	}

	std::optional<std::string> ConfigGraph::evaluate(std::string_view expression,
													 const std::map<std::string, std::string> &vars,
													 std::string *reason)
	{
		auto toks = NssLexer::tokenize(expression);
		std::string out;
		size_t i = 0;
		auto fail = [&](std::string why) -> std::optional<std::string>
		{
			if(reason) *reason = std::move(why);
			return std::nullopt;
		};
		auto text = [&](const Token &t) { return expression.substr(t.begin, t.size()); };

		for(;;)
		{
			if(toks[i].kind == TokenKind::At)
				i++;
			const Token &t = toks[i];
			if(t.error)
				return fail("invalid token");
			switch(t.kind)
			{
				case TokenKind::String:
				{
					auto lit = Literal::parse(text(t));
					if(lit.kind != LiteralKind::String)
						return fail("invalid string");
					out += lit.string;
					break;
				}
				case TokenKind::IString:
				{
					auto s = text(t);
					auto v = interpolate(s.substr(1, s.size() - 2), vars, reason);
					if(!v)
						return std::nullopt;
					out += *v;
					break;
				}
				case TokenKind::Number:
					out += text(t);
					break;
				case TokenKind::Ident:
				case TokenKind::Variable:
				{
					auto name = toLower(text(t));
					if(t.kind == TokenKind::Variable)
						name.erase(0, 1);
					// path.location(x) / path.parent(x): the folder of a simple x, as
					// taskbar.nss builds the Apps list's path from app.cfg.
					if(toks[i + 1].kind == TokenKind::LParen && (name == "path.location" || name == "path.parent"))
					{
						size_t close = i + 2;
						int depth = 1;
						for(; toks[close].kind != TokenKind::Eof; close++)
						{
							if(toks[close].kind == TokenKind::LParen)
								depth++;
							else if(toks[close].kind == TokenKind::RParen && --depth == 0)
								break;
						}
						if(depth != 0 || close == i + 2)
							return fail("function call '" + name + "(...)'");
						auto inner = expression.substr(toks[i + 2].begin, toks[close].begin - toks[i + 2].begin);
						auto arg = evaluate(inner, vars, reason);
						if(!arg)
							return std::nullopt;
						auto cut = arg->find_last_of("\\/");
						out += cut == std::string::npos ? std::string() : arg->substr(0, cut);
						i = close;
						break;
					}
					if(toks[i + 1].kind == TokenKind::LParen || toks[i + 1].kind == TokenKind::LBracket)
						return fail("function call '" + name + "(...)'");
					auto it = vars.find(name);
					if(it == vars.end())
						return fail("unknown identifier '" + name + "'");
					out += it->second;
					break;
				}
				default:
					return fail("unsupported expression");
			}
			i++;
			if(toks[i].kind == TokenKind::Eof)
				return out;
			if(toks[i].kind == TokenKind::Operator && text(toks[i]) == "+")
			{
				i++;
				continue;
			}
			return fail("unsupported operator");
		}
	}

	ConfigGraph ConfigGraph::load(const fs::path &root, const GraphOptions &options)
	{
		ConfigGraph g;
		std::error_code ec;
		g._root = fs::absolute(root, ec).lexically_normal();
		g._options = options;
		auto appDir = options.appDir.empty() ? g._root.parent_path() : options.appDir;
		g._vars["app.dir"] = narrow(appDir.lexically_normal().wstring());
		g._vars["app.cfg"] = narrow(g._root.wstring());
		for(auto &[k, v] : options.variables)
			g._vars[toLower(k)] = v;
		g.loadFile(g._root, {}, -1, 0, 0);
		return g;
	}

	const EffectiveValue *ConfigGraph::find(std::string_view key) const
	{
		auto it = _values.find(ThemeSchema::canonical(key));
		return it == _values.end() ? nullptr : &it->second;
	}

	const GraphFile *ConfigGraph::file(const fs::path &path) const
	{
		auto key = pathKey(fs::absolute(path));
		for(auto &f : _files)
			if(pathKey(f.path) == key)
				return &f;
		return nullptr;
	}

	fs::path ConfigGraph::resolve(const fs::path &from, const std::string &value) const
	{
		fs::path p(widen(trim(value)));
		if(!p.is_absolute())
			p = from.parent_path() / p;
		p = p.lexically_normal();
		// Legacy alias kept by load_import: ua.nss is now uk.nss.
		std::error_code ec;
		if(iequals(narrow(p.filename().wstring()), "ua.nss") && !fs::exists(p, ec))
		{
			auto alias = p.parent_path() / L"uk.nss";
			if(fs::exists(alias, ec))
				p = alias;
		}
		return p;
	}

	void ConfigGraph::loadFile(const fs::path &path, const ParseContext &context, int parent, size_t line, size_t depth)
	{
		auto key = pathKey(path);
		auto parentPath = parent >= 0 ? _files[size_t(parent)].path : fs::path();
		if(std::find(_stack.begin(), _stack.end(), key) != _stack.end())
		{
			_unresolved.push_back({ parentPath, line, narrow(path.wstring()), "import cycle" });
			return;
		}
		if(depth >= _options.maxDepth)
		{
			_unresolved.push_back({ parentPath, line, narrow(path.wstring()), "import nesting too deep" });
			return;
		}

		std::error_code ec;
		// As Shell does: one of the app's files (imports\settings-app*.nss) that isn't
		// written yet is no missing file, just nothing yet (taskbar.nss's Apps list).
		if(!fs::is_regular_file(path, ec) && toLower(narrow(path.filename().wstring())).rfind("settings-app", 0) == 0)
			return;

		GraphFile f;
		f.path = path;
		f.context = context;
		f.parent = parent;
		f.importLine = line;
		if(!fs::is_regular_file(path, ec))
			f.error = "file not found";
		else if(auto doc = NssDocument::load(path, context, &f.error))
			f.doc = std::make_shared<NssDocument>(std::move(*doc));
		size_t index = _files.size();
		_files.push_back(f);
		if(!f.doc)
			return;

		_stack.push_back(key);
		auto doc = _files[index].doc;  // keeps the nodes alive while _files grows
		walk(index, doc->nodes(), depth, context.mode == ParseMode::Config);
		_stack.pop_back();
	}

	void ConfigGraph::walk(size_t fileIndex, const std::vector<Node> &nodes, size_t depth, bool topLevel)
	{
		auto doc = _files[fileIndex].doc;
		auto filePath = _files[fileIndex].path;
		auto mode = _files[fileIndex].context.mode;

		for(auto &n : nodes)
		{
			switch(n.kind)
			{
				case NodeKind::Opaque:
					if(topLevel && n.path.size() > 1 && n.path[0] == '$' && !n.valueSpan.empty())
					{
						if(auto v = evaluate(doc->slice(n.valueSpan), _vars))
							_vars[n.path.substr(1)] = *v;
						else
							_vars.erase(n.path.substr(1));
					}
					break;
				case NodeKind::Assign:
				{
					auto key = ThemeSchema::canonical(n.path);
					auto &ev = _values[key];
					ev.key = key;
					ValueOrigin o;
					o.value = doc->slice(n.valueSpan);
					o.file = filePath;
					o.line = n.line;
					o.literal = Literal::parse(o.value).isLiteral();
					o.writtenAs = n.path;
					ev.chain.push_back(std::move(o));
					break;
				}
				case NodeKind::Block:
				case NodeKind::MenuDecl:
					walk(fileIndex, n.children, depth, false);
					break;
				case NodeKind::Import:
				{
					if(!n.enabled)
						break;
					ParseContext ctx;
					if(n.importKind == ImportKind::Lang)
					{
						if(!_options.followLang)
							break;
						ctx.mode = ParseMode::Lang;
					}
					else if(!n.context.empty())
					{
						ctx.mode = ParseMode::Settings;
						ctx.prefix = n.context;
					}
					else if(!topLevel || mode == ParseMode::Menu)
						ctx.mode = ParseMode::Menu;
					auto expr = std::string(doc->slice(n.valueSpan));
					std::string reason;
					auto value = evaluate(expr, _vars, &reason);
					if(!value)
					{
						_unresolved.push_back({ filePath, n.line, expr, reason });
						break;
					}
					if(trim(*value).empty())
						break;
					loadFile(resolve(filePath, *value), ctx, int(fileIndex), n.line, depth + 1);
					break;
				}
			}
		}
	}
}
