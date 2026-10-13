#include "Json.h"

#include <cctype>
#include <cstdint>
#include <cstdlib>

namespace nss::json
{
	namespace
	{
		struct Reader
		{
			std::string_view s;
			size_t i = 0;
			bool ok = true;

			void ws()
			{
				while(i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++;
			}

			bool eat(char c)
			{
				ws();
				if(i < s.size() && s[i] == c) { i++; return true; }
				return false;
			}

			void utf8(std::string &out, uint32_t cp)
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

			uint32_t hex4()
			{
				if(i + 4 > s.size()) { ok = false; return 0; }
				uint32_t v = 0;
				for(int k = 0; k < 4; k++)
				{
					char c = s[i++];
					v <<= 4;
					if(c >= '0' && c <= '9') v |= uint32_t(c - '0');
					else if(c >= 'a' && c <= 'f') v |= uint32_t(c - 'a' + 10);
					else if(c >= 'A' && c <= 'F') v |= uint32_t(c - 'A' + 10);
					else ok = false;
				}
				return v;
			}

			std::string str()
			{
				std::string out;
				i++; // opening quote
				while(i < s.size() && s[i] != '"')
				{
					char c = s[i++];
					if(c != '\\') { out += c; continue; }
					if(i >= s.size()) { ok = false; break; }
					char e = s[i++];
					switch(e)
					{
						case 'n': out += '\n'; break;
						case 'r': out += '\r'; break;
						case 't': out += '\t'; break;
						case 'b': out += '\b'; break;
						case 'f': out += '\f'; break;
						case 'u':
						{
							uint32_t cp = hex4();
							if(cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u')
							{
								i += 2;
								uint32_t lo = hex4();
								cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
							}
							utf8(out, cp);
							break;
						}
						default: out += e; break;
					}
				}
				if(i < s.size()) i++; else ok = false;
				return out;
			}

			Value value(int depth = 0)
			{
				Value v;
				ws();
				if(i >= s.size() || depth > 64) { ok = false; return v; }
				char c = s[i];
				if(c == '{')
				{
					v.type = Value::Type::Object;
					i++;
					if(eat('}')) return v;
					do
					{
						ws();
						if(i >= s.size() || s[i] != '"') { ok = false; return v; }
						auto key = str();
						if(!eat(':')) { ok = false; return v; }
						v.object.emplace_back(std::move(key), value(depth + 1));
						if(!ok) return v;
					} while(eat(','));
					if(!eat('}')) ok = false;
				}
				else if(c == '[')
				{
					v.type = Value::Type::Array;
					i++;
					if(eat(']')) return v;
					do
					{
						v.array.push_back(value(depth + 1));
						if(!ok) return v;
					} while(eat(','));
					if(!eat(']')) ok = false;
				}
				else if(c == '"')
				{
					v.type = Value::Type::String;
					v.string = str();
				}
				else if(s.substr(i, 4) == "true") { v.type = Value::Type::Bool; v.boolean = true; i += 4; }
				else if(s.substr(i, 5) == "false") { v.type = Value::Type::Bool; i += 5; }
				else if(s.substr(i, 4) == "null") { i += 4; }
				else
				{
					size_t b = i;
					while(i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '-' || s[i] == '+' ||
										   s[i] == '.' || s[i] == 'e' || s[i] == 'E'))
						i++;
					if(b == i) { ok = false; return v; }
					v.type = Value::Type::Number;
					v.number = std::strtod(std::string(s.substr(b, i - b)).c_str(), nullptr);
				}
				return v;
			}
		};
	}

	std::optional<Value> parse(std::string_view text)
	{
		if(text.size() >= 3 && text.substr(0, 3) == "\xEF\xBB\xBF")
			text.remove_prefix(3);
		Reader r{ text };
		auto v = r.value();
		r.ws();
		if(!r.ok || r.i != text.size())
			return std::nullopt;
		return v;
	}

	std::string quote(std::string_view s)
	{
		std::string out = "\"";
		for(char c : s)
		{
			switch(c)
			{
				case '"': out += "\\\""; break;
				case '\\': out += "\\\\"; break;
				case '\n': out += "\\n"; break;
				case '\r': out += "\\r"; break;
				case '\t': out += "\\t"; break;
				default:
					if(static_cast<unsigned char>(c) < 0x20)
					{
						const char *hex = "0123456789abcdef";
						out += "\\u00";
						out += hex[(c >> 4) & 0xF];
						out += hex[c & 0xF];
					}
					else
						out += c;
			}
		}
		out += '"';
		return out;
	}
}
