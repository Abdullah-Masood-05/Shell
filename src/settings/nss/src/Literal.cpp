#include "nss/Literal.h"
#include "nss/NssLexer.h"
#include "nss/Text.h"

#include <charconv>
#include <cmath>
#include <cstdlib>

namespace nss
{
	namespace
	{
		int hexValue(char c)
		{
			if(c >= '0' && c <= '9') return c - '0';
			if(c >= 'a' && c <= 'f') return c - 'a' + 10;
			if(c >= 'A' && c <= 'F') return c - 'A' + 10;
			return -1;
		}

		void appendCodePoint(std::string &out, uint32_t cp)
		{
			if(cp < 0x80) out += char(cp);
			else if(cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
			else if(cp < 0x10000)
			{
				out += char(0xE0 | (cp >> 12));
				out += char(0x80 | ((cp >> 6) & 0x3F));
				out += char(0x80 | (cp & 0x3F));
			}
			else
			{
				out += char(0xF0 | (cp >> 18));
				out += char(0x80 | ((cp >> 12) & 0x3F));
				out += char(0x80 | ((cp >> 6) & 0x3F));
				out += char(0x80 | (cp & 0x3F));
			}
		}

		// Decodes a "..." token body the way Parser::parse_string does.
		bool decodeDString(std::string_view tok, std::string &out)
		{
			if(tok.size() < 2 || tok.front() != '"' || tok.back() != '"')
				return false;
			auto s = tok.substr(1, tok.size() - 2);
			for(size_t i = 0; i < s.size(); i++)
			{
				char c = s[i];
				if(c != '\\') { out += c; continue; }
				if(++i >= s.size()) return false;
				char e = s[i];
				switch(e)
				{
					case '"': case '\\': out += e; break;
					case '0': out += '\0'; break;
					case 'q': out += '"'; break;
					case 'a': out += '\''; break;
					case 'n': out += '\n'; break;
					case 'r': out += '\r'; break;
					case 't': out += '\t'; break;
					case 'v': out += '\v'; break;
					case 'f': out += '\f'; break;
					case 'b': out += '\b'; break;
					case 'x': case 'u': case 'U':
					{
						uint32_t v = 0;
						size_t max = e == 'U' ? 8 : 4, count = 0;
						while(count < max && i + 1 < s.size() && hexValue(s[i + 1]) >= 0)
						{
							v = (v << 4) | uint32_t(hexValue(s[++i]));
							count++;
						}
						if(count == 0 || v > 0x10FFFF) return false;
						appendCodePoint(out, v);
						break;
					}
					default:
						return false;
				}
			}
			return true;
		}

		struct LiteralParser
		{
			std::string_view src;
			std::vector<Token> toks;
			size_t i = 0;
			bool ok = true;

			const Token &cur() const { return toks[i]; }
			std::string_view text(const Token &t) const { return src.substr(t.begin, t.end - t.begin); }

			Literal item()
			{
				size_t first = i;
				Literal lit = itemInner();
				if(ok && i > first)
					lit.text = src.substr(toks[first].begin, toks[i - 1].end - toks[first].begin);
				return lit;
			}

			Literal itemInner()
			{
				Literal lit;
				const Token &t = cur();
				if(t.error) { ok = false; return lit; }
				switch(t.kind)
				{
					case TokenKind::Operator:
					{
						auto op = text(t);
						if((op == "-" || op == "+") && toks[i + 1].kind == TokenKind::Number && !toks[i + 1].hasTrivia())
						{
							i++;
							Literal n = item();
							if(op == "-") n.number = -n.number;
							if(n.number == 0) n.number = 0; // no "-0"
							return n;
						}
						ok = false;
						return lit;
					}
					case TokenKind::Number:
					{
						auto s = text(t);
						lit.kind = LiteralKind::Number;
						if(s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
							lit.number = double(std::strtoull(std::string(s.substr(2)).c_str(), nullptr, 16));
						else
							lit.number = std::strtod(std::string(s).c_str(), nullptr);
						i++;
						return lit;
					}
					case TokenKind::String:
					{
						lit.kind = LiteralKind::String;
						if(!decodeDString(text(t), lit.string)) { ok = false; return lit; }
						i++;
						return lit;
					}
					case TokenKind::IString:
					{
						auto s = text(t);
						if(s.size() < 2 || s.back() != s.front()) { ok = false; return lit; }
						auto body = s.substr(1, s.size() - 2);
						if(body.find_first_of("@%") != std::string_view::npos || body.find("/*") != std::string_view::npos)
						{
							ok = false;
							return lit;
						}
						lit.kind = LiteralKind::String;
						lit.string = body;
						i++;
						return lit;
					}
					case TokenKind::Color:
					{
						auto c = parseColor(text(t));
						if(!c) { ok = false; return lit; }
						lit.kind = LiteralKind::Color;
						lit.rgba = *c;
						i++;
						// #fff.opacity(50) and friends are expressions.
						if(cur().kind == TokenKind::Dot) ok = false;
						return lit;
					}
					case TokenKind::Ident:
					{
						auto word = toLower(text(t));
						static const char *keywords[] = { "true", "false", "yes", "no", "auto", "default", "none" };
						for(auto k : keywords)
						{
							if(word == k)
							{
								lit.kind = LiteralKind::Keyword;
								lit.string = word;
								i++;
								return lit;
							}
						}
						auto dot = word.find('.');
						if(dot != std::string::npos && word.find('.', dot + 1) == std::string::npos)
						{
							auto ns = word.substr(0, dot);
							if(ns == "color" || ns == "view")
							{
								lit.kind = LiteralKind::Name;
								lit.string = word;
								i++;
								return lit;
							}
						}
						ok = false;
						return lit;
					}
					case TokenKind::LBracket:
					{
						lit.kind = LiteralKind::Array;
						i++;
						if(cur().kind == TokenKind::RBracket) { i++; return lit; }
						for(;;)
						{
							lit.items.push_back(item());
							if(!ok) return lit;
							if(cur().kind == TokenKind::Comma) { i++; continue; }
							if(cur().kind == TokenKind::RBracket) { i++; return lit; }
							ok = false;
							return lit;
						}
					}
					default:
						ok = false;
						return lit;
				}
			}
		};

		const char *hexDigits = "0123456789abcdef";
	}

	std::optional<uint32_t> parseColor(std::string_view s)
	{
		if(s.empty() || s[0] != '#') return std::nullopt;
		auto h = s.substr(1);
		uint32_t v = 0;
		for(char c : h)
		{
			int x = hexValue(c);
			if(x < 0) return std::nullopt;
			v = (v << 4) | uint32_t(x);
		}
		uint32_t r, g, b, a = 0xFF;
		switch(h.size())
		{
			case 3: r = (v >> 8) & 0xF; g = (v >> 4) & 0xF; b = v & 0xF; r |= r << 4; g |= g << 4; b |= b << 4; break;
			case 4: r = (v >> 12) & 0xF; g = (v >> 8) & 0xF; b = (v >> 4) & 0xF; a = v & 0xF;
				r |= r << 4; g |= g << 4; b |= b << 4; a |= a << 4; break;
			case 5: r = (v >> 16) & 0xF; g = (v >> 12) & 0xF; b = (v >> 8) & 0xF; a = v & 0xFF;
				r |= r << 4; g |= g << 4; b |= b << 4; break;
			case 6: r = (v >> 16) & 0xFF; g = (v >> 8) & 0xFF; b = v & 0xFF; break;
			case 8: r = (v >> 24) & 0xFF; g = (v >> 16) & 0xFF; b = (v >> 8) & 0xFF; a = v & 0xFF; break;
			default: return std::nullopt;
		}
		return (r << 24) | (g << 16) | (b << 8) | a;
	}

	std::string formatNumber(double value)
	{
		if(std::isfinite(value) && value == std::floor(value) && std::fabs(value) < 1e15)
			return std::to_string(static_cast<long long>(value));
		char buf[64];
		auto res = std::to_chars(buf, buf + sizeof(buf), value);
		return std::string(buf, res.ptr);
	}

	std::string quoteString(std::string_view value)
	{
		std::string out = "\"";
		for(char c : value)
		{
			switch(c)
			{
				case '\\': out += "\\\\"; break;
				case '"': out += "\\\""; break;
				case '\n': out += "\\n"; break;
				case '\r': out += "\\r"; break;
				case '\t': out += "\\t"; break;
				case '\0': out += "\\0"; break;
				default: out += c; break;
			}
		}
		out += '"';
		return out;
	}

	bool Literal::isBool() const
	{
		if(kind == LiteralKind::Keyword)
			return string == "true" || string == "false" || string == "yes" || string == "no";
		return kind == LiteralKind::Number && (number == 0 || number == 1);
	}

	bool Literal::boolValue() const
	{
		if(kind == LiteralKind::Keyword)
			return string == "true" || string == "yes";
		return kind == LiteralKind::Number && number != 0;
	}

	Literal Literal::parse(std::string_view text)
	{
		Literal lit;
		auto t = trim(text);
		lit.text = t;
		LiteralParser p;
		p.src = t;
		p.toks = NssLexer::tokenize(t);
		if(p.toks.size() <= 1)
			return lit;
		Literal r = p.item();
		if(!p.ok || p.cur().kind != TokenKind::Eof)
			return lit;
		r.text = t;
		return r;
	}

	Literal Literal::makeNumber(double value)
	{
		Literal l;
		l.kind = LiteralKind::Number;
		l.number = value;
		l.text = formatNumber(value);
		return l;
	}

	Literal Literal::makeString(std::string_view value)
	{
		Literal l;
		l.kind = LiteralKind::String;
		l.string = value;
		l.text = quoteString(value);
		return l;
	}

	Literal Literal::makeColor(uint32_t rgba, bool forceAlpha)
	{
		Literal l;
		l.kind = LiteralKind::Color;
		l.rgba = rgba;
		int digits = (forceAlpha || (rgba & 0xFF) != 0xFF) ? 8 : 6;
		uint32_t v = digits == 8 ? rgba : rgba >> 8;
		l.text = "#";
		for(int k = digits - 1; k >= 0; k--)
			l.text += hexDigits[(v >> (k * 4)) & 0xF];
		return l;
	}

	Literal Literal::makeKeyword(std::string_view word)
	{
		Literal l;
		l.kind = LiteralKind::Keyword;
		l.string = toLower(word);
		l.text = l.string;
		return l;
	}

	Literal Literal::makeName(std::string_view dotted)
	{
		Literal l;
		l.kind = LiteralKind::Name;
		l.string = toLower(dotted);
		l.text = l.string;
		return l;
	}

	Literal Literal::makeArray(std::vector<Literal> items)
	{
		Literal l;
		l.kind = LiteralKind::Array;
		l.text = "[";
		for(size_t k = 0; k < items.size(); k++)
		{
			if(k) l.text += ", ";
			l.text += items[k].text;
		}
		l.text += "]";
		l.items = std::move(items);
		return l;
	}
}
