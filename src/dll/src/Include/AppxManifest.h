#pragma once

// The AppxManifest.xml values behind appx(id, 'executable' / 'entrypoint' /
// 'logo' / ...) and the appx.logo / appx.executable / ... forms.
//
// The manifest names a logo as "Assets\Logo.png", while the package ships
// "Assets\Logo.scale-200.png", "Assets\Logo.targetsize-48.png" and so on.
// ResolveAsset() picks the installed file closest to the screen scale, or
// returns the folder when there is no such file.
//
// Plain Win32 and std::wstring only, so it can be tested outside shell.dll.

#include <windows.h>
#include <string>
#include <cwctype>

namespace Nilesoft
{
	namespace Shell
	{
		namespace Appx
		{
			struct Manifest
			{
				bool loaded = false;
				// Full paths (the logos resolved to a real file, or their folder).
				std::wstring executable;
				std::wstring logo;
				std::wstring square150;
				std::wstring square44;
				// As written in the manifest.
				std::wstring entrypoint;
			};

			inline std::wstring ReadUtf8File(const std::wstring &path)
			{
				auto h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
									   nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
				if(h == INVALID_HANDLE_VALUE)
					return {};

				std::string bytes;
				LARGE_INTEGER size{};
				if(::GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart < 16 * 1024 * 1024)
				{
					bytes.resize(static_cast<size_t>(size.QuadPart));
					DWORD read = 0;
					if(!::ReadFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr))
						read = 0;
					bytes.resize(read);
				}
				::CloseHandle(h);

				size_t skip = 0;
				if(bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
				   static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF)
					skip = 3;
				if(bytes.size() <= skip)
					return {};

				auto src = bytes.data() + skip;
				auto len = static_cast<int>(bytes.size() - skip);
				auto n = ::MultiByteToWideChar(CP_UTF8, 0, src, len, nullptr, 0);
				if(n <= 0)
					return {};
				std::wstring text(static_cast<size_t>(n), L'\0');
				::MultiByteToWideChar(CP_UTF8, 0, src, len, text.data(), n);
				return text;
			}

			inline bool IsNameChar(wchar_t c)
			{
				return std::iswalnum(c) || c == L'_' || c == L'-' || c == L'.';
			}

			// The start tag <name ...> or <prefix:name ...> at or after `from`:
			// returns the text between '<' and '>' (empty when not found) and sets `at`
			// to the position just past it.
			inline std::wstring FindTag(const std::wstring &xml, const std::wstring &name, size_t from, size_t *at = nullptr)
			{
				for(auto pos = xml.find(name, from); pos != std::wstring::npos; pos = xml.find(name, pos + 1))
				{
					auto end = pos + name.size();
					if(end >= xml.size() || IsNameChar(xml[end]) || xml[end] == L':')
						continue;
					if(pos == 0)
						continue;

					auto open = pos - 1;
					if(xml[open] == L':')
					{
						// Skip the namespace prefix back to '<'.
						while(open > 0 && IsNameChar(xml[open - 1]))
							open--;
						if(open == 0 || open == pos - 1)
							continue;
						open--;
					}
					if(xml[open] != L'<')
						continue;

					auto close = xml.find(L'>', end);
					if(close == std::wstring::npos)
						return {};
					if(at)
						*at = close + 1;
					return xml.substr(open + 1, close - open - 1);
				}
				return {};
			}

			inline std::wstring Unescape(std::wstring s)
			{
				static const struct { const wchar_t *from; wchar_t to; } entities[] = {
					{ L"&lt;", L'<' }, { L"&gt;", L'>' }, { L"&quot;", L'"' }, { L"&apos;", L'\'' }, { L"&amp;", L'&' },
				};
				for(auto &e : entities)
				{
					std::wstring from = e.from;
					for(auto pos = s.find(from); pos != std::wstring::npos; pos = s.find(from, pos + 1))
						s.replace(pos, from.size(), 1, e.to);
				}
				return s;
			}

			// The value of attribute `name` (no prefix) in a start tag's text.
			inline std::wstring Attribute(const std::wstring &tag, const std::wstring &name)
			{
				for(auto pos = tag.find(name); pos != std::wstring::npos; pos = tag.find(name, pos + 1))
				{
					if(pos == 0 || !std::iswspace(tag[pos - 1]))
						continue;
					auto i = pos + name.size();
					while(i < tag.size() && std::iswspace(tag[i])) i++;
					if(i >= tag.size() || tag[i] != L'=')
						continue;
					i++;
					while(i < tag.size() && std::iswspace(tag[i])) i++;
					if(i >= tag.size() || (tag[i] != L'"' && tag[i] != L'\''))
						continue;
					auto quote = tag[i++];
					auto end = tag.find(quote, i);
					if(end == std::wstring::npos)
						return {};
					return Unescape(tag.substr(i, end - i));
				}
				return {};
			}

			// The text of the first <name>text</name> after `from`.
			inline std::wstring ElementText(const std::wstring &xml, const std::wstring &name, size_t from)
			{
				size_t at = 0;
				auto tag = FindTag(xml, name, from, &at);
				if(tag.empty() || tag.back() == L'/')
					return {};
				auto end = xml.find(L'<', at);
				if(end == std::wstring::npos)
					return {};
				auto text = xml.substr(at, end - at);
				auto first = text.find_first_not_of(L" \t\r\n");
				auto last = text.find_last_not_of(L" \t\r\n");
				if(first == std::wstring::npos)
					return {};
				return Unescape(text.substr(first, last - first + 1));
			}

			inline std::wstring Combine(const std::wstring &root, std::wstring rel)
			{
				for(auto &c : rel)
					if(c == L'/') c = L'\\';
				while(!rel.empty() && rel.front() == L'\\')
					rel.erase(0, 1);
				if(rel.empty())
					return {};
				auto r = root;
				while(!r.empty() && (r.back() == L'\\' || r.back() == L'/'))
					r.pop_back();
				return r + L"\\" + rel;
			}

			// The scale Windows picks assets for: 100 at 96 dpi, 150 at 144 dpi, ...
			inline unsigned ScreenScale()
			{
				unsigned scale = 100;
				if(auto hdc = ::GetDC(nullptr))
				{
					auto dpi = ::GetDeviceCaps(hdc, LOGPIXELSY);
					if(dpi > 0)
						scale = static_cast<unsigned>(dpi) * 100 / 96;
					::ReleaseDC(nullptr, hdc);
				}
				return scale;
			}

			inline unsigned QualifierNumber(const std::wstring &q, const wchar_t *prefix)
			{
				std::wstring p = prefix;
				auto pos = q.find(p);
				if(pos == std::wstring::npos || (pos > 0 && q[pos - 1] != L'_'))
					return 0;
				return static_cast<unsigned>(std::wcstoul(q.c_str() + pos + p.size(), nullptr, 10));
			}

			// root\rel if that file exists, else the installed variant of it
			// (rel's name with resource qualifiers such as .scale-200 or
			// .targetsize-48 before the extension) closest to `scale`, else rel's
			// folder if it exists, else root.
			inline std::wstring ResolveAsset(const std::wstring &root, const std::wstring &rel, unsigned scale)
			{
				auto full = Combine(root, rel);
				if(full.empty())
					return {};

				auto attr = ::GetFileAttributesW(full.c_str());
				if(attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY))
					return full;

				auto slash = full.find_last_of(L'\\');
				auto dir = full.substr(0, slash);
				auto name = full.substr(slash + 1);
				auto dot = name.find_last_of(L'.');
				auto stem = dot == std::wstring::npos ? name : name.substr(0, dot);
				auto ext = dot == std::wstring::npos ? std::wstring() : name.substr(dot);

				std::wstring best;
				unsigned best_penalty = ~0u, best_distance = ~0u;

				WIN32_FIND_DATAW fd{};
				auto hfind = ::FindFirstFileExW((dir + L"\\" + stem + L".*" + ext).c_str(), FindExInfoBasic, &fd,
												FindExSearchNameMatch, nullptr, 0);
				if(hfind != INVALID_HANDLE_VALUE)
				{
					do
					{
						if(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
							continue;
						std::wstring file = fd.cFileName;
						if(file.size() <= stem.size() + 1 + ext.size())
							continue;
						auto q = file.substr(stem.size() + 1, file.size() - stem.size() - 1 - ext.size());
						for(auto &c : q)
							c = static_cast<wchar_t>(std::towlower(c));

						// High-contrast variants last, unplated/light-theme ones after the plain ones.
						unsigned penalty = 0;
						if(q.find(L"contrast-") != std::wstring::npos)
							penalty = 2;
						else if(q.find(L"altform-") != std::wstring::npos || q.find(L"theme-") != std::wstring::npos)
							penalty = 1;

						unsigned distance;
						if(auto s = QualifierNumber(q, L"scale-"); s)
							distance = s >= scale ? s - scale : 1000 + (scale - s);
						else if(auto t = QualifierNumber(q, L"targetsize-"); t)
							distance = 2000 + (t >= 256 ? 0 : 256 - t);
						else
							distance = 3000;

						if(penalty < best_penalty || (penalty == best_penalty && distance < best_distance))
						{
							best_penalty = penalty;
							best_distance = distance;
							best = dir + L"\\" + file;
						}
					} while(::FindNextFileW(hfind, &fd));
					::FindClose(hfind);
				}

				if(!best.empty())
					return best;

				attr = ::GetFileAttributesW(dir.c_str());
				if(attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
					return dir;
				return root;
			}

			inline Manifest Parse(const std::wstring &xml, const std::wstring &root, unsigned scale)
			{
				Manifest m;
				m.loaded = true;
				if(xml.empty())
					return m;

				// Package-level logo: <Properties><Logo>Assets\StoreLogo.png</Logo>.
				size_t at = 0;
				if(!FindTag(xml, L"Properties", 0, &at).empty())
				{
					auto logo = ElementText(xml, L"Logo", at);
					if(!logo.empty())
						m.logo = ResolveAsset(root, logo, scale);
				}

				// The first application: <Application Executable=".." EntryPoint="..">
				// with <uap:VisualElements Square150x150Logo=".." Square44x44Logo="..">.
				at = 0;
				auto app = FindTag(xml, L"Application", 0, &at);
				if(!app.empty())
				{
					auto exe = Attribute(app, L"Executable");
					if(!exe.empty())
						m.executable = Combine(root, exe);
					m.entrypoint = Attribute(app, L"EntryPoint");

					auto visual = FindTag(xml, L"VisualElements", at);
					if(!visual.empty())
					{
						auto s150 = Attribute(visual, L"Square150x150Logo");
						if(!s150.empty())
							m.square150 = ResolveAsset(root, s150, scale);
						auto s44 = Attribute(visual, L"Square44x44Logo");
						if(!s44.empty())
							m.square44 = ResolveAsset(root, s44, scale);
					}
				}
				return m;
			}

			inline Manifest Load(const std::wstring &root)
			{
				if(root.empty())
				{
					Manifest m;
					m.loaded = true;
					return m;
				}
				return Parse(ReadUtf8File(Combine(root, L"AppxManifest.xml")), root, ScreenScale());
			}
		}
	}
}
