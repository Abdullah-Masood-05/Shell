#include "nss/NssDocument.h"
#include "nss/Literal.h"

#include <algorithm>

namespace nss
{
	namespace
	{
		bool isBlank(char c) { return c == ' ' || c == '\t'; }

		MenuKind menuKindOf(std::string_view word)
		{
			if(word == "menu") return MenuKind::Menu;
			if(word == "item") return MenuKind::Item;
			if(word == "sep" || word == "separator") return MenuKind::Separator;
			if(word == "modify" || word == "set" || word == "update" || word == "change") return MenuKind::Modify;
			if(word == "remove") return MenuKind::Remove;
			return MenuKind::None;
		}

		// Structural parser over the token stream. It never fails: whatever it
		// does not understand becomes an Opaque node, and the text is kept by
		// the document anyway. Expression extents follow Parser::parse_expression:
		// a term, then as long as a binary operator follows, another term.
		struct TreeParser
		{
			std::string_view src;
			const std::vector<Token> &toks;
			std::vector<Diagnostic> &diags;
			size_t base = 0;  // offset added to spans (for imports parsed out of comments)
			size_t i = 0;

			TreeParser(std::string_view s, const std::vector<Token> &t, std::vector<Diagnostic> &d)
				: src(s), toks(t), diags(d) {}

			const Token &cur() const { return toks[i]; }
			const Token &peek(size_t k = 1) const { return toks[std::min(i + k, toks.size() - 1)]; }
			std::string_view text(const Token &t) const { return src.substr(t.begin, t.end - t.begin); }
			std::string lower(const Token &t) const { return toLower(text(t)); }
			bool atEnd() const { return cur().kind == TokenKind::Eof; }
			void advance() { if(!atEnd()) i++; }
			size_t prevEnd() const { return i > 0 ? toks[i - 1].end : 0; }
			Span spanFrom(size_t begin) const { return { base + begin, base + std::max(begin, prevEnd()) }; }

			static bool isOpen(TokenKind k) { return k == TokenKind::LParen || k == TokenKind::LBrace || k == TokenKind::LBracket; }
			static bool isClose(TokenKind k) { return k == TokenKind::RParen || k == TokenKind::RBrace || k == TokenKind::RBracket; }

			void skipBalanced()
			{
				int depth = 0;
				do
				{
					auto k = cur().kind;
					if(isOpen(k)) depth++;
					else if(isClose(k)) depth--;
					advance();
				} while(depth > 0 && !atEnd());
			}

			bool isBinary(const Token &t) const
			{
				if(t.kind == TokenKind::Operator)
				{
					auto op = text(t);
					return op != "!" && op != "~";
				}
				if(t.kind == TokenKind::Ident)
				{
					auto w = lower(t);
					return w == "or" || w == "and";
				}
				return false;
			}

			void postfix(std::string word)
			{
				for(;;)
				{
					auto k = cur().kind;
					if(k == TokenKind::LBracket)
					{
						skipBalanced();
						continue;
					}
					if(k == TokenKind::LParen)
					{
						skipBalanced();
						if(word == "if" && cur().kind == TokenKind::LBrace)
						{
							skipBalanced();
							if(cur().kind == TokenKind::Ident && lower(cur()) == "else" && peek().kind == TokenKind::LBrace)
							{
								advance();
								skipBalanced();
							}
						}
						else if(word == "for" && cur().kind == TokenKind::LBrace)
							skipBalanced();
						continue;
					}
					if(k == TokenKind::Dot && peek().kind == TokenKind::Ident)
					{
						advance();
						word = lower(cur());
						advance();
						continue;
					}
					return;
				}
			}

			bool term()
			{
				const Token &t = cur();
				switch(t.kind)
				{
					case TokenKind::At:
						advance();
						return term();
					case TokenKind::Operator:
					{
						auto op = text(t);
						if(op == "-" || op == "+" || op == "!" || op == "~")
						{
							advance();
							return term();
						}
						return false;
					}
					case TokenKind::Ident:
					{
						auto w = lower(t);
						if(w == "or" || w == "and")
							return false;
						advance();
						if(w == "not")
							return term();
						postfix(w);
						return true;
					}
					case TokenKind::Variable:
						advance();
						postfix({});
						return true;
					case TokenKind::String:
					case TokenKind::IString:
					case TokenKind::Char:
					case TokenKind::Number:
						advance();
						return true;
					case TokenKind::Color:
						advance();
						if(cur().kind == TokenKind::Dot && peek().kind == TokenKind::Ident)
						{
							advance();
							advance();
							if(cur().kind == TokenKind::LParen)
								skipBalanced();
						}
						return true;
					case TokenKind::LBracket:
					case TokenKind::LParen:
					case TokenKind::LBrace:
						skipBalanced();
						return true;
					default:
						return false;
				}
			}

			bool expr()
			{
				if(!term())
					return false;
				for(;;)
				{
					const Token &t = cur();
					if(isBinary(t))
					{
						advance();
						if(!term())
							break;
					}
					else if(t.kind == TokenKind::Question)
					{
						advance();
						expr();
						if(cur().kind == TokenKind::Colon)
						{
							advance();
							expr();
						}
					}
					else
						break;
				}
				return true;
			}

			// ident, plus "-ident" parts written without spaces (cmd-line).
			Span key()
			{
				size_t b = cur().begin;
				advance();
				while(cur().kind == TokenKind::Operator && text(cur()) == "-" && !cur().hasTrivia() &&
					  peek().kind == TokenKind::Ident && !peek().hasTrivia())
				{
					advance();
					advance();
				}
				return spanFrom(b);
			}

			std::string keyText(Span s) const { return toLower(src.substr(s.begin - base, s.size())); }

			bool isAssign(const Token &t) const { return t.kind == TokenKind::Assign || t.kind == TokenKind::Colon; }

			Node import(const std::string &context, ParseMode mode)
			{
				Node n;
				n.kind = NodeKind::Import;
				n.line = cur().line;
				size_t b = cur().begin;
				n.keywordSpan = { base + cur().begin, base + cur().end };
				n.context = context;
				advance();
				if(mode == ParseMode::Config && cur().kind == TokenKind::Ident)
				{
					auto w = lower(cur());
					if(w == "lang" || w == "loc")
					{
						n.importKind = ImportKind::Lang;
						advance();
					}
				}
				size_t vb = cur().begin;
				if(!expr())
					diags.push_back({ n.line, "import path expected" });
				n.valueSpan = spanFrom(vb);
				n.span = spanFrom(b);
				return n;
			}

			Node setting(const std::string &prefix)
			{
				Node n;
				n.line = cur().line;
				size_t b = cur().begin;
				n.keySpan = key();
				auto k = keyText(n.keySpan);
				n.path = prefix.empty() ? k : prefix + "." + k;
				if(cur().kind == TokenKind::LBrace)
				{
					n.kind = NodeKind::Block;
					size_t ob = cur().begin;
					advance();
					n.children = list(ParseMode::Settings, n.path, true);
					if(cur().kind == TokenKind::RBrace)
						advance();
					else
						diags.push_back({ n.line, "'}' expected" });
					n.bodySpan = spanFrom(ob);
				}
				else if(isAssign(cur()))
				{
					n.kind = NodeKind::Assign;
					advance();
					size_t vb = cur().begin;
					if(!expr())
						diags.push_back({ cur().line, "value expected" });
					n.valueSpan = spanFrom(vb);
				}
				else
				{
					n.kind = NodeKind::Opaque;
					diags.push_back({ n.line, "'=' or '{' expected after " + k });
				}
				n.span = spanFrom(b);
				return n;
			}

			Node menuDecl()
			{
				Node n;
				n.kind = NodeKind::MenuDecl;
				n.line = cur().line;
				size_t b = cur().begin;
				n.path = lower(cur());
				n.menuKind = menuKindOf(n.path);
				n.keySpan = { base + cur().begin, base + cur().end };
				advance();
				if(cur().kind == TokenKind::LParen)
				{
					size_t pb = cur().begin;
					advance();
					while(!atEnd() && cur().kind != TokenKind::RParen)
					{
						if(cur().kind == TokenKind::Comma)
						{
							advance();
							continue;
						}
						if(cur().kind != TokenKind::Ident)
						{
							if(isOpen(cur().kind)) skipBalanced();
							else advance();
							continue;
						}
						MenuProp p;
						p.nameSpan = key();
						p.name = keyText(p.nameSpan);
						if(isAssign(cur()))
						{
							advance();
							size_t vb = cur().begin;
							expr();
							p.valueSpan = spanFrom(vb);
							p.hasValue = true;
						}
						n.props.push_back(std::move(p));
					}
					if(cur().kind == TokenKind::RParen)
						advance();
					else
						diags.push_back({ n.line, "')' expected" });
					n.propsSpan = spanFrom(pb);
				}
				if(cur().kind == TokenKind::LBrace && (n.menuKind == MenuKind::Menu || n.menuKind == MenuKind::Item))
				{
					size_t ob = cur().begin;
					if(n.menuKind == MenuKind::Menu)
					{
						advance();
						n.children = list(ParseMode::Menu, {}, true);
						if(cur().kind == TokenKind::RBrace)
							advance();
						else
							diags.push_back({ n.line, "'}' expected" });
					}
					else
						skipBalanced();  // an item's command block
					n.bodySpan = spanFrom(ob);
				}
				n.span = spanFrom(b);
				return n;
			}

			// $name = expr, name = expr (lang keys), @image, ... = expr
			Node named()
			{
				Node n;
				n.kind = NodeKind::Opaque;
				n.line = cur().line;
				size_t b = cur().begin;
				if(cur().kind == TokenKind::At)
				{
					advance();
					while(cur().kind == TokenKind::Ident || cur().kind == TokenKind::String ||
						  cur().kind == TokenKind::IString || cur().kind == TokenKind::Comma || cur().kind == TokenKind::At)
						advance();
				}
				else
				{
					n.keySpan = { base + cur().begin, base + cur().end };
					n.path = lower(cur());
					advance();
				}
				if(isAssign(cur()))
				{
					advance();
					size_t vb = cur().begin;
					expr();
					n.valueSpan = spanFrom(vb);
				}
				n.span = spanFrom(b);
				return n;
			}

			Node statement(ParseMode mode, const std::string &prefix)
			{
				const Token &t = cur();
				if(t.kind == TokenKind::Ident)
				{
					auto word = lower(t);
					if(word == "import")
						return import(prefix, mode);
					if(mode == ParseMode::Settings)
						return setting(prefix);
					if(mode == ParseMode::Lang)
						return named();
					auto first = word.substr(0, word.find('.'));
					if(mode == ParseMode::Config && (first == "theme" || first == "settings"))
						return setting({});
					if(menuKindOf(word) != MenuKind::None)
						return menuDecl();
					if((word == "lang" || word == "loc") && peek().kind == TokenKind::LBrace)
					{
						Node n;
						n.kind = NodeKind::Opaque;
						n.line = t.line;
						size_t b = t.begin;
						n.path = word;
						advance();
						skipBalanced();
						n.span = spanFrom(b);
						return n;
					}
					return named();
				}
				if(t.kind == TokenKind::Variable || t.kind == TokenKind::At)
					return named();

				Node n;
				n.kind = NodeKind::Opaque;
				n.line = t.line;
				size_t b = t.begin;
				if(isOpen(t.kind)) skipBalanced();
				else advance();
				n.span = spanFrom(b);
				return n;
			}

			std::function<void(const Token &, std::vector<Node> &, const std::string &)> scanTrivia;

			std::vector<Node> list(ParseMode mode, const std::string &prefix, bool inBlock)
			{
				std::vector<Node> nodes;
				for(;;)
				{
					if(scanTrivia)
						scanTrivia(cur(), nodes, prefix);
					if(atEnd())
						break;
					if(inBlock && cur().kind == TokenKind::RBrace)
						break;
					size_t start = i;
					Node n = statement(mode, prefix);
					if(i == start)
					{
						n.kind = NodeKind::Opaque;
						n.line = cur().line;
						size_t b = cur().begin;
						advance();
						n.span = spanFrom(b);
					}
					nodes.push_back(std::move(n));
				}
				return nodes;
			}
		};
	}

	std::string quoteImportPath(std::string_view path)
	{
		if(path.find_first_of("'@%") == std::string_view::npos && path.find("/*") == std::string_view::npos)
			return "'" + std::string(path) + "'";
		return quoteString(path);
	}

	NssDocument NssDocument::fromText(std::string text, TextFormat format, ParseContext context)
	{
		NssDocument d;
		d._text = std::move(text);
		d._format = format;
		d._context = std::move(context);
		d.reparse();
		return d;
	}

	std::optional<NssDocument> NssDocument::fromBytes(std::string_view bytes, ParseContext context, std::string *error)
	{
		auto dec = decode(bytes);
		if(!dec.ok)
		{
			if(error) *error = dec.error;
			return std::nullopt;
		}
		auto d = fromText(std::move(dec.text), dec.format, std::move(context));
		d._lossless = dec.lossless;
		return d;
	}

	std::optional<NssDocument> NssDocument::load(const std::filesystem::path &path, ParseContext context, std::string *error)
	{
		std::string bytes;
		if(!readFile(path, bytes, error))
			return std::nullopt;
		auto d = fromBytes(bytes, std::move(context), error);
		if(d)
			d->_path = path;
		return d;
	}

	std::string NssDocument::serialize() const
	{
		return encode(_text, _format);
	}

	std::string_view NssDocument::slice(Span span) const
	{
		if(span.begin >= _text.size() || span.empty())
			return {};
		return std::string_view(_text).substr(span.begin, std::min(span.end, _text.size()) - span.begin);
	}

	size_t NssDocument::lineOf(size_t offset) const
	{
		auto it = std::upper_bound(_lineStarts.begin(), _lineStarts.end(), offset);
		return size_t(it - _lineStarts.begin());
	}

	void NssDocument::reparse()
	{
		_lineStarts.clear();
		_lineStarts.push_back(0);
		for(size_t k = 0; k < _text.size(); k++)
		{
			if(_text[k] == '\n' || (_text[k] == '\r' && (k + 1 >= _text.size() || _text[k + 1] != '\n')))
				_lineStarts.push_back(k + 1);
		}

		std::vector<LexError> lexErrors;
		_tokens = NssLexer::tokenize(_text, &lexErrors);
		_diagnostics.clear();
		for(auto &e : lexErrors)
			_diagnostics.push_back({ e.line, e.message });

		TreeParser p(_text, _tokens, _diagnostics);
		std::string_view src = _text;

		// Disabled imports: a line comment that is alone on its line and holds
		// exactly one import statement, e.g. "// import 'imports/goto.nss'".
		p.scanTrivia = [this, src](const Token &tok, std::vector<Node> &out, const std::string &context)
		{
			size_t pos = tok.triviaBegin;
			while(pos < tok.begin)
			{
				if(src[pos] == '/' && pos + 1 < tok.begin && src[pos + 1] == '*')
				{
					auto e = src.find("*/", pos + 2);
					pos = e == std::string_view::npos ? tok.begin : e + 2;
					continue;
				}
				if(!(src[pos] == '/' && pos + 1 < tok.begin && src[pos + 1] == '/'))
				{
					pos++;
					continue;
				}
				size_t cb = pos;
				size_t ce = src.find_first_of("\r\n", cb);
				if(ce == std::string_view::npos || ce > tok.begin) ce = tok.begin;
				pos = ce;

				size_t ls = cb;
				while(ls > 0 && src[ls - 1] != '\n' && src[ls - 1] != '\r') ls--;
				bool alone = true;
				for(size_t k = ls; k < cb; k++)
					if(!isBlank(src[k])) { alone = false; break; }
				if(!alone) continue;

				size_t kb = cb + 2;
				while(kb < ce && isBlank(src[kb])) kb++;
				if(src.substr(kb, 6) != "import" || kb + 6 >= ce || !isBlank(src[kb + 6]))
					continue;

				auto sub = src.substr(kb, ce - kb);
				auto subToks = NssLexer::tokenize(sub);
				std::vector<Diagnostic> ignored;
				TreeParser q(sub, subToks, ignored);
				q.base = kb;
				Node n = q.import(context, _context.mode == ParseMode::Settings ? ParseMode::Settings : ParseMode::Config);
				if(!ignored.empty() || !q.atEnd() || n.valueSpan.empty())
					continue;
				if(std::any_of(subToks.begin(), subToks.end(), [](const Token &t) { return t.error; }))
					continue;
				n.enabled = false;
				n.span = { cb, ce };
				n.line = lineOf(cb);
				out.push_back(std::move(n));
			}
		};

		_nodes = p.list(_context.mode, _context.prefix, false);
	}

	void NssDocument::visit(const std::function<bool(const Node &)> &fn) const
	{
		std::function<void(const std::vector<Node> &)> walk = [&](const std::vector<Node> &nodes)
		{
			for(auto &n : nodes)
				if(fn(n))
					walk(n.children);
		};
		walk(_nodes);
	}

	std::vector<const Node *> NssDocument::assigns(std::string_view path) const
	{
		std::vector<const Node *> out;
		auto want = toLower(path);
		visit([&](const Node &n)
		{
			if(n.kind == NodeKind::Assign && (want.empty() || n.path == want))
				out.push_back(&n);
			return true;
		});
		return out;
	}

	const Node *NssDocument::findAssign(std::string_view path) const
	{
		auto all = assigns(path);
		return all.empty() ? nullptr : all.back();
	}

	const Node *NssDocument::findBlock(std::string_view path) const
	{
		const Node *found = nullptr;
		auto want = toLower(path);
		visit([&](const Node &n)
		{
			if(n.kind == NodeKind::Block && n.path == want && !n.bodySpan.empty())
				found = &n;
			return true;
		});
		return found;
	}

	std::vector<const Node *> NssDocument::imports() const
	{
		std::vector<const Node *> out;
		for(auto &n : _nodes)
			if(n.kind == NodeKind::Import)
				out.push_back(&n);
		return out;
	}

	void NssDocument::replace(Span span, std::string_view text)
	{
		span.end = std::min(span.end, _text.size());
		span.begin = std::min(span.begin, span.end);
		_text.replace(span.begin, span.size(), text);
		_modified = true;
		reparse();
	}

	void NssDocument::setText(std::string text)
	{
		_text = std::move(text);
		_modified = true;
		reparse();
	}

	std::string NssDocument::indentOf(size_t offset) const
	{
		size_t ls = _lineStarts[lineOf(offset) - 1];
		size_t e = ls;
		while(e < _text.size() && isBlank(_text[e])) e++;
		return _text.substr(ls, e - ls);
	}

	Span NssDocument::lineSpanOf(Span span, bool includeEol) const
	{
		size_t b = _lineStarts[lineOf(span.begin) - 1];
		size_t e = span.end;
		while(e < _text.size() && _text[e] != '\r' && _text[e] != '\n') e++;
		if(includeEol)
		{
			if(e < _text.size())
			{
				if(_text[e] == '\r' && e + 1 < _text.size() && _text[e + 1] == '\n') e += 2;
				else e++;
			}
			else if(b > 0)
			{
				// Last line without a line break: take the one before it instead,
				// so the file keeps ending without one.
				b--;
				if(b > 0 && _text[b] == '\n' && _text[b - 1] == '\r') b--;
			}
		}
		return { b, e };
	}

	bool NssDocument::ownsLines(Span span) const
	{
		size_t ls = _lineStarts[lineOf(span.begin) - 1];
		for(size_t k = ls; k < span.begin; k++)
			if(!isBlank(_text[k])) return false;
		size_t k = span.end;
		while(k < _text.size() && isBlank(_text[k])) k++;
		if(k + 1 < _text.size() && _text[k] == '/' && _text[k + 1] == '/')
			return true; // a trailing line comment goes along
		return k >= _text.size() || _text[k] == '\r' || _text[k] == '\n';
	}

	bool NssDocument::setValue(std::string_view path, std::string_view literal, bool force)
	{
		auto n = findAssign(path);
		if(!n || n->valueSpan.empty())
			return false;
		if(!force && !Literal::parse(slice(n->valueSpan)).isLiteral())
			return false;
		if(slice(n->valueSpan) == literal)
			return true;
		replace(n->valueSpan, literal);
		return true;
	}

	bool NssDocument::insertAssign(std::string_view blockPath, std::string_view key, std::string_view literal, std::string_view emptySep)
	{
		auto block = findBlock(blockPath);
		if(!block)
			return false;
		size_t close = block->bodySpan.end - 1;
		if(close >= _text.size() || _text[close] != '}')
			return false;

		const Node *sibling = nullptr;
		for(auto &c : block->children)
			if(c.kind == NodeKind::Assign || c.kind == NodeKind::Block || (c.kind == NodeKind::Import && c.enabled))
				sibling = &c;

		std::string indent, sep(emptySep);
		if(sibling && ownsLines({ sibling->span.begin, sibling->span.begin }))
			indent = indentOf(sibling->span.begin);
		else
		{
			// One level deeper than the block's own line; tabs unless the file
			// indents with spaces.
			std::string unit = "\t";
			for(size_t k = 1; k < _lineStarts.size(); k++)
			{
				size_t ls = _lineStarts[k];
				if(ls < _text.size() && _text[ls] == ' ')
				{
					size_t e = ls;
					while(e < _text.size() && _text[e] == ' ') e++;
					if(e < _text.size() && _text[e] != '\r' && _text[e] != '\n')
					{
						unit.assign(e - ls, ' ');
						break;
					}
				}
				else if(ls < _text.size() && _text[ls] == '\t')
					break;
			}
			indent = indentOf(block->span.begin) + unit;
		}
		if(sibling && sibling->kind == NodeKind::Assign)
		{
			auto between = slice({ sibling->keySpan.end, sibling->valueSpan.begin });
			if(!between.empty() && between.find_first_of("\r\n/") == std::string_view::npos)
				sep = between;
		}

		std::string line = indent + std::string(key) + sep + std::string(literal);
		auto eolText = std::string(eol());
		size_t ls = _lineStarts[lineOf(close) - 1];
		bool braceAlone = true;
		for(size_t k = ls; k < close; k++)
			if(!isBlank(_text[k])) { braceAlone = false; break; }

		if(braceAlone)
			replace({ ls, ls }, line + eolText);
		else
			replace({ close, close }, eolText + line + eolText + indentOf(block->span.begin));
		return true;
	}

	bool NssDocument::removeAssign(std::string_view path)
	{
		auto n = findAssign(path);
		if(!n)
			return false;
		Span s = n->span;
		if(ownsLines(s))
		{
			// Keep a trailing comment out of it: only whitespace may follow.
			size_t k = s.end;
			while(k < _text.size() && isBlank(_text[k])) k++;
			if(k < _text.size() && _text[k] == '/')
			{
				size_t b = s.begin;
				while(b > 0 && isBlank(_text[b - 1])) b--;
				replace({ b, k }, std::string_view(_text).substr(b, s.begin - b));
				return true;
			}
			replace(lineSpanOf(s, true), "");
			return true;
		}
		size_t b = s.begin;
		while(b > 0 && isBlank(_text[b - 1])) b--;
		replace({ b, s.end }, "");
		return true;
	}

	bool NssDocument::toggleImport(size_t index, bool enabled)
	{
		auto list = imports();
		if(index >= list.size())
			return false;
		auto n = list[index];
		if(n->enabled == enabled)
			return true;
		if(enabled)
		{
			replace({ n->span.begin, n->keywordSpan.begin }, "");
			return true;
		}
		if(slice(n->span).find_first_of("\r\n") != std::string_view::npos || !ownsLines(n->span))
			return false;
		replace({ n->span.begin, n->span.begin }, "// ");
		return true;
	}

	bool NssDocument::removeImport(size_t index)
	{
		auto list = imports();
		if(index >= list.size())
			return false;
		auto n = list[index];
		if(!ownsLines(n->span))
			return false;
		replace(lineSpanOf(n->span, true), "");
		return true;
	}

	bool NssDocument::appendImport(std::string_view path, ImportKind kind)
	{
		std::string stmt = "import ";
		if(kind == ImportKind::Lang)
			stmt += "lang ";
		stmt += quoteImportPath(path);
		auto eolText = std::string(eol());
		size_t end = _text.size();
		if(_text.empty() || _text.back() == '\n' || _text.back() == '\r')
			replace({ end, end }, stmt + eolText);
		else
			replace({ end, end }, eolText + stmt);
		return true;
	}
}
