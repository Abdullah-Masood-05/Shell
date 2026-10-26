#pragma once

// Where an item with pos=indexof(pattern, offset, default) goes, once the
// menu's own items (Explorer's and the shell extensions') are in place.
//
//  - The first item whose title matches `pattern` is the anchor; the item goes
//    at anchor + offset (1 = right after it), kept inside the menu.
//  - No match: the default position (top, middle, a number N for index N kept
//    inside the menu, else the end).
//  - Several items with the same indexof() keep their order in the file: each
//    one after the first goes right after the one placed before it. (Each used
//    to go to anchor + offset again, so a group came out reversed.)
//
// Header-only and free of menu types so the settings app's tests can check it.

#include <algorithm>
#include <cstddef>
#include <vector>

namespace Nilesoft
{
	namespace Shell
	{
		namespace IndexOf
		{
			// Default positions, as Shell's Position enum: top -2, middle -3; a number >= 0 is that
			// index (kept inside the menu); anything else is the end.
			constexpr int Top = -2;
			constexpr int Middle = -3;

			// anchor: the index of the matching item, or < 0 when nothing matched.
			// previous: the item placed just before with the same indexof(), or null.
			template<class T>
			size_t insert(std::vector<T> &items, const T &item, const T *previous, std::ptrdiff_t anchor, int offset, int def)
			{
				const auto size = static_cast<std::ptrdiff_t>(items.size());
				std::ptrdiff_t at = size;

				auto prev = previous ? std::find(items.begin(), items.end(), *previous) : items.end();
				if(prev != items.end())
					at = (prev - items.begin()) + 1;
				else if(anchor >= 0)
					at = std::clamp<std::ptrdiff_t>(anchor + offset, 0, size);
				else if(def == Top)
					at = 0;
				else if(def == Middle)
					at = size / 2;
				else if(def >= 0)
					at = std::min<std::ptrdiff_t>(def, size);

				items.insert(items.begin() + at, item);
				return static_cast<size_t>(at);
			}
		}
	}
}
