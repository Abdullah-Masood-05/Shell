#pragma once

// Colours for a menu item an extension draws itself (#14).
//
// XnView's image preview is an owner-drawn item: Shell passes WM_DRAWITEM on
// and the extension paints the row into a bitmap that Shell then blends into
// the menu. The extension paints its background and text with the Windows
// menu colours (GetSysColor, GetSysColorBrush), not with the Shell theme's,
// so the row did not match the rest of the menu and its anti-aliased label
// was blended against the wrong colour.
//
// While Shell forwards one of those messages, and only on that thread, the
// menu colours are answered with the theme's. user32's GetSysColor and
// GetSysColorBrush are patched once per process, the first time such an item
// is measured or drawn (most menus never have one). Outside a Scope, and on
// every other thread, the patch only reads a thread-local pointer and calls
// the real function.
//
// The extension that draws is not known (Explorer passes the message on
// through its own menu objects), so its import table cannot be patched; the
// functions themselves are. Removing the patch while another thread may be
// inside it is not safe, so the module is pinned instead and the patch stays
// until the process exits.

#include <windows.h>
#include <mutex>
#include <string>
#include <unordered_map>
#include "Library/detours.h"

namespace Nilesoft
{
	namespace Shell
	{
		namespace OwnerDraw
		{
			struct Colors
			{
				COLORREF menu{};			// COLOR_MENU, COLOR_MENUBAR, COLOR_WINDOW, COLOR_3DFACE
				COLORREF text{};			// COLOR_MENUTEXT, COLOR_WINDOWTEXT
				COLORREF highlight{};		// COLOR_HIGHLIGHT, COLOR_MENUHILIGHT
				COLORREF highlight_text{};	// COLOR_HIGHLIGHTTEXT
				COLORREF gray{};			// COLOR_GRAYTEXT
			};

			enum class Install { None, Done, Failed, Unpinned };

			namespace detail
			{
				inline thread_local Colors *current = nullptr;
				inline thread_local long *text_right = nullptr;
				inline Detours<decltype(::GetSysColor)> get_sys_color;
				inline Detours<decltype(::GetSysColorBrush)> get_sys_color_brush;
				inline Detours<decltype(::DrawTextW)> draw_text;

				inline bool map(const Colors *colors, int index, COLORREF &color) noexcept
				{
					switch(index)
					{
						case COLOR_MENU:
						case COLOR_MENUBAR:
						case COLOR_WINDOW:
						case COLOR_3DFACE:
							color = colors->menu;
							return true;
						case COLOR_MENUTEXT:
						case COLOR_WINDOWTEXT:
							color = colors->text;
							return true;
						case COLOR_HIGHLIGHT:
						case COLOR_MENUHILIGHT:
							color = colors->highlight;
							return true;
						case COLOR_HIGHLIGHTTEXT:
							color = colors->highlight_text;
							return true;
						case COLOR_GRAYTEXT:
							color = colors->gray;
							return true;
					}
					return false;
				}

				// One solid brush per colour, created on first use and never deleted:
				// the owner may keep the handle it was given, as it may with a
				// system colour brush. A brush the owner deleted anyway is made
				// again (GDI may hand a deleted handle out again for another brush,
				// so its colour is checked too).
				inline HBRUSH brush(COLORREF color)
				{
					static std::mutex lock;
					static std::unordered_map<COLORREF, HBRUSH> brushes;

					std::lock_guard<std::mutex> guard(lock);
					auto it = brushes.find(color);
					if(it != brushes.end())
					{
						LOGBRUSH lb{};
						if(::GetObjectW(it->second, sizeof(lb), &lb) == sizeof(lb) &&
						   lb.lbStyle == BS_SOLID && lb.lbColor == color)
							return it->second;
						brushes.erase(it);
					}

					// a theme has a handful of colours; the cap only guards against a leak
					if(brushes.size() >= 64)
						return nullptr;

					auto hbrush = ::CreateSolidBrush(color);
					if(hbrush)
						brushes.emplace(color, hbrush);
					return hbrush;
				}

				inline DWORD WINAPI GetSysColorHook(int index)
				{
					if(auto colors = current; colors)
					{
							COLORREF color;
						if(map(colors, index, color))
							return color;
					}
					return get_sys_color.invoke(index);
				}

				inline HBRUSH WINAPI GetSysColorBrushHook(int index)
				{
					if(auto colors = current; colors)
					{
							COLORREF color;
						if(map(colors, index, color))
						{
							if(auto hbrush = brush(color); hbrush)
								return hbrush;
						}
					}
					return get_sys_color_brush.invoke(index);
				}

				// While a TextExtent is live, the right edge of every single-line
				// string the owner draws is recorded before it is drawn.
				inline int WINAPI DrawTextWHook(HDC hdc, LPCWSTR text, int length, LPRECT rect, UINT format)
				{
					if(auto right = text_right; right && rect && text && !(format & (DT_CALCRECT | DT_WORDBREAK)))
					{
						RECT calc = *rect;
						if(draw_text.invoke(hdc, text, length, &calc, format | DT_CALCRECT) && calc.right > *right)
							*right = calc.right;
					}
					return draw_text.invoke(hdc, text, length, rect, format);
				}
			}

			// Patches GetSysColor and GetSysColorBrush once per process and returns
			// whether the patch is in place. *result is set only on the call that
			// tried (Install::None on every later call).
			inline bool install(Install *result = nullptr)
			{
				static std::once_flag once;
				static bool installed = false;

				if(result) *result = Install::None;

				std::call_once(once, [result]()
				{
					auto set = [result](Install value) { if(result) *result = value; };

					auto user32 = ::GetModuleHandleW(L"user32.dll");
					if(!user32)
						return set(Install::Failed);

					// Pinned first: once patched, this module must never be unloaded.
					HMODULE self{};
					if(!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
											 reinterpret_cast<LPCWSTR>(&detail::GetSysColorHook), &self))
						return set(Install::Unpinned);

					detail::get_sys_color.init(user32, "GetSysColor", reinterpret_cast<void *>(&detail::GetSysColorHook));
					detail::get_sys_color_brush.init(user32, "GetSysColorBrush", reinterpret_cast<void *>(&detail::GetSysColorBrushHook));
					detail::draw_text.init(user32, "DrawTextW", reinterpret_cast<void *>(&detail::DrawTextWHook));

					using detours = decltype(detail::get_sys_color);
					if(detours::Begin() != NO_ERROR)
						return set(Install::Failed);

					detail::get_sys_color.hook();
					detail::get_sys_color_brush.hook();
					detail::draw_text.hook();

					if(detours::Commit() != NO_ERROR)
					{
						detail::get_sys_color.forget();
						detail::get_sys_color_brush.forget();
						detail::draw_text.forget();
						return set(Install::Failed);
					}

					installed = detail::get_sys_color.is_hooked() || detail::get_sys_color_brush.is_hooked();
					set(installed ? Install::Done : Install::Failed);
				});

				return installed;
			}

			// The furthest right edge, in the owner's own coordinates, of the text it
			// draws on this thread while it lives. Install first.
			class TextExtent
			{
				long *_previous;
			public:
				long right{};
				TextExtent() noexcept : _previous(detail::text_right) { detail::text_right = &right; }
				~TextExtent() { detail::text_right = _previous; }
				TextExtent(const TextExtent &) = delete;
				TextExtent &operator=(const TextExtent &) = delete;
			};

			// The theme's colours for the owner's calls on this thread while it lives.
			class Scope
			{
				Colors *_previous;
			public:
				explicit Scope(Colors &colors) noexcept : _previous(detail::current) { detail::current = &colors; }
				~Scope() { detail::current = _previous; }
				Scope(const Scope &) = delete;
				Scope &operator=(const Scope &) = delete;
			};
		}
	}
}
