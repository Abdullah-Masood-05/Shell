#pragma once

// The order pos= gives the items Shell adds to one menu, as
// ContextMenu::OnInitMenuPopup lays them out:
//
//  - pos=top items first, then those without pos= (auto), then pos=bottom;
//  - pos=middle items go in at the middle of that list, together;
//  - last, in config order, pos=N goes in at index N (the end when N is past
//    it) and pos=indexof(pattern, offset, default) at the first item the
//    pattern matches plus offset (IndexOf.h).
//
// `shell -config:tree` (MenuTree.cpp) uses it to list Shell's items in the
// order they show, and the settings app's tests check it. Only Shell's items
// are arranged here: in the main menu Windows' items sit between them, so a
// number in pos=N counts Windows' items there too.
//
// Header-only and free of menu types, like IndexOf.h.

#include "IndexOf.h"

#include <cstddef>
#include <vector>

namespace Nilesoft
{
	namespace Shell
	{
		namespace Placement
		{
			enum class Kind
			{
				Auto,
				Top,
				Middle,
				Bottom,
				Index,		// pos=N
				IndexOf,	// pos=indexof(...)
			};

			struct Spec
			{
				Kind kind = Kind::Auto;
				int index = 0;		// Index
				int offset = 0;		// IndexOf
				int def = -1;		// IndexOf: IndexOf::Top, IndexOf::Middle, N >= 0, else the end
			};

			// spec(item) -> const Spec &
			// anchor(items, item) -> std::ptrdiff_t: the first of `items` the item's pattern
			//     matches, or < 0
			// same(a, b) -> bool: a and b have the same indexof() (pattern, offset and default)
			template<class T, class SpecOf, class Anchor, class Same>
			std::vector<T> arrange(const std::vector<T> &list, SpecOf spec, Anchor anchor, Same same)
			{
				std::vector<T> items, middle, bottom, custom;
				for(const auto &item : list)
				{
					if(spec(item).kind == Kind::Top)
						items.push_back(item);
				}
				for(const auto &item : list)
				{
					switch(spec(item).kind)
					{
						case Kind::Auto: items.push_back(item); break;
						case Kind::Middle: middle.push_back(item); break;
						case Kind::Bottom: bottom.push_back(item); break;
						case Kind::Index:
						case Kind::IndexOf: custom.push_back(item); break;
						default: break;
					}
				}
				items.insert(items.end(), bottom.begin(), bottom.end());
				items.insert(items.begin() + static_cast<std::ptrdiff_t>(items.size() / 2), middle.begin(), middle.end());

				struct Placed { T spec; T last; };
				std::vector<Placed> placed;
				for(const auto &item : custom)
				{
					const auto &s = spec(item);
					if(s.kind == Kind::Index)
					{
						if(s.index < 0 || s.index > static_cast<int>(items.size()))
							items.push_back(item);
						else
							items.insert(items.begin() + s.index, item);
						continue;
					}
					Placed *group = nullptr;
					for(auto &p : placed)
					{
						if(same(p.spec, item))
						{
							group = &p;
							break;
						}
					}
					std::ptrdiff_t at = group ? -1 : anchor(items, item);
					IndexOf::insert(items, item, group ? &group->last : nullptr, at, s.offset, s.def);
					if(group)
						group->last = item;
					else
						placed.push_back({ item, item });
				}
				return items;
			}
		}
	}
}
