#pragma once

// Helpers behind some NSS functions, kept free of Shell's own types so a
// standalone test can check them.

#include <windows.h>

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
	}
}
