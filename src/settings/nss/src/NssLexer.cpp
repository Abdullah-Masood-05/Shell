#include "nss/NssLexer.h"

namespace nss
{
	namespace
	{
		bool isHex(char c)
		{
			return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
		}

		bool isDigit(char c) { return c >= '0' && c <= '9'; }

		bool isSpace(char c)
		{
			return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
		}

		struct Scanner
		{
			std::string_view s;
			std::vector<LexError> *errors = nullptr;
			bool error = false;

			char at(size_t i) const { return i < s.size() ? s[i] : '\0'; }

			void fail(size_t offset, const char *message)
			{
				error = true;
				if(errors)
				{
					size_t line = 1;
					for(size_t i = 0; i < offset && i < s.size(); i++)
						if(s[i] == '\n') line++;
					errors->push_back({ offset, line, message });
				}
			}

			// pos is at "/*"; returns the position after "*/" (or the end).
			size_t blockComment(size_t pos)
			{
				size_t start = pos;
				pos += 2;
				while(pos < s.size())
				{
					if(s[pos] == '*' && at(pos + 1) == '/')
						return pos + 2;
					pos++;
				}
				fail(start, "'*/' expected");
				return pos;
			}

			// pos is at "//"; the comment ends before the line break.
			size_t lineComment(size_t pos)
			{
				while(pos < s.size() && s[pos] != '\n' && s[pos] != '\r')
					pos++;
				return pos;
			}

			size_t trivia(size_t pos)
			{
				for(;;)
				{
					if(pos >= s.size()) return pos;
					char c = s[pos];
					if(isSpace(c)) pos++;
					else if(c == '/' && at(pos + 1) == '/') pos = lineComment(pos);
					else if(c == '/' && at(pos + 1) == '*') pos = blockComment(pos);
					else return pos;
				}
			}

			size_t ident(size_t pos)
			{
				while(pos < s.size())
				{
					char c = s[pos];
					if(NssLexer::isIdentChar(c))
						pos++;
					else if(c == '.' && NssLexer::isIdentStart(at(pos + 1)))
						pos++;
					else
						break;
				}
				return pos;
			}

			// pos is at '"'. Escapes as in Parser::parse_string; no line breaks.
			size_t dstring(size_t pos)
			{
				size_t start = pos++;
				while(pos < s.size())
				{
					char c = s[pos];
					if(c == '"')
						return pos + 1;
					if(c == '\n' || c == '\r')
					{
						fail(start, "newline in string");
						return pos;
					}
					if(c == '\\')
					{
						char e = at(pos + 1);
						switch(e)
						{
							case '"': case '\\': case '0': case 'q': case 'a': case 'n':
							case 'r': case 't': case 'v': case 'f': case 'b':
								pos += 2;
								break;
							case 'x': case 'u': case 'U':
							{
								pos += 2;
								size_t max = 4, count = 0;
								if(e == 'U') max = 8;
								while(count < max && isHex(at(pos))) { pos++; count++; }
								if(count == 0 || (e != 'x' && count < max && e != 'U'))
									fail(pos, "illegal escape");
								break;
							}
							default:
								fail(pos, "illegal escape");
								pos += (e == '\0' || e == '\n' || e == '\r') ? 1 : 2;
								break;
						}
						continue;
					}
					pos++;
				}
				fail(start, "string not terminated");
				return pos;
			}

			// pos is at an opening bracket; returns the position after its
			// matching closer, skipping strings and comments inside.
			size_t balanced(size_t pos)
			{
				size_t start = pos;
				int depth = 0;
				while(pos < s.size())
				{
					char c = s[pos];
					switch(c)
					{
						case '(': case '[': case '{':
							depth++;
							pos++;
							break;
						case ')': case ']': case '}':
							depth--;
							pos++;
							if(depth <= 0)
								return pos;
							break;
						case '"':
							pos = dstring(pos);
							break;
						case '\'': case '`':
							pos = istring(pos);
							break;
						case '/':
							if(at(pos + 1) == '/') pos = lineComment(pos);
							else if(at(pos + 1) == '*') pos = blockComment(pos);
							else pos++;
							break;
						default:
							pos++;
							break;
					}
				}
				fail(start, "closing bracket expected");
				return pos;
			}

			// pos is just after '@' inside an interpolated string.
			size_t interpolation(size_t pos)
			{
				char c = at(pos);
				if(c == '"')
					return dstring(pos);
				if(c == '(' || c == '{')
					return balanced(pos);
				if(c == '@')
					return pos + 1;
				if(!NssLexer::isIdentStart(c))
					return pos;
				for(;;)
				{
					pos = ident(pos);
					bool call = false;
					if(at(pos) == '[')
						pos = balanced(pos);
					if(at(pos) == '(')
					{
						pos = balanced(pos);
						call = true;
					}
					if(call && at(pos) == '.' && NssLexer::isIdentStart(at(pos + 1)))
					{
						pos++;
						continue;
					}
					return pos;
				}
			}

			// pos is at the opening quote (' or `).
			size_t istring(size_t pos)
			{
				size_t start = pos;
				char q = s[pos++];
				while(pos < s.size())
				{
					char c = s[pos];
					if(c == q)
						return pos + 1;
					if(c == '@')
						pos = interpolation(pos + 1);
					else if(c == '/' && at(pos + 1) == '*')
						pos = blockComment(pos); // the core skips these inside '...'
					else
						pos++;
				}
				fail(start, "closing quote expected");
				return pos;
			}

			size_t utf8Char(size_t pos)
			{
				auto c = static_cast<unsigned char>(s[pos]);
				size_t len = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
				return pos + len <= s.size() ? pos + len : s.size();
			}
		};
	}

	bool NssLexer::isIdentStart(char c)
	{
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || static_cast<unsigned char>(c) >= 0x80;
	}

	bool NssLexer::isIdentChar(char c)
	{
		return isIdentStart(c) || isDigit(c);
	}

	const char *NssLexer::kindName(TokenKind kind)
	{
		switch(kind)
		{
			case TokenKind::Ident: return "ident";
			case TokenKind::Number: return "number";
			case TokenKind::Color: return "color";
			case TokenKind::String: return "string";
			case TokenKind::IString: return "istring";
			case TokenKind::Char: return "char";
			case TokenKind::Variable: return "variable";
			case TokenKind::At: return "@";
			case TokenKind::Operator: return "operator";
			case TokenKind::Dot: return ".";
			case TokenKind::Assign: return "=";
			case TokenKind::Colon: return ":";
			case TokenKind::Question: return "?";
			case TokenKind::Comma: return ",";
			case TokenKind::LParen: return "(";
			case TokenKind::RParen: return ")";
			case TokenKind::LBrace: return "{";
			case TokenKind::RBrace: return "}";
			case TokenKind::LBracket: return "[";
			case TokenKind::RBracket: return "]";
			case TokenKind::Eof: return "eof";
			default: return "unknown";
		}
	}

	std::vector<Token> NssLexer::tokenize(std::string_view src, std::vector<LexError> *errors)
	{
		std::vector<Token> tokens;
		Scanner sc{ src, errors };
		size_t pos = 0, line = 1, counted = 0;

		auto lineAt = [&](size_t offset)
		{
			for(; counted < offset; counted++)
				if(src[counted] == '\n') line++;
			return line;
		};

		for(;;)
		{
			Token t;
			t.triviaBegin = pos;
			pos = sc.trivia(pos);
			t.begin = pos;
			t.line = lineAt(pos);
			sc.error = false;

			if(pos >= src.size())
			{
				t.kind = TokenKind::Eof;
				t.end = pos;
				tokens.push_back(t);
				break;
			}

			char c = src[pos];
			char n1 = sc.at(pos + 1);
			auto two = [&](char a, char b) { return c == a && n1 == b; };

			if(NssLexer::isIdentStart(c))
			{
				t.kind = TokenKind::Ident;
				pos = sc.ident(pos);
			}
			else if(isDigit(c) || (c == '.' && isDigit(n1)))
			{
				t.kind = TokenKind::Number;
				if(c == '0' && (n1 == 'x' || n1 == 'X'))
				{
					pos += 2;
					size_t start = pos;
					while(isHex(sc.at(pos))) pos++;
					if(pos == start) sc.fail(pos, "hexadecimal digit expected");
				}
				else
				{
					while(isDigit(sc.at(pos))) pos++;
					if(sc.at(pos) == '.' && isDigit(sc.at(pos + 1)))
					{
						pos++;
						while(isDigit(sc.at(pos))) pos++;
					}
				}
			}
			else if(c == '"')
			{
				t.kind = TokenKind::String;
				pos = sc.dstring(pos);
			}
			else if(c == '\'' || c == '`')
			{
				t.kind = TokenKind::IString;
				pos = sc.istring(pos);
			}
			else if(c == '#' && isHex(n1))
			{
				t.kind = TokenKind::Color;
				pos++;
				size_t start = pos;
				while(isHex(sc.at(pos))) pos++;
				size_t count = pos - start;
				if(!((count >= 3 && count <= 6) || count == 8))
					sc.fail(start, "invalid color");
			}
			else if(c == '$' && NssLexer::isIdentStart(n1))
			{
				t.kind = TokenKind::Variable;
				pos++;
				while(NssLexer::isIdentChar(sc.at(pos))) pos++;
			}
			else if(c == '\\' && (n1 == 'u' || n1 == 'U'))
			{
				t.kind = TokenKind::Char;
				pos += 2;
				size_t start = pos;
				while(pos - start < 4 && isHex(sc.at(pos))) pos++;
				if(pos == start) sc.fail(pos, "illegal escape");
			}
			else if(two('=', '=') || two('!', '=') || two('<', '=') || two('>', '=') || two('&', '&') ||
					two('|', '|') || two('<', '<') || two('>', '>') || two('+', '=') || two('-', '='))
			{
				t.kind = TokenKind::Operator;
				pos += 2;
			}
			else
			{
				pos++;
				switch(c)
				{
					case '+': case '-': case '*': case '/': case '%': case '<': case '>':
					case '&': case '|': case '^': case '!': case '~':
						t.kind = TokenKind::Operator; break;
					case '@': t.kind = TokenKind::At; break;
					case '.': t.kind = TokenKind::Dot; break;
					case '=': t.kind = TokenKind::Assign; break;
					case ':': t.kind = TokenKind::Colon; break;
					case '?': t.kind = TokenKind::Question; break;
					case ',': t.kind = TokenKind::Comma; break;
					case '(': t.kind = TokenKind::LParen; break;
					case ')': t.kind = TokenKind::RParen; break;
					case '{': t.kind = TokenKind::LBrace; break;
					case '}': t.kind = TokenKind::RBrace; break;
					case '[': t.kind = TokenKind::LBracket; break;
					case ']': t.kind = TokenKind::RBracket; break;
					default:
						t.kind = TokenKind::Unknown;
						pos = sc.utf8Char(pos - 1);
						break;
				}
			}

			t.end = pos;
			t.error = sc.error;
			tokens.push_back(t);
		}
		return tokens;
	}
}
