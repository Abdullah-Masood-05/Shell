#pragma once

// Helpers behind some NSS functions, kept free of Shell's own types so a
// standalone test can check them.

#include <cstddef>
#include <string>
#include <string_view>
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
	}
}
