#pragma once

// io.dir.create and io.file.create(..., rename): tell Explorer about a new item and,
// when the open folder view shows the folder it was made in, select it and start
// renaming it, as Windows' own New > Folder does.
//
// The folder view belongs to its window's thread and Shell's pointer to its browser
// is a raw one read there, so the select runs on that thread, posted without waiting:
// driving the view from the command thread is what issue #8 is about. The
// notification is always sent from the caller, so Explorer hears about the item even
// when the select is dropped (a busy or closed window).
//
// Kept free of Shell's own types, so the issue #8 harness builds this same code.

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <string>
#include "WindowThread.h"

namespace Nilesoft
{
	namespace Shell
	{
		namespace new_item
		{
			// A full path in Windows' form: "/" becomes "\", "." and ".." are resolved and
			// a trailing separator goes ("C:\" keeps its own).
			inline std::wstring normal(const wchar_t *path)
			{
				std::wstring s = path ? path : L"";
				if(s.empty())
					return s;
				wchar_t full[MAX_PATH * 4]{};
				auto n = ::GetFullPathNameW(s.c_str(), _countof(full), full, nullptr);
				if(n && n < _countof(full))
					s = full;
				for(auto &c : s)
				{
					if(c == L'/')
						c = L'\\';
				}
				while(s.length() > 3 && s.back() == L'\\')
					s.pop_back();
				return s;
			}

			inline bool same_path(const std::wstring &a, const std::wstring &b)
			{
				if(a.empty() || b.empty())
					return false;
				auto x = normal(a.c_str()), y = normal(b.c_str());
				return ::CompareStringOrdinal(x.c_str(), (int)x.length(), y.c_str(), (int)y.length(), TRUE) == CSTR_EQUAL;
			}

			inline std::wstring known_folder(REFKNOWNFOLDERID id)
			{
				std::wstring s;
				PWSTR path = nullptr;
				if(SUCCEEDED(::SHGetKnownFolderPath(id, 0, nullptr, &path)) && path)
					s = path;
				::CoTaskMemFree(path);
				return s;
			}

			// Does the view's folder show the items of the folder `parent`? The desktop's
			// view is the Desktop root, a virtual folder that shows your Desktop and the
			// Public Desktop.
			inline bool view_shows(PCIDLIST_ABSOLUTE pidlFolder, const std::wstring &parent)
			{
				if(!pidlFolder || pidlFolder->mkid.cb == 0)
					return same_path(parent, known_folder(FOLDERID_Desktop)) ||
						same_path(parent, known_folder(FOLDERID_PublicDesktop));

				wchar_t path[MAX_PATH * 4]{};
				if(!::SHGetPathFromIDListEx(pidlFolder, path, _countof(path), GPFIDL_DEFAULT))
					return false;
				return same_path(parent, path);
			}

			// On the view's own thread.
			inline void select_in_view(HWND hwnd, IShellBrowser *browser, const std::wstring &item, LONG event, UINT flags)
			{
				// The view's window goes with the browser: if it is gone, so may the browser be.
				if(!::IsWindow(hwnd))
					return;

				IShellView *sv = nullptr;
				if(S_OK != browser->QueryActiveShellView(&sv) || !sv)
					return;

				IShellView2 *sv2 = nullptr;
				IFolderView *fv = nullptr;
				IPersistFolder2 *pf = nullptr;
				PIDLIST_ABSOLUTE pidlFolder = nullptr;
				IShellFolder *psf = nullptr;
				PIDLIST_RELATIVE pidlItem = nullptr;

				if(S_OK == sv->QueryInterface(IID_PPV_ARGS(&sv2)) &&
				   S_OK == sv->QueryInterface(IID_PPV_ARGS(&fv)) &&
				   S_OK == fv->GetFolder(IID_PPV_ARGS(&pf)) &&
				   S_OK == pf->GetCurFolder(&pidlFolder) && pidlFolder)
				{
					std::wstring parent = item;
					auto slash = parent.find_last_of(L'\\');
					parent.resize(slash == std::wstring::npos ? 0 : (slash < 3 ? slash + 1 : slash));
					std::wstring leaf = ::PathFindFileNameW(item.c_str());

					// The item is looked up by its plain name, and only in the folder the
					// view shows, so nothing of the same name elsewhere is renamed.
					if(view_shows(pidlFolder, parent) && !leaf.empty() &&
					   S_OK == fv->GetFolder(IID_PPV_ARGS(&psf)))
					{
						// First, so the view has the item before it is selected (Windows'
						// New menu does the same). The caller sent it already, without waiting.
						::SHChangeNotify(event, SHCNF_PATH | SHCNF_FLUSH, item.c_str(), nullptr);
					}
					if(psf &&
					   SUCCEEDED(psf->ParseDisplayName(nullptr, nullptr, (LPWSTR)leaf.c_str(), nullptr, &pidlItem, nullptr)) && pidlItem)
					{
						sv2->SelectAndPositionItem(::ILFindLastID(pidlItem), flags, nullptr);
					}
				}

				if(pidlItem) ::ILFree(pidlItem);
				if(psf) psf->Release();
				if(pidlFolder) ::ILFree(pidlFolder);
				if(pf) pf->Release();
				if(fv) fv->Release();
				if(sv2) sv2->Release();
				sv->Release();
			}

			// From the command thread. `hwnd` is the view's window (Selections' Window.handle)
			// and `browser` its shell browser; either may be null. Returns whether a select
			// was posted.
			inline bool announce(HWND hwnd, IShellBrowser *browser, const wchar_t *path, bool folder,
								 UINT flags = SVSI_SELECT | SVSI_POSITIONITEM | SVSI_EDIT)
			{
				const LONG event = folder ? SHCNE_MKDIR : SHCNE_CREATE;
				auto item = normal(path);
				if(item.empty())
					return false;

				::SHChangeNotify(event, SHCNF_PATH | SHCNF_FLUSHNOWAIT, item.c_str(), nullptr);

				if(!browser || !hwnd)
					return false;
				return on_window_thread::post(hwnd, [hwnd, browser, item, event, flags]()
				{
					select_in_view(hwnd, browser, item, event, flags);
				});
			}
		}
	}
}
