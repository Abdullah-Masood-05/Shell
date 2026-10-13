#include "nss/Text.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <fstream>

namespace nss
{
	namespace
	{
		void appendUtf8(std::string &out, uint32_t cp)
		{
			if(cp < 0x80)
				out += char(cp);
			else if(cp < 0x800)
			{
				out += char(0xC0 | (cp >> 6));
				out += char(0x80 | (cp & 0x3F));
			}
			else if(cp < 0x10000)
			{
				// Lone surrogates land here too (WTF-8).
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

		// Strict UTF-8 check: rejects overlongs, surrogates and > U+10FFFF.
		bool isValidUtf8(std::string_view s)
		{
			auto p = reinterpret_cast<const uint8_t *>(s.data());
			size_t n = s.size(), i = 0;
			while(i < n)
			{
				uint8_t c = p[i];
				if(c < 0x80) { i++; continue; }
				size_t len = 0;
				uint32_t cp = 0;
				if(c >= 0xC2 && c <= 0xDF) { len = 2; cp = c & 0x1F; }
				else if(c >= 0xE0 && c <= 0xEF) { len = 3; cp = c & 0x0F; }
				else if(c >= 0xF0 && c <= 0xF4) { len = 4; cp = c & 0x07; }
				else return false;
				if(i + len > n) return false;
				for(size_t k = 1; k < len; k++)
				{
					if((p[i + k] & 0xC0) != 0x80) return false;
					cp = (cp << 6) | (p[i + k] & 0x3F);
				}
				if((len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)))
					return false;
				if(cp >= 0xD800 && cp <= 0xDFFF)
					return false;
				i += len;
			}
			return true;
		}

		// UTF-16 code units to UTF-8, keeping unpaired surrogates (WTF-8).
		std::string utf16ToUtf8(const uint16_t *u, size_t n)
		{
			std::string out;
			out.reserve(n);
			for(size_t i = 0; i < n; i++)
			{
				uint32_t c = u[i];
				if(c >= 0xD800 && c <= 0xDBFF && i + 1 < n && u[i + 1] >= 0xDC00 && u[i + 1] <= 0xDFFF)
				{
					c = 0x10000 + ((c - 0xD800) << 10) + (u[i + 1] - 0xDC00);
					i++;
				}
				appendUtf8(out, c);
			}
			return out;
		}

		// UTF-8 (WTF-8) to UTF-16 code units; invalid bytes become U+FFFD.
		std::u16string utf8ToUtf16(std::string_view s)
		{
			std::u16string out;
			out.reserve(s.size());
			auto p = reinterpret_cast<const uint8_t *>(s.data());
			size_t n = s.size(), i = 0;
			while(i < n)
			{
				uint8_t c = p[i];
				uint32_t cp = 0;
				size_t len = 1;
				if(c < 0x80) cp = c;
				else if(c >= 0xC0 && c <= 0xDF) { len = 2; cp = c & 0x1F; }
				else if(c >= 0xE0 && c <= 0xEF) { len = 3; cp = c & 0x0F; }
				else if(c >= 0xF0 && c <= 0xF7) { len = 4; cp = c & 0x07; }
				else { out += char16_t(0xFFFD); i++; continue; }
				if(i + len > n) { out += char16_t(0xFFFD); i++; continue; }
				bool bad = false;
				for(size_t k = 1; k < len; k++)
				{
					if((p[i + k] & 0xC0) != 0x80) { bad = true; break; }
					cp = (cp << 6) | (p[i + k] & 0x3F);
				}
				if(bad) { out += char16_t(0xFFFD); i++; continue; }
				i += len;
				if(cp >= 0x10000)
				{
					cp -= 0x10000;
					out += char16_t(0xD800 + (cp >> 10));
					out += char16_t(0xDC00 + (cp & 0x3FF));
				}
				else
					out += char16_t(cp);
			}
			return out;
		}
	}

	TextFormat defaultFormat()
	{
		TextFormat f;
		f.encoding = Encoding::Utf8;
		f.bom = true;
		f.eol = Eol::CRLF;
		return f;
	}

	std::string_view eolString(Eol eol)
	{
		switch(eol)
		{
			case Eol::LF: return "\n";
			case Eol::CR: return "\r";
			default: return "\r\n";
		}
	}

	const char *encodingName(Encoding encoding)
	{
		switch(encoding)
		{
			case Encoding::Utf16LE: return "UTF-16LE";
			case Encoding::Utf16BE: return "UTF-16BE";
			case Encoding::Ansi: return "ANSI";
			default: return "UTF-8";
		}
	}

	Eol detectEol(std::string_view text, bool *mixed)
	{
		size_t crlf = 0, lf = 0, cr = 0;
		for(size_t i = 0; i < text.size(); i++)
		{
			if(text[i] == '\r')
			{
				if(i + 1 < text.size() && text[i + 1] == '\n') { crlf++; i++; }
				else cr++;
			}
			else if(text[i] == '\n')
				lf++;
		}
		if(mixed)
			*mixed = (crlf > 0) + (lf > 0) + (cr > 0) > 1;
		if(lf > crlf && lf >= cr) return Eol::LF;
		if(cr > crlf && cr > lf) return Eol::CR;
		return Eol::CRLF;
	}

	DecodedText decode(std::string_view bytes)
	{
		DecodedText r;
		auto b = reinterpret_cast<const uint8_t *>(bytes.data());
		size_t n = bytes.size();

		auto fromUtf16 = [&](size_t offset, bool bigEndian)
		{
			size_t units = (n - offset) / 2;
			std::u16string u(units, u'\0');
			for(size_t i = 0; i < units; i++)
			{
				uint8_t lo = b[offset + i * 2], hi = b[offset + i * 2 + 1];
				if(bigEndian) std::swap(lo, hi);
				u[i] = char16_t(lo | (hi << 8));
			}
			r.text = utf16ToUtf8(reinterpret_cast<const uint16_t *>(u.data()), u.size());
			r.format.encoding = bigEndian ? Encoding::Utf16BE : Encoding::Utf16LE;
			r.lossless = ((n - offset) % 2) == 0;
		};

		if(n >= 2 && b[0] == 0xFF && b[1] == 0xFE)
		{
			r.format.bom = true;
			fromUtf16(2, false);
		}
		else if(n >= 2 && b[0] == 0xFE && b[1] == 0xFF)
		{
			r.format.bom = true;
			fromUtf16(2, true);
		}
		else if(n >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF)
		{
			// Invalid sequences after a UTF-8 BOM are kept as they are, so
			// encode() still writes the same bytes back.
			r.format.bom = true;
			r.text.assign(bytes.substr(3));
		}
		else if(n >= 4 && b[0] == 0 && b[1] == 0 && b[2] == 0xFE && b[3] == 0xFF)
		{
			r.error = "UTF-32 files are not supported";
			return r;
		}
		else if(n >= 4 && b[0] == 0x2B && b[1] == 0x2F && b[2] == 0x76 &&
				(b[3] == 0x38 || b[3] == 0x39 || b[3] == 0x2B || b[3] == 0x2F))
		{
			r.error = "UTF-7 files are not supported";
			return r;
		}
		else if(n >= 4 && b[0] && !b[1] && b[2] && !b[3])
			fromUtf16(0, false);
		else if(n >= 4 && !b[0] && b[1] && !b[2] && b[3])
			fromUtf16(0, true);
		else if(isValidUtf8(bytes))
			r.text.assign(bytes);
		else
		{
			r.format.encoding = Encoding::Ansi;
			if(n > 0)
			{
				int wlen = ::MultiByteToWideChar(CP_ACP, 0, bytes.data(), int(n), nullptr, 0);
				std::wstring w(size_t(wlen), L'\0');
				::MultiByteToWideChar(CP_ACP, 0, bytes.data(), int(n), w.data(), wlen);
				r.text = narrow(w);
			}
		}

		r.format.eol = detectEol(r.text, &r.format.mixedEol);
		if(r.lossless)
			r.lossless = encode(r.text, r.format) == bytes;
		r.ok = true;
		return r;
	}

	std::string encode(std::string_view utf8, const TextFormat &format)
	{
		std::string out;
		switch(format.encoding)
		{
			case Encoding::Utf16LE:
			case Encoding::Utf16BE:
			{
				bool be = format.encoding == Encoding::Utf16BE;
				auto u = utf8ToUtf16(utf8);
				out.reserve(u.size() * 2 + 2);
				if(format.bom)
					out += be ? std::string("\xFE\xFF", 2) : std::string("\xFF\xFE", 2);
				for(char16_t c : u)
				{
					char lo = char(c & 0xFF), hi = char(c >> 8);
					if(be) { out += hi; out += lo; }
					else { out += lo; out += hi; }
				}
				break;
			}
			case Encoding::Ansi:
			{
				auto w = widen(utf8);
				if(!w.empty())
				{
					int len = ::WideCharToMultiByte(CP_ACP, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
					out.resize(size_t(len));
					::WideCharToMultiByte(CP_ACP, 0, w.data(), int(w.size()), out.data(), len, nullptr, nullptr);
				}
				break;
			}
			default:
				if(format.bom)
					out = "\xEF\xBB\xBF";
				out.append(utf8);
				break;
		}
		return out;
	}

	std::wstring widen(std::string_view utf8)
	{
		auto u = utf8ToUtf16(utf8);
		return std::wstring(u.begin(), u.end());
	}

	std::string narrow(std::wstring_view wide)
	{
		return utf16ToUtf8(reinterpret_cast<const uint16_t *>(wide.data()), wide.size());
	}

	bool readFile(const std::filesystem::path &path, std::string &bytes, std::string *error)
	{
		std::ifstream f(path, std::ios::binary);
		if(!f)
		{
			if(error) *error = "cannot open " + narrow(path.wstring());
			return false;
		}
		bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
		return true;
	}

	std::string toLower(std::string_view s)
	{
		std::string r(s);
		for(auto &c : r)
			if(c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
		return r;
	}

	bool iequals(std::string_view a, std::string_view b)
	{
		if(a.size() != b.size()) return false;
		for(size_t i = 0; i < a.size(); i++)
		{
			char x = a[i], y = b[i];
			if(x >= 'A' && x <= 'Z') x = char(x - 'A' + 'a');
			if(y >= 'A' && y <= 'Z') y = char(y - 'A' + 'a');
			if(x != y) return false;
		}
		return true;
	}

	std::string_view trim(std::string_view s)
	{
		size_t b = 0, e = s.size();
		while(b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) b++;
		while(e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) e--;
		return s.substr(b, e - b);
	}

	std::string pathKey(const std::filesystem::path &path)
	{
		auto p = path.lexically_normal().wstring();
		for(auto &c : p)
			if(c == L'/') c = L'\\';
		auto s = narrow(p);
		return toLower(s);
	}
}
