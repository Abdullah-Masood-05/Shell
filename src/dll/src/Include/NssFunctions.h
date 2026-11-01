#pragma once

// Helpers behind some NSS functions, kept free of Shell's own types so a
// standalone test can check them.

#include <windows.h>
#include <shlwapi.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Nilesoft
{
	namespace Shell
	{
		// Shell's string as std::wstring. Its c_str() is null when it is empty,
		// which std::wstring cannot take.
		template<class S>
		std::wstring as_wstring(const S &s)
		{
			auto p = s.c_str();
			return p ? std::wstring(p, s.length()) : std::wstring();
		}

		namespace StrFormat
		{
			// str.fmt / str.format: each {N} becomes argument N, in one pass, so
			// an argument that itself contains "{1}" is left as it is. A {N} with
			// no matching argument, or anything else in braces, stays as written.
			inline std::wstring format(std::wstring_view fmt, const std::vector<std::wstring> &args)
			{
				std::wstring out;
				out.reserve(fmt.size());

				for(size_t i = 0; i < fmt.size(); i++)
				{
					if(fmt[i] == L'{')
					{
						size_t j = i + 1;
						size_t index = 0;
						bool too_big = false;
						while(j < fmt.size() && fmt[j] >= L'0' && fmt[j] <= L'9')
						{
							index = index * 10 + size_t(fmt[j] - L'0');
							too_big |= index > args.size();
							j++;
						}

						if(j > i + 1 && j < fmt.size() && fmt[j] == L'}' && !too_big && index < args.size())
						{
							out += args[index];
							i = j;
							continue;
						}
					}
					out += fmt[i];
				}
				return out;
			}
		}

		// app.set / app.get / app.remove (app.delete) / app.exists: text values
		// that last between menus, stored as REG_SZ under HKCU. The subkey is a
		// parameter only so a test can use a scratch key.
		namespace AppVariables
		{
			constexpr const wchar_t *Key = L"Software\\Nilesoft\\Shell\\Variables";

			// An empty name would be the key's default value; it is not a variable.
			inline bool set(const std::wstring &name, const std::wstring &value, const wchar_t *subkey = Key)
			{
				if(name.empty())
					return false;

				HKEY hKey = nullptr;
				if(::RegCreateKeyExW(HKEY_CURRENT_USER, subkey, 0, nullptr, REG_OPTION_NON_VOLATILE,
									 KEY_SET_VALUE, nullptr, &hKey, nullptr) != ERROR_SUCCESS)
					return false;

				auto bytes = DWORD((value.size() + 1) * sizeof(wchar_t));
				bool ok = ::RegSetValueExW(hKey, name.c_str(), 0, REG_SZ,
										   reinterpret_cast<const BYTE *>(value.c_str()), bytes) == ERROR_SUCCESS;
				::RegCloseKey(hKey);
				return ok;
			}

			// False when the value is missing or is not text (REG_SZ or REG_EXPAND_SZ,
			// returned as stored). The buffer is sized from the value itself.
			inline bool get(const std::wstring &name, std::wstring &out, const wchar_t *subkey = Key)
			{
				if(name.empty())
					return false;

				HKEY hKey = nullptr;
				if(::RegOpenKeyExW(HKEY_CURRENT_USER, subkey, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS)
					return false;

				bool ok = false;
				// Another write can grow the value between the two calls; try again then.
				for(int attempt = 0; attempt < 3; attempt++)
				{
					DWORD type = 0, size = 0;
					if(::RegQueryValueExW(hKey, name.c_str(), nullptr, &type, nullptr, &size) != ERROR_SUCCESS ||
					   (type != REG_SZ && type != REG_EXPAND_SZ))
						break;

					// One more character than the data holds, for a value stored without its terminator.
					std::wstring buffer(size / sizeof(wchar_t) + 1, L'\0');
					auto capacity = DWORD(buffer.size() * sizeof(wchar_t));
					auto status = ::RegQueryValueExW(hKey, name.c_str(), nullptr, &type,
													 reinterpret_cast<BYTE *>(buffer.data()), &capacity);
					if(status == ERROR_MORE_DATA)
						continue;
					if(status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ))
						break;

					buffer.resize(capacity / sizeof(wchar_t));
					if(auto end = buffer.find(L'\0'); end != std::wstring::npos)
						buffer.resize(end);
					out = std::move(buffer);
					ok = true;
					break;
				}
				::RegCloseKey(hKey);
				return ok;
			}

			inline bool remove(const std::wstring &name, const wchar_t *subkey = Key)
			{
				if(name.empty())
					return false;

				HKEY hKey = nullptr;
				if(::RegOpenKeyExW(HKEY_CURRENT_USER, subkey, 0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS)
					return false;
				bool ok = ::RegDeleteValueW(hKey, name.c_str()) == ERROR_SUCCESS;
				::RegCloseKey(hKey);
				return ok;
			}

			inline bool exists(const std::wstring &name, const wchar_t *subkey = Key)
			{
				if(name.empty())
					return false;

				HKEY hKey = nullptr;
				if(::RegOpenKeyExW(HKEY_CURRENT_USER, subkey, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS)
					return false;
				bool ok = ::RegQueryValueExW(hKey, name.c_str(), nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
				::RegCloseKey(hKey);
				return ok;
			}
		}

		namespace FileDialog
		{
			// path.file.save_box: the extension a typed name gets when it has none,
			// from the first pattern of the first filter ('Text|*.txt;*.text|...'
			// gives "txt"). Empty for '*.*' or anything that is not a plain '*.ext'.
			inline std::wstring default_extension(std::wstring_view filter)
			{
				auto bar = filter.find(L'|');
				if(bar == std::wstring_view::npos)
					return {};
				auto pattern = filter.substr(bar + 1);
				pattern = pattern.substr(0, pattern.find_first_of(L"|;"));
				while(!pattern.empty() && pattern.front() == L' ')
					pattern.remove_prefix(1);
				while(!pattern.empty() && pattern.back() == L' ')
					pattern.remove_suffix(1);
				if(pattern.size() < 3 || pattern[0] != L'*' || pattern[1] != L'.')
					return {};
				auto ext = pattern.substr(2);
				if(ext.find_first_of(L"*?.\\/") != std::wstring_view::npos)
					return {};
				return std::wstring(ext);
			}
		}

		// path.absolute / path.relative, rooted at a base folder (the folder the
		// menu was opened in) instead of the process working directory, which is
		// Explorer's and means nothing to a menu.
		namespace PathResolve
		{
			inline std::wstring full(const std::wstring &path)
			{
				if(path.empty())
					return {};
				std::wstring buffer(MAX_PATH, L'\0');
				for(int attempt = 0; attempt < 3; attempt++)
				{
					auto n = ::GetFullPathNameW(path.c_str(), DWORD(buffer.size()), buffer.data(), nullptr);
					if(n == 0)
						return path;
					if(n < buffer.size())
					{
						buffer.resize(n);
						return buffer;
					}
					buffer.assign(n, L'\0');	// n is the size needed, terminator included
				}
				return path;
			}

			// A relative path joined to base, then normalized ('..', '/'). A path
			// that starts with one backslash goes on base's drive. Without a base, a
			// relative path comes back as it is.
			inline std::wstring absolute(const std::wstring &path, const std::wstring &base)
			{
				if(path.empty())
					return {};

				auto slash = [](wchar_t c) { return c == L'\\' || c == L'/'; };
				std::wstring combined;
				if(slash(path[0]) && !(path.size() > 1 && slash(path[1])))
				{
					if(base.size() >= 2 && base[1] == L':')
						combined = base.substr(0, 2) + path;
					else
						combined = path;
				}
				else if(::PathIsRelativeW(path.c_str()))
				{
					if(base.empty())
						return path;
					combined = base;
					if(!slash(combined.back()))
						combined += L'\\';
					combined += path;
				}
				else
					combined = path;

				return full(combined);
			}

			// The path as seen from base ('..\docs\a.txt'). When there is no way
			// there from base (another drive, a path too long for the API), the
			// full path; without a base, the path as it is.
			inline std::wstring relative(const std::wstring &path, const std::wstring &base)
			{
				if(path.empty() || base.empty())
					return path;

				auto to = absolute(path, base);
				auto from = full(base);
				if(to.size() >= MAX_PATH || from.size() >= MAX_PATH)
					return to;

				// The base folder itself (the API would treat it as a file next to itself).
				auto trimmed = [](std::wstring s) {
					while(s.size() > 3 && (s.back() == L'\\' || s.back() == L'/'))
						s.pop_back();
					return s;
				};
				auto a = trimmed(to), b = trimmed(from);
				if(::CompareStringOrdinal(a.c_str(), int(a.size()), b.c_str(), int(b.size()), TRUE) == CSTR_EQUAL)
					return L".";

				wchar_t out[MAX_PATH]{};
				if(!::PathRelativePathToW(out, from.c_str(), FILE_ATTRIBUTE_DIRECTORY, to.c_str(), FILE_ATTRIBUTE_NORMAL))
					return to;

				std::wstring result(out);
				if(result.size() > 2 && result[0] == L'.' && result[1] == L'\\')
					result.erase(0, 2);
				return result;
			}
		}

		namespace PowerShellArgs
		{
			// Base64 (RFC 4648, with padding) of raw bytes.
			inline std::wstring base64(const unsigned char *data, size_t size)
			{
				static constexpr wchar_t table[] =
					L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
				std::wstring out;
				out.reserve((size + 2) / 3 * 4);
				size_t i = 0;
				for(; i + 2 < size; i += 3)
				{
					unsigned v = (unsigned(data[i]) << 16) | (unsigned(data[i + 1]) << 8) | data[i + 2];
					out += table[(v >> 18) & 63];
					out += table[(v >> 12) & 63];
					out += table[(v >> 6) & 63];
					out += table[v & 63];
				}
				if(i < size)
				{
					unsigned v = unsigned(data[i]) << 16;
					if(i + 1 < size)
						v |= unsigned(data[i + 1]) << 8;
					out += table[(v >> 18) & 63];
					out += table[(v >> 12) & 63];
					out += i + 1 < size ? table[(v >> 6) & 63] : L'=';
					out += L'=';
				}
				return out;
			}

			// The arguments for powershell.exe or pwsh.exe from a cmd-ps= / cmd-pwsh= value.
			// A one-line value is passed as it is. A script of several lines is cut off
			// at the first line break when given on the command line, so it is sent as
			// -EncodedCommand <Base64 of its UTF-16LE text>. Switches before the script
			// (-NoExit, -NoProfile, -ExecutionPolicy Bypass, -Command, ...) stay as they
			// are; only the script is encoded. A value that runs a file or is already
			// encoded (-File, -EncodedCommand) is left alone.
			inline std::wstring encode(const std::wstring &value)
			{
				if(value.find_first_of(L"\r\n") == std::wstring::npos)
					return value;

				auto is_space = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; };
				auto lower = [](std::wstring s)
				{
					for(auto &c : s)
						if(c >= L'A' && c <= L'Z')
							c = wchar_t(c - L'A' + L'a');
					return s;
				};
				auto any_of = [](const std::wstring &s, std::initializer_list<const wchar_t *> names)
				{
					for(auto n : names)
						if(s == n)
							return true;
					return false;
				};

				std::wstring switches;
				bool command = false;
				size_t pos = 0;
				for(;;)
				{
					while(pos < value.size() && is_space(value[pos]))
						pos++;
					if(pos >= value.size() || (value[pos] != L'-' && value[pos] != L'/'))
						break;

					size_t end = pos;
					while(end < value.size() && !is_space(value[end]))
						end++;
					auto token = lower(value.substr(pos + 1, end - pos - 1));
					if(token.empty())
						break;

					if(any_of(token, { L"file", L"f", L"encodedcommand", L"enc", L"e", L"ec",
									   L"encodedarguments", L"ea" }))
						return value;

					if(any_of(token, { L"command", L"c" }))
					{
						command = true;
						pos = end;
						while(pos < value.size() && is_space(value[pos]))
							pos++;
						break;
					}

					// A switch that takes a value: keep the value with it.
					if(any_of(token, { L"executionpolicy", L"ex", L"ep", L"windowstyle", L"w", L"version", L"v",
									   L"inputformat", L"inp", L"if", L"outputformat", L"o", L"of",
									   L"configurationname", L"config", L"workingdirectory", L"wd", L"wo",
									   L"settingsfile", L"custompipename", L"psconsolefile" }))
					{
						while(end < value.size() && is_space(value[end]))
							end++;
						while(end < value.size() && !is_space(value[end]))
							end++;
					}
					else if(!any_of(token, { L"noexit", L"noe", L"noprofile", L"nop", L"nologo", L"nol",
											 L"noninteractive", L"noni", L"sta", L"mta", L"login", L"l",
											 L"noprofileloadtime", L"interactive", L"i" }))
					{
						// Not a switch PowerShell knows: it is the start of the script.
						break;
					}

					switches.append(value, pos, end - pos);
					switches += L' ';
					pos = end;
				}

				auto last = value.find_last_not_of(L" \t\r\n");
				if(pos >= value.size() || last == std::wstring::npos || last < pos)
					return value;
				std::wstring script = value.substr(pos, last + 1 - pos);
				// Only the switches were on lines of their own.
				if(script.find_first_of(L"\r\n") == std::wstring::npos)
					return switches + (command ? L"-Command " : L"") + script;

				return switches + L"-EncodedCommand " +
					base64(reinterpret_cast<const unsigned char *>(script.data()), script.size() * sizeof(wchar_t));
			}
		}
	}
}
