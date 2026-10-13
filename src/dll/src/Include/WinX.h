#pragma once

// Recognising the Win+X menu (also opened by right-clicking Start).
//
// The menu is a classic Win32 popup that twinui.pcshell.dll shows inside
// explorer.exe with TrackPopupMenu(TPM_RETURNCMD). Its command IDs index
// twinui's own list of entries, so Shell must never hand one back that twinui
// did not put in the menu (upstream #25).
//
// Up to Windows 10 the popup is owned by a "LauncherTipWnd" (or the taskbar's
// "Start" button), and the window class alone identified it. On Windows 11 the
// owner passed to TrackPopupMenu is the taskbar window itself (Shell_TrayWnd),
// so the class test classified the menu as a taskbar menu: window.is_start was
// always 0 and type='taskbar' matched it instead (upstream #535, whose
// workaround was `type='taskbar' where=window.parent.name == null`).
//
// So a taskbar-owned popup is checked further, in this order:
//   1. the owner/parent chain contains LauncherTipWnd or Start;
//   2. the code that called TrackPopupMenu(Ex) is twinui.pcshell.dll (the
//      nearest modules on this thread's stack, past shell.dll, user32, win32u
//      and ntdll);
//   3. fallback, only when the stack could not be attributed to any caller
//      (e.g. a truncated x86 frame-pointer walk): the menu has the "Shut down
//      or sign out" submenu, whose commands twinui numbers from 4000.
// Shell's own Windows 11 taskbar menu (ScopedTaskbarOrigin) is never a Win+X
// menu, and is excluded before any of this runs.

namespace Nilesoft
{
	namespace Shell
	{
		namespace WinX
		{
			enum class Source : uint8_t
			{
				None,		// not the Win+X menu
				OwnerClass,	// the owner window is LauncherTipWnd or Start
				OwnerChain,	// an owner or parent of it is
				Caller,		// TrackPopupMenu was called from twinui.pcshell.dll
				Fallback	// the stack said nothing; the menu has the shutdown submenu
			};

			inline const wchar_t *source_name(Source source) noexcept
			{
				switch(source)
				{
					case Source::OwnerClass: return L"owner class";
					case Source::OwnerChain: return L"owner chain";
					case Source::Caller: return L"caller module";
					case Source::Fallback: return L"fallback (shutdown submenu)";
					default: return L"none";
				}
			}

			inline bool is_launcher_class(uint32_t class_hash) noexcept
			{
				return class_hash == WC_LauncherTipWnd || class_hash == WC_START;
			}

			// LauncherTipWnd or Start among the owners and parents of hWnd
			// (not hWnd itself), a few levels up.
			inline bool owner_chain_has_launcher(HWND hWnd) noexcept
			{
				HWND h = hWnd;
				for(int depth = 0; h && depth < 8; depth++)
				{
					HWND next = ::GetWindow(h, GW_OWNER);
					if(!next)
						next = ::GetAncestor(h, GA_PARENT);
					if(!next || next == ::GetDesktopWindow())
						break;
					if(is_launcher_class(Windows::Window::class_hash(next)))
						return true;
					h = next;
				}
				return false;
			}

			struct CallerInfo
			{
				// Frames on this thread's stack, and how many were in a module
				// other than shell.dll, user32, win32u and ntdll.
				uint16_t frames = 0;
				uint16_t attributed = 0;
				// twinui.pcshell.dll is one of the nearest such modules.
				bool twinui = false;
				// The nearest of them, for the log: "a.dll < b.dll < c.exe".
				string modules;
			};

			// The modules that led to this TrackPopupMenu(Ex) call. Must run on
			// the thread that is tracking the popup, inside the hook.
			inline CallerInfo callers(HMODULE self, size_t nearest = 3)
			{
				CallerInfo info;

				void *frames[62]{};
				info.frames = ::RtlCaptureStackBackTrace(0, static_cast<ULONG>(std::size(frames)), frames, nullptr);

				const HMODULE skip[] = {
					self,
					::GetModuleHandleW(L"user32.dll"),
					::GetModuleHandleW(L"win32u.dll"),
					::GetModuleHandleW(L"ntdll.dll"),
				};
				const HMODULE twinui = ::GetModuleHandleW(L"twinui.pcshell.dll");

				HMODULE last{};
				size_t found = 0;
				for(uint16_t i = 0; i < info.frames && found < nearest; i++)
				{
					HMODULE module{};
					if(!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
											 static_cast<LPCWSTR>(frames[i]), &module) || !module)
						continue;

					if(std::find(std::begin(skip), std::end(skip), module) != std::end(skip))
						continue;

					info.attributed++;

					// Consecutive frames in one module name it once.
					if(module == last)
						continue;
					last = module;
					found++;

					if(twinui && module == twinui)
						info.twinui = true;

					if(!info.modules.empty())
						info.modules += L" < ";
					info.modules += IO::Path::Name(IO::Path::Module(module)).move();
				}
				return info;
			}

			// The Win+X menu's "Shut down or sign out" submenu: twinui numbers
			// its commands from 4000.
			inline bool has_shutdown_submenu(HMENU hMenu) noexcept
			{
				if(!hMenu)
					return false;

				const int count = ::GetMenuItemCount(hMenu);
				for(int i = 0; i < count; i++)
				{
					auto sub = ::GetSubMenu(hMenu, i);
					if(!sub)
						continue;

					const int sub_count = ::GetMenuItemCount(sub);
					for(int j = 0; j < sub_count; j++)
					{
						auto id = ::GetMenuItemID(sub, j);
						if(id >= 4000 && id < 4100)
							return true;
					}
				}
				return false;
			}
		}
	}
}
