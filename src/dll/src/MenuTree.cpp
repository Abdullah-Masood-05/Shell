#include <pch.h>
#include "Include\IdNames.h"

#include <shobjidl.h>
#include <wrl/client.h>

#pragma comment(lib, "version.lib")

/*
	shell -config:tree <root.nss> -context:<where> [-sample:<path>]: see
	ConfigCheck.h (ConfigTree) and Parser::ReportTree.

	Three lists, as JSON:

	"shell":  the items of the user's config that show in <where> (type=), in
	          config order, with the file and line of each declaration.
	"system": the Windows menu for <sample> as Explorer's IContextMenu builds
	          it, into a menu that is never shown (no window is created), with
	          the program that added each item where that can be told, and the
	          modify/remove rules that reach it.
	"rules":  every modify/remove, with what can be read from it without a
	          real selection.

	The selection is the one the real menu would have for <sample> (This PC,
	the desktop and the taskbar need none): Selections::Parse of the item, as
	in Explorer (sample_selection). Conditions are worked out against it where
	that gives the real answer: sel.*, type.*, the window.is_* a context
	decides, sys.* and path.* that only read (Known, knowable()). Keys, the
	time, the clipboard, variables and the like make the answer "maybe".

	Rules are matched the way ContextMenu::build_main_system_menuitems does:
	find= and in= are evaluated, where= as above, mode= is not tested (one
	item is selected). Shell's own items get "shows" (yes, no, maybe) from
	their where=, find= and vis= as ContextMenu::prepare_new_items decides it,
	and from the menu they end up in.
*/

namespace Nilesoft
{
	namespace Shell
	{
		using Microsoft::WRL::ComPtr;

		namespace
		{
			// Set while ReportTree builds menus: DllGetClassObject then hands out
			// nothing, so this copy of shell.dll never joins the menus it reads.
			bool g_reading_menus = false;

			constexpr UINT first_cmd = 1;
			constexpr UINT last_cmd = 0x7FFF;
			constexpr int max_depth = 4;

			void jstr(std::wstring &o, const wchar_t *s, size_t n)
			{
				o += L'"';
				for(size_t i = 0; i < n; i++)
				{
					auto c = s[i];
					switch(c)
					{
						case L'"': o += L"\\\""; break;
						case L'\\': o += L"\\\\"; break;
						case L'\n': o += L"\\n"; break;
						case L'\r': o += L"\\r"; break;
						case L'\t': o += L"\\t"; break;
						default:
							if(c < 0x20)
							{
								wchar_t buf[8];
								swprintf_s(buf, L"\\u%04x", static_cast<unsigned>(c));
								o += buf;
							}
							else
								o += c;
					}
				}
				o += L'"';
			}
			void jstr(std::wstring &o, const std::wstring &s) { jstr(o, s.c_str(), s.size()); }
			void jstr(std::wstring &o, const string &s) { jstr(o, s.c_str(), s.length()); }
			void jkey(std::wstring &o, const wchar_t *key)
			{
				if(!o.empty() && o.back() != L'{' && o.back() != L'[')
					o += L',';
				o += L'"';
				o += key;
				o += L"\":";
			}
			void jbool(std::wstring &o, const wchar_t *key, bool v)
			{
				jkey(o, key);
				o += v ? L"true" : L"false";
			}
			void jnum(std::wstring &o, const wchar_t *key, long long v)
			{
				jkey(o, key);
				o += std::to_wstring(v);
			}
			void jtext(std::wstring &o, const wchar_t *key, const std::wstring &v)
			{
				jkey(o, key);
				jstr(o, v);
			}

			const wchar_t *id_name(uint32_t id)
			{
				for(auto &n : IdNames)
				{
					if(n.id == id)
						return n.name;
				}
				return nullptr;
			}

			// --- Calls into other programs' menu handlers, which may fault ---

			HRESULT seh_query(IContextMenu *cm, HMENU menu, UINT first, UINT last, UINT flags)
			{
				__try
				{
					return cm->QueryContextMenu(menu, 0, first, last, flags);
				}
				__except(EXCEPTION_EXECUTE_HANDLER)
				{
					return E_UNEXPECTED;
				}
			}

			HRESULT seh_verb(IContextMenu *cm, UINT_PTR offset, wchar_t *buf, UINT cch)
			{
				__try
				{
					buf[0] = 0;
					return cm->GetCommandString(offset, GCS_VERBW, nullptr, reinterpret_cast<LPSTR>(buf), cch);
				}
				__except(EXCEPTION_EXECUTE_HANDLER)
				{
					return E_UNEXPECTED;
				}
			}

			// What Explorer sends before a submenu opens; "Send to", "Open with"
			// and "New" fill themselves then.
			void seh_init_popup(IContextMenu *cm, HMENU sub, UINT index)
			{
				__try
				{
					IContextMenu3 *cm3 = nullptr;
					IContextMenu2 *cm2 = nullptr;
					if(SUCCEEDED(cm->QueryInterface(IID_PPV_ARGS(&cm3))) && cm3)
					{
						LRESULT r = 0;
						cm3->HandleMenuMsg2(WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(sub), MAKELPARAM(index, FALSE), &r);
						cm3->Release();
					}
					else if(SUCCEEDED(cm->QueryInterface(IID_PPV_ARGS(&cm2))) && cm2)
					{
						cm2->HandleMenuMsg(WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(sub), MAKELPARAM(index, FALSE));
						cm2->Release();
					}
				}
				__except(EXCEPTION_EXECUTE_HANDLER)
				{
				}
			}

			HRESULT seh_create(REFCLSID clsid, IShellExtInit **out)
			{
				__try
				{
					return ::CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(out));
				}
				__except(EXCEPTION_EXECUTE_HANDLER)
				{
					*out = nullptr;
					return E_UNEXPECTED;
				}
			}

			HRESULT seh_init(IShellExtInit *init, PCIDLIST_ABSOLUTE folder, IDataObject *data, HKEY key)
			{
				__try
				{
					return init->Initialize(folder, data, key);
				}
				__except(EXCEPTION_EXECUTE_HANDLER)
				{
					return E_UNEXPECTED;
				}
			}

			// --- Registry ---

			std::wstring reg_string(HKEY root, const std::wstring &key, const wchar_t *value = nullptr)
			{
				wchar_t buf[2048]{};
				DWORD size = sizeof(buf) - sizeof(wchar_t);
				DWORD type = 0;
				if(::RegGetValueW(root, key.c_str(), value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, &type,
								  buf, &size) != ERROR_SUCCESS)
					return {};
				std::wstring s = buf;
				if(type == REG_EXPAND_SZ)
				{
					wchar_t expanded[2048]{};
					if(::ExpandEnvironmentStringsW(s.c_str(), expanded, ARRAYSIZE(expanded)))
						s = expanded;
				}
				return s;
			}

			std::vector<std::wstring> reg_subkeys(HKEY root, const std::wstring &key)
			{
				std::vector<std::wstring> out;
				HKEY h{};
				if(::RegOpenKeyExW(root, key.c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS)
					return out;
				for(DWORD i = 0;; i++)
				{
					wchar_t name[256];
					DWORD len = ARRAYSIZE(name);
					if(::RegEnumKeyExW(h, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
						break;
					out.emplace_back(name, len);
				}
				::RegCloseKey(h);
				return out;
			}

			bool is_blocked(const std::wstring &clsid)
			{
				static const wchar_t *key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Blocked";
				for(auto root : { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER })
				{
					HKEY h{};
					if(::RegOpenKeyExW(root, key, 0, KEY_READ, &h) != ERROR_SUCCESS)
						continue;
					auto found = ::RegQueryValueExW(h, clsid.c_str(), nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
					::RegCloseKey(h);
					if(found)
						return true;
				}
				return false;
			}

			bool under_windows(const std::wstring &path)
			{
				wchar_t win[MAX_PATH]{};
				if(!::GetWindowsDirectoryW(win, MAX_PATH) || path.empty())
					return false;
				std::wstring w = win;
				if(!w.empty() && w.back() != L'\\')
					w += L'\\';
				return path.size() > w.size() && ::_wcsnicmp(path.c_str(), w.c_str(), w.size()) == 0;
			}

			// "7-Zip" from 7-zip.dll's version resource, else the file name.
			std::wstring program_name(const std::wstring &module)
			{
				if(module.empty())
					return {};
				std::wstring name;
				DWORD handle = 0;
				auto size = ::GetFileVersionInfoSizeW(module.c_str(), &handle);
				if(size)
				{
					std::vector<BYTE> data(size);
					if(::GetFileVersionInfoW(module.c_str(), 0, size, data.data()))
					{
						struct LANGCP
						{
							WORD lang, cp;
						} *tr = nullptr;
						UINT len = 0;
						if(::VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void **>(&tr), &len) && tr && len >= sizeof(LANGCP))
						{
							for(auto field : { L"ProductName", L"FileDescription" })
							{
								wchar_t sub[128];
								swprintf_s(sub, L"\\StringFileInfo\\%04x%04x\\%s", tr->lang, tr->cp, field);
								wchar_t *value = nullptr;
								UINT vlen = 0;
								if(::VerQueryValueW(data.data(), sub, reinterpret_cast<void **>(&value), &vlen) && value && vlen > 1)
								{
									name.assign(value, wcsnlen(value, vlen));
									while(!name.empty() && iswspace(name.back()))
										name.pop_back();
									if(!name.empty())
										break;
								}
							}
						}
					}
				}
				if(name.empty())
				{
					auto slash = module.find_last_of(L"\\/");
					name = slash == std::wstring::npos ? module : module.substr(slash + 1);
				}
				return name;
			}

			// The program a command line starts: "C:\x\a.exe" -y, or C:\x\a.exe %1.
			std::wstring command_exe(const std::wstring &cmd)
			{
				std::wstring s = cmd;
				while(!s.empty() && iswspace(s.front()))
					s.erase(0, 1);
				if(s.empty())
					return {};
				if(s.front() == L'"')
				{
					auto end = s.find(L'"', 1);
					return s.substr(1, end == std::wstring::npos ? std::wstring::npos : end - 1);
				}
				auto lower = s;
				for(auto &c : lower)
					c = towlower(c);
				auto exe = lower.find(L".exe");
				if(exe != std::wstring::npos)
					return s.substr(0, exe + 4);
				auto space = s.find(L' ');
				return s.substr(0, space);
			}

			std::wstring full_exe(const std::wstring &exe)
			{
				if(exe.empty() || exe.find(L'\\') != std::wstring::npos)
					return exe;
				wchar_t buf[MAX_PATH]{};
				if(::SearchPathW(nullptr, exe.c_str(), L".exe", MAX_PATH, buf, nullptr))
					return buf;
				return exe;
			}

			// --- The Windows menu ---

			struct Handler
			{
				std::wstring clsid;
				std::wstring name;		// HKCR\CLSID\{...} default value
				std::wstring module;	// InprocServer32
				std::wstring program;
				bool windows = false;
			};

			struct SysItem
			{
				int type = 0;			// 0 item, 1 submenu, 2 separator
				std::wstring title;		// as the menu has it, with & accelerators
				string name;			// normalized: what find= and this.name see
				std::wstring path;		// the submenu it is in, as in= names it ("" at the top)
				uint32_t uid = 0;		// this.id
				std::wstring verb;
				bool disabled = false;
				bool owner_drawn = false;
				const Handler *handler = nullptr;
				std::wstring source;	// windows | program | unknown
				std::wstring program;
				std::wstring module;
				std::vector<int> rules;	// rules that reach it
				std::vector<int> maybe;	// rules that may, depending on the selection
				bool hidden = false;
				bool maybe_hidden = false;
				bool pinned = false;	// menu='/' kept it in (or put it in) the main menu
				std::wstring moved, renamed;
				std::vector<SysItem> items;
			};

			std::wstring menu_text(HMENU menu, UINT index)
			{
				MENUITEMINFOW mii{ sizeof mii };
				mii.fMask = MIIM_STRING;
				if(!::GetMenuItemInfoW(menu, index, TRUE, &mii) || mii.cch == 0)
					return {};
				std::wstring text(mii.cch + 1, L'\0');
				mii.dwTypeData = text.data();
				mii.cch++;
				if(!::GetMenuItemInfoW(menu, index, TRUE, &mii))
					return {};
				text.resize(mii.cch);
				return text;
			}

			void read_menu(HMENU menu, IContextMenu *cm, std::vector<SysItem> &out, const std::wstring &path, int depth)
			{
				auto count = ::GetMenuItemCount(menu);
				for(int i = 0; i < count; i++)
				{
					MENUITEMINFOW mii{ sizeof mii };
					mii.fMask = MIIM_FTYPE | MIIM_STATE | MIIM_ID | MIIM_SUBMENU;
					if(!::GetMenuItemInfoW(menu, i, TRUE, &mii))
						continue;

					SysItem item;
					item.path = path;
					if(mii.fType & MFT_SEPARATOR)
					{
						item.type = 2;
						out.push_back(std::move(item));
						continue;
					}
					item.type = mii.hSubMenu ? 1 : 0;
					item.disabled = (mii.fState & MFS_DISABLED) != 0;
					item.title = menu_text(menu, i);
					item.owner_drawn = item.title.empty() && (mii.fType & MFT_OWNERDRAW);

					string title = item.title.c_str();
					auto hash = MenuItemInfo::normalize(title, &item.name);
					auto ui = Initializer::get_muid(hash);
					item.uid = ui ? ui->id : hash;

					if(cm && !mii.hSubMenu && mii.wID >= first_cmd && mii.wID <= last_cmd)
					{
						wchar_t verb[260]{};
						if(SUCCEEDED(seh_verb(cm, mii.wID - first_cmd, verb, ARRAYSIZE(verb))))
							item.verb = verb;
					}

					if(mii.hSubMenu && depth < max_depth)
					{
						if(cm)
							seh_init_popup(cm, mii.hSubMenu, static_cast<UINT>(i));
						string lower = item.name;
						std::wstring sub = path.empty() ? std::wstring(lower.tolower().c_str())
														: path + L'/' + lower.tolower().c_str();
						read_menu(mii.hSubMenu, cm, item.items, sub, depth + 1);
					}
					out.push_back(std::move(item));
				}
			}

			// Every title in a menu, normalized, for telling which handler added which items.
			// "7-zip/add to archive": an item by its submenu and title, so two
			// programs' "Add to archive..." are told apart.
			string item_key(const SysItem &it)
			{
				string name = it.name;
				std::wstring key = it.path + L'/' + name.tolower().c_str();
				return string(key.c_str());
			}

			void collect_names(const std::vector<SysItem> &items, std::vector<string> &names)
			{
				for(auto &it : items)
				{
					if(it.type != 2 && !it.name.empty())
						names.push_back(item_key(it));
					collect_names(it.items, names);
				}
			}

			// The items a folder window adds to its background menu itself (View,
			// Sort by, Refresh, Paste...): shell32's menu resource 215, as the
			// window loads it. The background IContextMenu has only the rest.
			void read_view_menu(bool desktop, std::vector<SysItem> &out)
			{
				auto shell32 = ::LoadLibraryExW(L"shell32.dll", nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
				if(!shell32)
					return;
				if(auto menu = ::LoadMenuW(shell32, MAKEINTRESOURCEW(215)))
				{
					if(auto sub = ::GetSubMenu(menu, 0))
					{
						std::vector<SysItem> all;
						read_menu(sub, nullptr, all, {}, 0);
						// 0 Arrange by (unused), 1 -, 2 View, 3 Sort by, 4 Group by, 5 Refresh,
						// 6 -, 7 Customize this folder, 8 -, 9 Paste, 10 Paste shortcut,
						// 11 Undo, 12 Redo (both only when there is something to undo).
						std::vector<size_t> pick = desktop ? std::vector<size_t>{ 2, 3, 5, 8, 9, 10 }
														   : std::vector<size_t>{ 2, 3, 4, 5, 6, 7, 8, 9, 10 };
						std::function<void(std::vector<SysItem> &)> tidy = [&](std::vector<SysItem> &items)
						{
							std::vector<SysItem> kept;
							for(auto &it : items)
							{
								// Sort by and Group by list the folder's columns at run time;
								// the resource has placeholders.
								if(it.type != 2 && (it.name.empty() || it.title == L"none"))
									continue;
								if(it.type == 2 && (kept.empty() || kept.back().type == 2))
									continue;
								tidy(it.items);
								it.source = L"windows";
								kept.push_back(std::move(it));
							}
							while(!kept.empty() && kept.back().type == 2)
								kept.pop_back();
							items = std::move(kept);
						};
						for(auto i : pick)
						{
							if(i < all.size())
								out.push_back(std::move(all[i]));
						}
						tidy(out);
					}
					::DestroyMenu(menu);
				}
				::FreeLibrary(shell32);
			}

			struct Target
			{
				std::wstring where;		// file | folder | drive | back | desktop
				std::wstring sample;
				PIDLIST_ABSOLUTE pidl = nullptr;		// the sample
				PIDLIST_ABSOLUTE folder = nullptr;		// the folder a handler is told about
				ComPtr<IDataObject> data;
				std::vector<std::wstring> keys;			// HKCR keys whose handlers apply

				~Target()
				{
					if(pidl)
						::CoTaskMemFree(pidl);
					if(folder)
						::CoTaskMemFree(folder);
				}
			};

			bool background(const std::wstring &where)
			{
				return where == L"back" || where == L"desktop";
			}

			// The menu Explorer builds for a right-click in `t.where` on `t.sample`.
			HRESULT open_menu(Target &t, ComPtr<IContextMenu> &cm, std::wstring &error)
			{
				HRESULT hr = S_OK;
				if(t.where == L"desktop")
				{
					ComPtr<IShellFolder> desktop;
					if(FAILED(hr = ::SHGetDesktopFolder(&desktop)))
						return hr;
					::SHGetKnownFolderIDList(FOLDERID_Desktop, 0, nullptr, &t.folder);
					return desktop->CreateViewObject(nullptr, IID_PPV_ARGS(&cm));
				}

				if(t.where == L"thispc")
				{
					// The This PC icon (desktop or navigation pane): the Computer
					// folder's own item in the desktop folder.
					if(FAILED(hr = ::SHGetKnownFolderIDList(FOLDERID_ComputerFolder, 0, nullptr, &t.pidl)))
					{
						error = L"This PC can't be opened";
						return hr;
					}
				}
				else if(t.sample.empty())
				{
					error = L"no sample path";
					return E_INVALIDARG;
				}
				else if(FAILED(hr = ::SHParseDisplayName(t.sample.c_str(), nullptr, &t.pidl, 0, nullptr)))
				{
					error = L"the sample path can't be opened: " + t.sample;
					return hr;
				}

				if(t.where == L"back")
				{
					ComPtr<IShellFolder> folder;
					if(FAILED(hr = ::SHBindToObject(nullptr, t.pidl, nullptr, IID_PPV_ARGS(&folder))))
						return hr;
					t.folder = ILCloneFull(t.pidl);
					return folder->CreateViewObject(nullptr, IID_PPV_ARGS(&cm));
				}

				ComPtr<IShellFolder> parent;
				PCUITEMID_CHILD child = nullptr;
				if(FAILED(hr = ::SHBindToParent(t.pidl, IID_PPV_ARGS(&parent), &child)))
					return hr;
				t.folder = ILCloneFull(t.pidl);
				ILRemoveLastID(t.folder);
				parent->GetUIObjectOf(nullptr, 1, &child, IID_IDataObject, nullptr, reinterpret_cast<void **>(t.data.GetAddressOf()));
				return parent->GetUIObjectOf(nullptr, 1, &child, IID_IContextMenu, nullptr, reinterpret_cast<void **>(cm.GetAddressOf()));
			}

			// The HKCR keys whose shellex\ContextMenuHandlers Explorer loads for the
			// target, most specific first.
			void association_keys(Target &t)
			{
				if(t.where == L"file")
				{
					auto dot = t.sample.find_last_of(L'.');
					auto slash = t.sample.find_last_of(L"\\/");
					if(dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash))
					{
						auto ext = t.sample.substr(dot);
						auto progid = reg_string(HKEY_CLASSES_ROOT, ext);
						if(!progid.empty())
							t.keys.push_back(progid);
						t.keys.push_back(L"SystemFileAssociations\\" + ext);
						auto perceived = reg_string(HKEY_CLASSES_ROOT, ext, L"PerceivedType");
						if(!perceived.empty())
							t.keys.push_back(L"SystemFileAssociations\\" + perceived);
					}
					t.keys.push_back(L"*");
					t.keys.push_back(L"AllFilesystemObjects");
				}
				else if(t.where == L"folder")
				{
					t.keys = { L"Directory", L"Folder", L"AllFilesystemObjects" };
				}
				else if(t.where == L"drive")
				{
					t.keys = { L"Drive", L"Folder", L"AllFilesystemObjects" };
				}
				else if(t.where == L"thispc")
				{
					t.keys = { L"CLSID\\{20D04FE0-3AEA-1069-A2D8-08002B30309D}" };
				}
				else if(t.where == L"back")
				{
					t.keys = { L"Directory\\Background" };
				}
				else if(t.where == L"desktop")
				{
					t.keys = { L"DesktopBackground", L"Directory\\Background" };
				}
			}

			bool is_shell_clsid(const std::wstring &clsid)
			{
				// Nilesoft Shell's own handlers ({BAE3934B-8A6A-4BFB-81BD-3FC599A1BAF0..3}).
				return clsid.size() > 30 && ::_wcsnicmp(clsid.c_str(), L"{BAE3934B-8A6A-4BFB-81BD-3FC599A1BAF", 36) == 0;
			}

			Handler describe_handler(const std::wstring &clsid)
			{
				Handler h;
				h.clsid = clsid;
				h.name = reg_string(HKEY_CLASSES_ROOT, L"CLSID\\" + clsid);
				h.module = reg_string(HKEY_CLASSES_ROOT, L"CLSID\\" + clsid + L"\\InprocServer32");
				h.windows = under_windows(h.module);
				h.program = h.windows ? std::wstring() : program_name(h.module);
				return h;
			}

			// Loads each handler of the target's keys on its own, into a menu that is
			// never shown, to learn which titles it adds.
			// quiet: the handlers that load but add nothing here. Some add their items
			// only inside File Explorer (or when their program's service runs), so the
			// map can say which ones it couldn't read.
			void read_handlers(Target &t, std::vector<std::unique_ptr<Handler>> &handlers,
							   std::vector<std::pair<string, const Handler *>> &by_name, std::vector<Handler> &quiet)
			{
				std::vector<std::wstring> seen;
				for(auto &key : t.keys)
				{
					auto base = key + L"\\shellex\\ContextMenuHandlers";
					for(auto &name : reg_subkeys(HKEY_CLASSES_ROOT, base))
					{
						auto clsid = reg_string(HKEY_CLASSES_ROOT, base + L"\\" + name);
						if(clsid.empty() && !name.empty() && name.front() == L'{')
							clsid = name;
						if(clsid.empty() || clsid.front() != L'{' || is_shell_clsid(clsid) || is_blocked(clsid))
							continue;
						bool dup = false;
						for(auto &s : seen)
							dup |= ::_wcsicmp(s.c_str(), clsid.c_str()) == 0;
						if(dup)
							continue;
						seen.push_back(clsid);

						CLSID id{};
						if(FAILED(::CLSIDFromString(clsid.c_str(), &id)))
							continue;

						IShellExtInit *raw = nullptr;
						if(FAILED(seh_create(id, &raw)) || !raw)
							continue;

						// A handler may keep the key until it is released: close it after that.
						HKEY progid{};
						::RegOpenKeyExW(HKEY_CLASSES_ROOT, key.c_str(), 0, KEY_READ, &progid);
						struct close_key
						{
							HKEY &k;
							~close_key()
							{
								if(k)
									::RegCloseKey(k);
							}
						} closer{ progid };
						ComPtr<IShellExtInit> init;
						init.Attach(raw);
						if(FAILED(seh_init(init.Get(), t.folder, background(t.where) ? nullptr : t.data.Get(), progid)))
						{
							quiet.push_back(describe_handler(clsid));
							continue;
						}

						ComPtr<IContextMenu> cm;
						if(FAILED(init.As(&cm)))
							continue;
						auto menu = ::CreatePopupMenu();
						if(!menu)
							continue;
						if(SUCCEEDED(seh_query(cm.Get(), menu, first_cmd, last_cmd, CMF_NORMAL)))
						{
							std::vector<SysItem> items;
							read_menu(menu, cm.Get(), items, {}, 0);
							std::vector<string> names;
							collect_names(items, names);
							if(!names.empty())
							{
								auto h = std::make_unique<Handler>(describe_handler(clsid));
								for(auto &n : names)
									by_name.emplace_back(n, h.get());
								handlers.push_back(std::move(h));
							}
							else
								quiet.push_back(describe_handler(clsid));
						}
						else
							quiet.push_back(describe_handler(clsid));
						::DestroyMenu(menu);
					}
				}
			}

			const wchar_t *const windows_verbs[] = {
				L"open", L"opennewwindow", L"opennewtab", L"opennewprocess", L"explore", L"find", L"cut", L"copy",
				L"paste", L"delete", L"rename", L"properties", L"link", L"sendto", L"openas", L"pintohome",
				L"pintostartscreen", L"pintotaskbar", L"copyaspath", L"runas", L"runasuser", L"print", L"edit",
				L"format", L"eject", L"undo", L"redo", L"refresh", L"pastelink", L"windows.share", L"share",
				L"windows.modernshare", L"opencontaining", L"cast", L"setdesktopwallpaper", L"rotate90",
				L"rotate270", L"extract", L"cmd", L"powershell", L"mapnetworkdrive", L"disconnectnetworkdrive",
				L"emptyrecyclebin", L"restore", L"undelete", L"newfolder", L"new", L"preview", L"play", L"enqueue",
			};

			// Who added an item: a handler that adds the same title, else the
			// program behind its verb (shell\<verb>\command), else Windows.
			void attribute_one(SysItem &item, const Target &t, const std::vector<std::pair<string, const Handler *>> &by_name,
							   std::vector<std::unique_ptr<Handler>> &handlers);

			// Items of a submenu nobody claims belong to whoever added the submenu.
			void attribute(SysItem &item, const Target &t, const std::vector<std::pair<string, const Handler *>> &by_name,
						   std::vector<std::unique_ptr<Handler>> &handlers)
			{
				attribute_one(item, t, by_name, handlers);
				std::function<void(SysItem &)> inherit = [&](SysItem &parent)
				{
					for(auto &sub : parent.items)
					{
						if(sub.type != 2 && (sub.source.empty() || sub.source == L"unknown") &&
						   !parent.source.empty() && parent.source != L"unknown")
						{
							sub.source = parent.source;
							sub.program = parent.program;
							sub.module = parent.module;
							sub.handler = parent.handler;
						}
						inherit(sub);
					}
				};
				inherit(item);
			}

			void attribute_one(SysItem &item, const Target &t, const std::vector<std::pair<string, const Handler *>> &by_name,
							   std::vector<std::unique_ptr<Handler>> &handlers)
			{
				for(auto &sub : item.items)
					attribute_one(sub, t, by_name, handlers);
				if(item.type == 2)
					return;

				if(!item.name.empty())
				{
					auto key = item_key(item);
					for(auto &[name, h] : by_name)
					{
						if(name.equals(key, true))
						{
							item.handler = h;
							item.module = h->module;
							item.source = h->windows ? L"windows" : L"program";
							item.program = h->program;
							return;
						}
					}
				}

				if(!item.verb.empty())
				{
					for(auto &key : t.keys)
					{
						auto verb_key = key + L"\\shell\\" + item.verb;
						auto command = reg_string(HKEY_CLASSES_ROOT, verb_key + L"\\command");
						std::wstring module;
						if(!command.empty())
							module = full_exe(command_exe(command));
						else
						{
							for(auto value : { L"DelegateExecute", L"ExplorerCommandHandler" })
							{
								auto clsid = reg_string(HKEY_CLASSES_ROOT, verb_key, value);
								if(!clsid.empty())
								{
									module = reg_string(HKEY_CLASSES_ROOT, L"CLSID\\" + clsid + L"\\InprocServer32");
									if(module.empty())
										module = reg_string(HKEY_CLASSES_ROOT, L"CLSID\\" + clsid + L"\\LocalServer32");
									break;
								}
							}
						}
						// "%1" %*: the file runs itself (exefile, lnkfile), Windows' verb.
						if(module.empty() || module.front() == L'%')
							continue;
						item.module = module;
						item.source = under_windows(module) ? L"windows" : L"program";
						if(item.source == L"program")
							item.program = program_name(module);
						return;
					}
					for(auto v : windows_verbs)
					{
						if(::_wcsicmp(item.verb.c_str(), v) == 0)
						{
							item.source = L"windows";
							return;
						}
					}
				}

				// Submenus and items with no verb that no handler claims: Windows'
				// own (Send to, Open with, New, View, ...) unless they hold items of
				// a program.
				if(item.type == 1)
				{
					for(auto &sub : item.items)
					{
						if(sub.source == L"program")
						{
							item.source = L"program";
							item.program = sub.program;
							item.module = sub.module;
							return;
						}
					}
				}
				item.source = item.uid != 0 && id_name(item.uid) ? L"windows" : L"unknown";
			}

			// --- Rules ---

			enum class Tri
			{
				No,
				Yes,
				Maybe
			};

			// What an expression may use and still be worked out here.
			//
			// Always: the item (this.*, unless `self` is false), the names of
			// Windows' items (id.*, title.*), strings and constants.
			//
			// With `sel` (the sample's selection is set up, see sample_selection):
			// also what the real menu would see for that selection and this
			// machine: sel.* (not sel.tofile, sel.meta or sel.drop), type.* and
			// mode.*, the window.is_* that a context decides (is_taskbar, is_edit,
			// is_start, is_titlebar; is_desktop on the desktop, a background and
			// the taskbar), sys.* that don't change while Windows runs (version,
			// folders, language), user.* folders, path.* that only read, package.*,
			// and a few plain functions (if, not, len, equals...). Keys, the time,
			// the clipboard, variables and anything that does something stay out.
			struct Known
			{
				bool self = true;		// this.*
				bool sel = false;
				bool desktop_known = false;	// window.is_desktop is decided by the context
				// Set while walking: the expression reads what makes one file or
				// folder different from another (its path, name, attributes...), not
				// only what kind of item it is.
				bool sample = false;
			};

			// sel.<Id[1]>...: true when the selection set up from the sample answers
			// it as the real one would. Sets k.sample for the parts that depend on
			// which item the sample is.
			bool sel_known(FuncExpression *f, Known &k)
			{
				if(f->Id.length() == 1)
				{
					k.sample = true;	// sel, sel(quote, sep): the paths
					return true;
				}
				switch(f->Id[1])
				{
					// What kind of selection: decided by the context (and, for a file,
					// by the type of sample chosen).
					case IDENT_COUNT:
					case IDENT_BACK:
					case IDENT_TYPE:
					case IDENT_MODE:
					case IDENT_TYPES:
					case IDENT_EXTS:
						return true;
					case IDENT_FILE:
					case IDENT_FILES:
						if(f->Id[2] != IDENT_EXT && f->Id[2] != IDENT_COUNT)
							k.sample = true;
						return true;
					case IDENT_DIRS:
					case IDENT_DIRECTORIES:
					case IDENT_PATHS:
					case IDENT_SHORTS:
					case IDENT_ROOTS:
					case IDENT_DRIVERS:
					case IDENT_NAMESPACES:
						if(f->Id[2] != IDENT_COUNT)
							k.sample = true;
						return true;
					// Which item it is.
					case IDENT_READONLY:
					case IDENT_HIDDEN:
					case IDENT_LEN:
					case IDENT_LENGTH:
					case IDENT_RAW:
					case IDENT_WORKDIR:
					case IDENT_CURDIR:
					case IDENT_FULL:
					case IDENT_PATH:
					case IDENT_SHORT:
					case IDENT_PARENT:
					case IDENT_LOCATION:
					case IDENT_ROOT:
					case IDENT_ITEM:
					case IDENT_NAME:
					case IDENT_TITLE:
					case IDENT_DIR:
					case IDENT_DIRECTORY:
					case IDENT_TITLES:
					case IDENT_NAMES:
					case IDENT_INDEX:
					case IDENT_I:
					case IDENT_GET:
					case IDENT_LNK:
					case IDENT_LNKTARGET:
						k.sample = true;
						return true;
					default:
						return false;	// sel.tofile writes a file; sel.meta, sel.drop need Explorer
				}
			}

			bool window_known(FuncExpression *f, const Known &k)
			{
				switch(f->Id[1])
				{
					case IDENT_IS_TASKBAR:
					case IDENT_IS_EDIT:
					case IDENT_IS_START:
					case IDENT_IS_TITLEBAR:
						return f->Id.length() == 2;
					case IDENT_IS_DESKTOP:
						return f->Id.length() == 2 && k.desktop_known;
					default:
						return false;
				}
			}

			bool sys_known(FuncExpression *f)
			{
				if(f->Id.length() == 1)
					return true;
				switch(f->Id[1])
				{
					case IDENT_TYPE:
					case IDENT_IS64:
					case IDENT_IS11:
					case IDENT_IS10:
					case IDENT_IS81:
					case IDENT_IS8:
					case IDENT_IS7:
					case IDENT_ISOREARLIER:
					case IDENT_ISORGREATER:
					case IDENT_IS7ORGREATER:
					case IDENT_IS8ORGREATER:
					case IDENT_IS81ORGREATER:
					case IDENT_IS10ORGREATER:
					case IDENT_S11ORGREATER:
					case IDENT_IS7OREARLIER:
					case IDENT_IS8OREARLIER:
					case IDENT_IS81OREARLIER:
					case IDENT_IS10OREARLIER:
					case IDENT_IS11OREARLIER:
					case IDENT_VER:
					case IDENT_VERSION:
					case IDENT_NAME:
					case IDENT_ROOT:
					case IDENT_PATH:
					case IDENT_DIR:
					case IDENT_DIRECTORY:
					case IDENT_BIN:
					case IDENT_BIN64:
					case IDENT_BIN32:
					case IDENT_WOW:
					case IDENT_PROG:
					case IDENT_PROG32:
					case IDENT_TEMP:
					case IDENT_APPDATA:
					case IDENT_PROGRAMDATA:
					case IDENT_USERS:
					case IDENT_TEMPLATES:
					case IDENT_EXPAND:
					case IDENT_LANG:
					case IDENT_LOC:
						return true;
					default:
						return false;	// the time, power, memory, the theme, the monitor...
				}
			}

			// path.* that only read a path or the file system (not path.files,
			// which lists a folder, or the dialogs, path.refresh, path.lnk.create).
			bool path_known(FuncExpression *f)
			{
				if(f->Id.length() != 2)
					return false;
				switch(f->Id[1])
				{
					case IDENT_ROOT:
					case IDENT_NAME:
					case IDENT_TITLE:
					case IDENT_SHORT:
					case IDENT_FULL:
					case IDENT_PARENT:
					case IDENT_LOCATION:
					case IDENT_EMPTY:
					case IDENT_EXISTS:
					case IDENT_JOIN:
					case IDENT_COMBINE:
					case IDENT_ISABSOLUTE:
					case IDENT_ISRELATIVE:
					case IDENT_ISFILE:
					case IDENT_ISDIR:
					case IDENT_ISDIRECTORY:
					case IDENT_ISROOT:
					case IDENT_ISDRIVE:
					case IDENT_ISCLSID:
					case IDENT_ISNAMESPACE:
					case IDENT_TYPE:
					case IDENT_ISEXE:
					case IDENT_REMOVEEXTENSION:
					case IDENT_GETKNOWNFOLDER:
					case IDENT_REMOVE_ARGS:
					case IDENT_ARGS:
					case IDENT_QUOTE_SPACES:
					case IDENT_REMOVE_EXTENSION:
					case IDENT_RENAME_EXTENSION:
					case IDENT_ADD_EXTENSION:
						return true;
					default:
						return false;
				}
			}

			bool knowable(Expression *e, Known &k)
			{
				if(!e)
					return true;
				switch(e->Type())
				{
					case ExpressionType::Null:
					case ExpressionType::Literal:
					case ExpressionType::String:
					case ExpressionType::Number:
					case ExpressionType::Color:
						return true;
					case ExpressionType::Array:
					{
						auto a = static_cast<ArrayExpression *>(e);
						if(!a)
							return false;
						for(auto x : a->items())
						{
							if(!knowable(x, k))
								return false;
						}
						return true;
					}
					case ExpressionType::Statement:
					{
						auto s = static_cast<StatementExpression *>(e);
						if(!s)
							return false;
						for(auto x : s->Body)
						{
							if(!knowable(x, k))
								return false;
						}
						return true;
					}
					case ExpressionType::BinaryOperator:
					{
						auto b = static_cast<BinaryExpression *>(e);
						return b && knowable(b->Left, k) && knowable(b->Right, k);
					}
					case ExpressionType::UnaryOperator:
					{
						auto u = static_cast<UnaryExpression *>(e);
						return u && knowable(u->Operand, k);
					}
					case ExpressionType::Ternary:
					{
						auto t = static_cast<TernaryExpression *>(e);
						return t && knowable(t->Condition, k) && knowable(t->True, k) && knowable(t->False, k);
					}
					case ExpressionType::Identifier:
					{
						auto f = static_cast<FuncExpression *>(e);
						if(!f || f->Array)
							return false;
						switch(f->Id[0])
						{
							case IDENT_THIS:
								if(!k.self)
									return false;
								break;
							case IDENT_ID:
							case IDENT_STR:
							case IDENT_TITLE:
							case IDENT_LOC:
							case IDENT_VIS:
							case IDENT_MODE:
							case IDENT_POS:
							case IDENT_INDEXOF:
							case IDENT_QUOTE:
							case IDENT_SEP:
							case IDENT_TRUE:
							case IDENT_FALSE:
							case IDENT_NULL:
								break;
							// Plain keywords: pos=bottom, vis=disable, mode=single...
							case IDENT_NIL:
							case IDENT_OK:
							case IDENT_YES:
							case IDENT_NO:
							case IDENT_DEFAULT:
							case IDENT_AUTO:
							case IDENT_ENABLE:
							case IDENT_DISABLE:
							case IDENT_ENABLED:
							case IDENT_DISABLED:
							case IDENT_INHERIT:
							case IDENT_NONE:
							case IDENT_BOTH:
							case IDENT_TOP:
							case IDENT_BOTTOM:
							case IDENT_MIDDLE:
							case IDENT_BEFORE:
							case IDENT_AFTER:
							case IDENT_REMOVE:
							case IDENT_HIDDEN:
							case IDENT_MODE_SINGLE:
							case IDENT_MODE_UNIQUE:
							case IDENT_MODE_MULTIPLE:
							case IDENT_MODE_MULTI_SINGLE:
							case IDENT_MODE_MULTI_UNIQUE:
							case IDENT_MODE_MULTI:
							case IDENT_VIS_STATIC:
							case IDENT_VIS_LABEL:
								if(f->Id.length() != 1)
									return false;
								break;
							case IDENT_SEL:
								if(!k.sel || !sel_known(f, k))
									return false;
								break;
							case IDENT_WINDOW:
							case IDENT_WND:
								if(!k.sel || !window_known(f, k))
									return false;
								break;
							case IDENT_SYS:
							case IDENT_SYSTEM:
								if(!k.sel || !sys_known(f))
									return false;
								break;
							case IDENT_PATH:
								if(!k.sel || !path_known(f))
									return false;
								break;
							case IDENT_TYPE:
							case IDENT_USER:
							case IDENT_PACKAGE:
							case IDENT_APPX:
							case IDENT_UWP:
							case IDENT_IF:
							case IDENT_NOT:
							case IDENT_LEN:
							case IDENT_LENGTH:
							case IDENT_EQUALS:
							case IDENT_EQUAL:
							case IDENT_GREATER:
							case IDENT_LESS:
							case IDENT_TOINT:
							case IDENT_TODOUBLE:
							case IDENT_TOUINT:
							case IDENT_TOFLOAT:
								if(!k.sel)
									return false;
								break;
							default:
								return false;
						}
						for(auto x : f->Arguments)
						{
							if(!knowable(x, k))
								return false;
						}
						return !f->Child || knowable(f->Child, k);
					}
					default:
						return false;
				}
			}

			// True when evaluating `e` needs nothing but the item (this.*), the
			// names of Windows' items (id.*, title.*), strings and constants.
			bool is_safe(Expression *e)
			{
				Known k;
				return knowable(e, k);
			}

			// The selection the real menu would have for a right-click in `where`
			// on `sample`, as Selections::QuerySelected and ContextMenu fill it in
			// Explorer: the same Parse() of the item, its parent, Background for a
			// folder's or the desktop's background, then Preparing(). False when it
			// can't be made (a context without one, a sample that can't be opened).
			bool sample_selection(Selections &s, const std::wstring &where, const std::wstring &sample)
			{
				if(where == L"taskbar")
				{
					// ContextMenu::Initialize for a taskbar: no items.
					s.Window.id = WINDOW_TASKBAR;
					s.Window.parent = WINDOW_TASKBAR;
					s.Types[FSO_TASKBAR] = TRUE;
					s.front = FSO_TASKBAR;
					s.Directory = Path::GetKnownFolder(FOLDERID_Desktop).move();
					s.Preparing();
					return true;
				}

				ComPtr<IShellItem> item;
				HRESULT hr = E_FAIL;
				if(where == L"thispc")
					hr = ::SHGetKnownFolderItem(FOLDERID_ComputerFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&item));
				else if(where == L"desktop")
					hr = ::SHGetKnownFolderItem(FOLDERID_Desktop, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&item));
				else if((where == L"file" || where == L"folder" || where == L"drive" || where == L"back") && !sample.empty())
					hr = ::SHCreateItemFromParsingName(sample.c_str(), nullptr, IID_PPV_ARGS(&item));
				else
					return false;
				if(FAILED(hr) || !item)
					return false;

				s.Window.id = where == L"desktop" ? WINDOW_DESKTOP : WINDOW_EXPLORER;
				s.Window.desktop = where == L"desktop";
				s.Background = background(where);
				if(!s.Parse(item.Get()))
					return false;

				if(where == L"desktop")
				{
					s.Parent = Path::Parent(s.Items[0]->Path).move();
					s.ParentRaw = Path::Parent(s.Items[0]->Raw).move();
				}
				else
				{
					ComPtr<IShellItem> parent;
					FileProperties fp;
					if(SUCCEEDED(item->GetParent(&parent)) && parent && Selections::GetFileProperties(parent.Get(), &fp))
					{
						s.Parent = fp.Path.move();
						s.ParentRaw = fp.PathRaw.move();
					}
				}
				s.Preparing();
				if(s.Directory.empty())
					s.Directory = Path::CurrentDirectory().move();
				return true;
			}
		}

		bool Parser::ReportTree(std::wstring &json, const wchar_t *where_arg, const wchar_t *sample_arg)
		{
			int32_t types[FSO_MAX]{};
			bool back = false, filter = false;
			size_t count = 1;
			if(!where_arg || !*where_arg || !WhereTypes(where_arg, types, back, count, filter))
				return false;

			std::wstring where = where_arg;
			for(auto &c : where)
				c = towlower(c);
			if(where == L"background")
				where = L"back";
			if(where == L"dir")
				where = L"folder";
			if(where == L"computer")
				where = L"thispc";

			// The selection the real menu would have here: sel.* in where=, find=
			// and vis= are evaluated against it (Known, knowable()).
			Initializer::ensure_com();
			Selections sample_sel;
			const bool have_sel = sample_selection(sample_sel, where, sample_arg ? sample_arg : L"");
			// A sample file or folder (or the system drive) stands for any of its
			// kind: what depends on which one it is is marked "for this sample".
			// This PC, the desktop and the taskbar are the real ones.
			const bool placeholder = where == L"file" || where == L"folder" || where == L"back" || where == L"drive";
			struct sel_scope
			{
				Context &c;
				Selections *old;
				~sel_scope() { c.Selections = old; }
			} restore_sel{ context, context.Selections };
			if(have_sel)
				context.Selections = &sample_sel;

			auto known = [&](bool self)
			{
				Known k;
				k.self = self;
				k.sel = have_sel;
				k.desktop_known = where == L"desktop" || where == L"back" || where == L"taskbar";
				return k;
			};

			// Whether a condition is true here: And, Or and Not are worked out part by
			// part, so `sel.raw == X and key.shift()` is No wherever sel.raw isn't X.
			// k.sample: the answer depends on which file or folder the sample is.
			std::function<Tri(Expression *, Known &)> tri = [&](Expression *e, Known &k) -> Tri
			{
				if(!e)
					return Tri::Yes;
				if(e->Type() == ExpressionType::BinaryOperator)
				{
					auto b = static_cast<BinaryExpression *>(e);
					if(b && (b->Operator == OperatorType::LogicalAnd || b->Operator == OperatorType::LogicalOr))
					{
						Known kl = k, kr = k;
						kl.sample = kr.sample = false;
						auto l = tri(b->Left, kl), r = tri(b->Right, kr);
						// The side that decides is the one whose sample-dependence counts:
						// for And a No, for Or a Yes (both when both decide).
						const Tri decisive = b->Operator == OperatorType::LogicalAnd ? Tri::No : Tri::Yes;
						if(l == decisive || r == decisive)
						{
							k.sample |= l == decisive && r == decisive ? kl.sample && kr.sample
									  : l == decisive ? kl.sample : kr.sample;
							return decisive;
						}
						k.sample |= kl.sample || kr.sample;
						if(l == Tri::Maybe || r == Tri::Maybe)
							return Tri::Maybe;
						return decisive == Tri::No ? Tri::Yes : Tri::No;
					}
				}
				if(e->Type() == ExpressionType::UnaryOperator)
				{
					auto u = static_cast<UnaryExpression *>(e);
					if(u && (u->Operator == OperatorType::LogicalNot || u->Operator == OperatorType::UnaryNot))
					{
						auto v = tri(u->Operand, k);
						return v == Tri::Maybe ? v : v == Tri::Yes ? Tri::No : Tri::Yes;
					}
				}
				Known part = k;
				part.sample = false;
				if(!knowable(e, part))
					return Tri::Maybe;
				// sel.raw (or sel.path) == '::{...}': a namespace such as This PC or the
				// Recycle Bin. A file's, folder's or drive's path never is one, so that
				// is decided by the kind of item, whichever one the sample is.
				if(placeholder && e->Type() == ExpressionType::BinaryOperator)
				{
					auto b = static_cast<BinaryExpression *>(e);
					auto is_raw = [](Expression *x)
					{
						if(!x || x->Type() != ExpressionType::Identifier)
							return false;
						auto f = static_cast<FuncExpression *>(x);
						return f->Id.length() == 2 && f->Id[0] == IDENT_SEL && (f->Id[1] == IDENT_RAW || f->Id[1] == IDENT_PATH) &&
							   f->Arguments.empty() && !f->Child && !f->Array;
					};
					auto is_namespace = [](Expression *x)
					{
						// A quoted string is a statement around the string.
						while(x && x->Type() == ExpressionType::Statement)
						{
							auto st = static_cast<StatementExpression *>(x);
							x = st->Body.size() == 1 ? st->Body[0] : nullptr;
						}
						return x && x->Type() == ExpressionType::String &&
							   static_cast<StringExpression *>(x)->Value.to_string().starts_with(L"::");
					};
					if(b && (b->Operator == OperatorType::EqualEqual || b->Operator == OperatorType::NotEqual) &&
					   ((is_raw(b->Left) && is_namespace(b->Right)) || (is_namespace(b->Left) && is_raw(b->Right))))
						part.sample = false;
				}
				try
				{
					auto v = context.Eval(e).to_bool() ? Tri::Yes : Tri::No;
					k.sample |= part.sample;
					return v;
				}
				catch(...)
				{
					return Tri::Maybe;
				}
			};

			auto declared = [&](NativeMenu *item, TitleInfo &title, std::wstring &o)
			{
				auto it = m_items.find(item);
				const string &file = it != m_items.end() ? it->second.file : title.file;
				jkey(o, L"file");
				jstr(o, file);
				jnum(o, L"line", static_cast<long long>(it != m_items.end() ? it->second.line : title.line));
				jnum(o, L"column", static_cast<long long>(it != m_items.end() ? it->second.column : title.column));
			};

			// A constant vis= (vis=0, vis=vis.remove, 'disable'), or "dynamic".
			auto visibility = [&](Expression *e) -> std::wstring
			{
				if(!e)
					return L"";
				if(!is_safe(e))
					return L"dynamic";
				try
				{
					switch(context.parse_visibility(e, Visibility::Enabled))
					{
						case Visibility::Hidden: return L"hidden";
						case Visibility::Disabled: return L"disabled";
						case Visibility::Static: return L"static";
						case Visibility::Label: return L"label";
						default: return L"enabled";
					}
				}
				catch(...)
				{
					return L"dynamic";
				}
			};

			auto text_of = [&](Expression *e, bool &dynamic) -> std::wstring
			{
				dynamic = false;
				if(!e)
					return {};
				if(!is_safe(e))
				{
					dynamic = true;
					return {};
				}
				try
				{
					string s;
					context.Eval(e, s, true);
					return s.empty() ? std::wstring() : std::wstring(s.c_str());
				}
				catch(...)
				{
					dynamic = true;
					return {};
				}
			};

			// --- Shell's own items ---
			// Read first into a tree, so menu= and pos= can be applied as the menu
			// applies them: an item with menu='Title' (or 'A/B') goes into that menu
			// of this tree ('/' is the main menu), then each menu's items are put in
			// the order pos= gives them (MenuPlacement.h).
			struct ShellNode
			{
				NativeMenu *item{};
				uint32_t depth{};
				TitleInfo title;
				string normalized;			// the title as indexof() matches it
				std::wstring menu;			// a constant menu=, without the outer '/'
				bool menu_dynamic = false;
				bool to_root = false;		// menu='/'
				bool moved = false;			// placed in another menu than its own here
				bool expanded = false;		// expanded=1 (a constant): its items show in its place
				std::wstring moved_from;	// the menu it is written in ("" the main menu)
				std::wstring pos;			// top, bottom, middle, N, before:<title>, after:<title>, indexof
				Placement::Spec spec;
				string indexof;				// the pattern of pos=indexof()
				// Whether it shows for the sample, by its own where=, find= and a vis=
				// that depends on the selection (a constant vis= is in "vis"), and
				// then also by the menu it ends up in (shows).
				Tri own = Tri::Yes;
				bool own_sample = false;
				const wchar_t *own_why = L"";	// where | find | vis: what makes it No or Maybe
				Tri shows = Tri::Yes;
				bool shows_sample = false;
				const wchar_t *why = L"";		// own_why, or "menu" when its menu isn't here
				std::vector<std::unique_ptr<ShellNode>> children;
			};

			// As ContextMenu::prepare_new_items decides it (type= is already
			// matched): where=, then vis= (hidden leaves it out; disabled still
			// shows), then find= against each selected item's title, extension and
			// path. mode= can't leave out an item for one selected item.
			auto shows_of = [&](ShellNode &n)
			{
				auto item = n.item;
				auto merge = [&](Tri t, bool sample, const wchar_t *why)
				{
					if(t == Tri::Yes)
					{
						n.own_sample |= sample;
						return;
					}
					if(n.own == Tri::No)
						return;
					if(t == Tri::No)
					{
						n.own = Tri::No;
						n.own_sample = sample;
						n.own_why = why;
					}
					else if(n.own == Tri::Yes)
					{
						n.own = Tri::Maybe;
						n.own_why = why;
					}
				};
				if(item->where.get())
				{
					auto k = known(false);
					auto t = tri(item->where, k);
					merge(t, k.sample, L"where");
				}
				if(item->visibility.get() && !is_safe(item->visibility))
				{
					auto k = known(false);
					Tri t = Tri::Maybe;
					if(knowable(item->visibility, k))
					{
						try
						{
							t = context.parse_visibility(item->visibility, Visibility::Enabled) == Visibility::Hidden ? Tri::No : Tri::Yes;
						}
						catch(...)
						{
						}
					}
					merge(t, k.sample, L"vis");
				}
				if(item->find.get() && !item->is_separator())
				{
					Tri t = Tri::Maybe;
					bool sample = placeholder;
					if(have_sel)
					{
						auto k = known(false);
						if(knowable(item->find, k))
						{
							try
							{
								string value;
								context.Eval(item->find, value, true);
								FindPattern fp;
								if(value.empty() || !fp.split(value, L'|'))
									t = Tri::Yes, sample = false;
								else
								{
									size_t found = 0;
									for(auto s : sample_sel.Items)
									{
										string ext = s->Extension.substr(1).move();
										if(!fp(&s->Title, s->IsFile() ? &ext : nullptr, &s->Path))
										{
											found = 0;
											break;
										}
										found++;
									}
									t = found ? Tri::Yes : Tri::No;
									sample = placeholder || k.sample;
								}
							}
							catch(...)
							{
							}
						}
					}
					merge(t, sample, L"find");
				}
			};
			using ShellList = std::vector<std::unique_ptr<ShellNode>>;

			auto position_of = [&](ShellNode &n)
			{
				auto e = n.item->position.get();
				if(!e)
					return;
				if(!is_safe(e))
				{
					n.pos = L"dynamic";
					return;
				}
				try
				{
					Object obj = context.Eval(e).move();
					if(obj.is_array(true))
					{
						auto ptr = obj.get_pointer();
						if((uint32_t)ptr[0] != IDENT_INDEXOF)
							return;
						int ac = ptr[1];
						n.indexof = ptr[2].to_string().move();
						n.spec.kind = Placement::Kind::IndexOf;
						n.spec.offset = ptr[3];
						n.spec.def = -1;
						if(ac == 3)
						{
							int d = ptr[4].is_number() ? ptr[4].to_number<int>() : -1;
							n.spec.def = d >= 0 ? d : (int)context.parse_pos(ptr[4], Position::Auto);
						}
						// '"Title"' with offset 0 or 1: right before or right after that item.
						std::wstring pattern = n.indexof.empty() ? std::wstring() : std::wstring(n.indexof.c_str());
						if(pattern.size() >= 2 && pattern.front() == L'"' && pattern.back() == L'"' &&
						   pattern.find(L'|') == std::wstring::npos && (n.spec.offset == 0 || n.spec.offset == 1))
							n.pos = (n.spec.offset == 0 ? L"before:" : L"after:") + pattern.substr(1, pattern.size() - 2);
						else
							n.pos = L"indexof";
						return;
					}
					if(obj.is_null())
						return;
					auto p = context.parse_pos(obj, Position::Auto);
					switch(p)
					{
						case Position::Top: n.spec.kind = Placement::Kind::Top; n.pos = L"top"; break;
						case Position::Bottom: n.spec.kind = Placement::Kind::Bottom; n.pos = L"bottom"; break;
						case Position::Middle: n.spec.kind = Placement::Kind::Middle; n.pos = L"middle"; break;
						case Position::Auto:
						case Position::None: n.pos = L"auto"; break;
						default:
							n.spec.kind = Placement::Kind::Index;
							n.spec.index = static_cast<int>(p);
							n.pos = std::to_wstring(static_cast<int>(p));
							break;
					}
				}
				catch(...)
				{
					n.pos = L"dynamic";
				}
			};

			std::function<void(std::vector<NativeMenu *> &, uint32_t, ShellList &)> read =
				[&](std::vector<NativeMenu *> &items, uint32_t depth, ShellList &out)
			{
				for(auto item : items)
				{
					if(!item)
						continue;
					if(!Selections::match_types(item->fso, types, back, count))
						continue;
					auto n = std::make_unique<ShellNode>();
					n->item = item;
					n->depth = depth;
					auto kind = item->type == NativeMenuType::Menu ? TitleInfo::Menu : TitleInfo::Item;
					n->title = DescribeTitle(item, depth, kind);
					string t = n->title.text;
					MenuItemInfo::normalize(t, &n->normalized);
					if(!item->is_separator())
					{
						if(item->moveto.get())
						{
							auto all = text_of(item->moveto, n->menu_dynamic);
							std::wstring m = all;
							while(!m.empty() && (m.front() == L'/' || iswspace(m.front())))
								m.erase(0, 1);
							while(!m.empty() && (m.back() == L'/' || iswspace(m.back())))
								m.pop_back();
							n->menu = m;
							n->to_root = !n->menu_dynamic && m.empty() && all.find(L'/') != std::wstring::npos;
						}
					}
					// expanded=1: the menu shows its items in its place, and the Windows
					// items a modify(menu=) moves into it show there too (ContextMenu.cpp).
					if(item->is_menu() && item->expanded.get() && is_safe(item->expanded))
					{
						try
						{
							n->expanded = context.Eval(item->expanded).to_bool();
						}
						catch(...)
						{
						}
					}
					position_of(*n);
					shows_of(*n);
					read(item->items, depth + 1, n->children);
					out.push_back(std::move(n));
				}
			};
			ShellList shell_root;
			read(context.Cache->dynamic.items, 0, shell_root);

			// The menu each item is written in, as a path of titles.
			std::function<void(ShellList &, const std::wstring &)> origin = [&](ShellList &list, const std::wstring &path)
			{
				for(auto &n : list)
				{
					n->moved_from = path;
					// c_str() of an empty string is null (a separator's title).
					std::wstring title = n->title.text.empty() ? std::wstring() : std::wstring(n->title.text.c_str());
					origin(n->children, path.empty() ? title : path + L"/" + title);
				}
			};
			origin(shell_root, L"");

			// menu=: the item leaves its menu for the end of the one it names, found by
			// its titles as the menu matches them (any case, '&' left out). When that
			// menu isn't in this context the item stays where it is written (in the real
			// menu it doesn't show).
			auto find_menu = [&](const std::wstring &path, const ShellNode *self) -> ShellList *
			{
				std::vector<std::wstring> parts;
				size_t at = 0;
				while(at <= path.size())
				{
					auto slash = path.find(L'/', at);
					auto part = path.substr(at, slash == std::wstring::npos ? std::wstring::npos : slash - at);
					while(!part.empty() && iswspace(part.front()))
						part.erase(0, 1);
					while(!part.empty() && iswspace(part.back()))
						part.pop_back();
					if(!part.empty())
						parts.push_back(part);
					if(slash == std::wstring::npos)
						break;
					at = slash + 1;
				}
				if(parts.empty())
					return nullptr;
				auto *list = &shell_root;
				for(auto &part : parts)
				{
					auto h = MenuItemInfo::hashing(part.c_str());
					ShellNode *hit = nullptr;
					for(auto &c : *list)
					{
						if(c.get() == self || !c->item->is_menu() || c->title.state != TitleInfo::Text)
							continue;
						if(MenuItemInfo::hashing(c->title.text) == h)
						{
							hit = c.get();
							break;
						}
					}
					if(!hit)
						return nullptr;
					list = &hit->children;
				}
				return list;
			};
			std::function<bool(const ShellNode *, const ShellList *)> inside = [&](const ShellNode *n, const ShellList *list) -> bool
			{
				if(list == &n->children)
					return true;
				for(auto &c : n->children)
				{
					if(inside(c.get(), list))
						return true;
				}
				return false;
			};
			struct Move { ShellList *from; ShellNode *node; };
			std::vector<Move> moves;
			std::function<void(ShellList &, bool)> collect = [&](ShellList &list, bool top)
			{
				for(auto &n : list)
				{
					if(!n->menu.empty() || (n->to_root && !top))
						moves.push_back({ &list, n.get() });
					collect(n->children, false);
				}
			};
			collect(shell_root, true);
			for(auto &m : moves)
			{
				auto target = m.node->to_root ? &shell_root : find_menu(m.node->menu, m.node);
				if(!target || target == m.from || inside(m.node, target))
					continue;
				auto it = std::find_if(m.from->begin(), m.from->end(), [&](const auto &x) { return x.get() == m.node; });
				if(it == m.from->end())
					continue;
				auto owned = std::move(*it);
				m.from->erase(it);
				owned->moved = true;
				// The items inside it now live in the target's tree: moves listed for them
				// still point at their own lists, which moved with it.
				target->push_back(std::move(owned));
			}

			// pos=: each menu in the order it shows.
			std::function<void(ShellList &)> arrange = [&](ShellList &list)
			{
				for(auto &n : list)
					arrange(n->children);
				bool any = false;
				std::vector<ShellNode *> plain;
				for(auto &n : list)
				{
					plain.push_back(n.get());
					any |= n->spec.kind != Placement::Kind::Auto;
				}
				if(!any)
					return;
				auto order = Placement::arrange(plain,
					[](ShellNode *n) -> const Placement::Spec & { return n->spec; },
					[](const std::vector<ShellNode *> &items, ShellNode *n) -> std::ptrdiff_t
					{
						// The patterns in order ("a|b": b only when nothing matches a).
						FindPattern fp;
						if(!fp.split(n->indexof, L'|'))
							return -1;
						for(const auto &pat : fp.matches)
						{
							for(size_t i = 0; i < items.size(); i++)
							{
								if(fp.find(pat, &items[i]->normalized))
									return static_cast<std::ptrdiff_t>(i);
							}
						}
						return -1;
					},
					[](ShellNode *a, ShellNode *b)
					{
						return a->spec.offset == b->spec.offset && a->spec.def == b->spec.def &&
							a->indexof.equals(b->indexof, false);
					});
				ShellList sorted;
				for(auto p : order)
				{
					for(auto &n : list)
					{
						if(n.get() == p)
						{
							sorted.push_back(std::move(n));
							break;
						}
					}
				}
				list = std::move(sorted);
			};
			// The main menu's items in the order the menu takes them (config order, then
			// those menu= brings here), for the order with Windows' items (below).
			std::vector<ShellNode *> shell_config;
			for(auto &n : shell_root)
				shell_config.push_back(n.get());
			arrange(shell_root);

			// Shown in the menu it ends up in: not when that menu isn't shown. (An
			// item that menu= takes out of a menu that isn't shown can still show:
			// it now hangs under the menu it went to.)
			std::function<void(ShellList &, Tri, bool)> effective = [&](ShellList &list, Tri parent, bool parent_sample)
			{
				for(auto &n : list)
				{
					if(parent == Tri::No)
					{
						n->shows = Tri::No;
						n->shows_sample = parent_sample;
						n->why = L"menu";
					}
					else
					{
						n->shows = n->own == Tri::Yes ? parent : n->own;
						n->shows_sample = n->own_sample || (n->own == Tri::Yes && parent_sample);
						n->why = n->own != Tri::Yes ? n->own_why : parent == Tri::Maybe ? L"menu" : L"";
					}
					effective(n->children, n->shows, n->shows_sample);
				}
			};
			effective(shell_root, Tri::Yes, false);

			std::wstring shell;
			std::function<void(ShellList &)> walk = [&](ShellList &list)
			{
				bool first = true;
				for(auto &np : list)
				{
					auto &n = *np;
					auto item = n.item;
					auto &title = n.title;
					shell += first ? L"{" : L",{";
					first = false;
					jtext(shell, L"kind", item->is_separator() ? L"separator" : item->type == NativeMenuType::Menu ? L"menu" : L"item");
					jkey(shell, L"title");
					jstr(shell, title.text);
					jtext(shell, L"state", !item->title.get() ? L"none" : title.state == TitleInfo::Text ? L"text" :
						  title.state == TitleInfo::Empty ? L"empty" : L"dynamic");
					jkey(shell, L"source");
					jstr(shell, title.source);
					declared(item, title, shell);
					jtext(shell, L"vis", visibility(item->visibility));
					std::wstring conds;
					auto cond = [&](bool on, const wchar_t *name)
					{
						if(!on)
							return;
						if(!conds.empty())
							conds += L',';
						conds += L'"';
						conds += name;
						conds += L'"';
					};
					cond(item->where.get() != nullptr, L"where");
					cond(item->find.get() != nullptr, L"find");
					cond(item->mode.get() != nullptr, L"mode");
					cond(item->visibility.get() && !is_safe(item->visibility), L"vis");
					cond(item->fso.defined && item->fso.count > 0, L"type");
					jkey(shell, L"conditions");
					shell += L"[" + conds + L"]";
					jbool(shell, L"dynamic", item->for_each != 0 || (item->title.get() && title.state == TitleInfo::Dynamic));
					// menu= and pos= as written (menu "/" is the main menu), whether menu=
					// put it in another menu here, and the menu it is written in.
					jtext(shell, L"menu", n.to_root ? std::wstring(L"/") : n.menu);
					jbool(shell, L"menu_dynamic", n.menu_dynamic);
					jbool(shell, L"moved", n.moved);
					jbool(shell, L"expanded", n.expanded);
					jtext(shell, L"moved_from", n.moved_from);
					jtext(shell, L"pos", n.pos);
					// yes | no | maybe: whether it shows in this menu for the sample, whether
					// that depends on which file or folder the sample is, and what decides it
					// (where, find, vis, or menu: the menu it is in).
					jtext(shell, L"shows", n.shows == Tri::Yes ? L"yes" : n.shows == Tri::No ? L"no" : L"maybe");
					jbool(shell, L"shows_sample", placeholder && n.shows_sample);
					jtext(shell, L"shows_why", n.why);
					jkey(shell, L"items");
					shell += L"[";
					walk(n.children);
					shell += L"]}";
				}
			};
			// Written once the Windows menu is read: a menu of yours that nothing in it
			// shows in is dropped, unless a rule moves Windows' items into it.

			// --- The rules ---
			auto &statics = context.Cache->statics;
			struct RuleInfo
			{
				bool applies = false;	// type= lets it reach this context
				Tri find_static = Tri::Maybe;
				std::wstring find;
				bool find_dynamic = false;
				std::wstring in;
				bool in_dynamic = false;
				std::wstring vis;
				std::wstring menu;
				bool menu_dynamic = false;
				std::wstring title;
				bool title_dynamic = false;
			};
			std::vector<RuleInfo> infos(statics.size());
			std::wstring rules;
			for(size_t i = 0; i < statics.size(); i++)
			{
				auto si = statics[i];
				auto &ri = infos[i];
				if(!si)
					continue;
				ri.applies = !si->has_clsid && Selections::match_types(si->fso, types, back, count);
				ri.find = text_of(si->find, ri.find_dynamic);
				ri.in = text_of(si->location, ri.in_dynamic);
				ri.vis = visibility(si->visibility);
				ri.menu = text_of(si->moveto, ri.menu_dynamic);
				ri.title = text_of(si->title, ri.title_dynamic);

				rules += i ? L",{" : L"{";
				jnum(rules, L"index", static_cast<long long>(i));
				auto it = m_items.find(si);
				jkey(rules, L"file");
				jstr(rules, it != m_items.end() ? it->second.file : string());
				jnum(rules, L"line", it != m_items.end() ? static_cast<long long>(it->second.line) : 0);
				jnum(rules, L"column", it != m_items.end() ? static_cast<long long>(it->second.column) : 0);
				jbool(rules, L"applies", ri.applies);
				jbool(rules, L"clsid", si->has_clsid);
				jtext(rules, L"find", ri.find);
				jbool(rules, L"find_dynamic", ri.find_dynamic);
				jtext(rules, L"in", ri.in);
				jtext(rules, L"where", !si->where.get() ? L"" : is_safe(si->where) ? L"item" : L"selection");
				jtext(rules, L"vis", ri.vis);
				jtext(rules, L"menu", ri.menu);
				jbool(rules, L"menu_dynamic", ri.menu_dynamic);
				jtext(rules, L"title", ri.title);
				jbool(rules, L"title_dynamic", ri.title_dynamic);
				jbool(rules, L"pos", si->position.get() != nullptr);
				jbool(rules, L"sep", si->separator.get() != nullptr);
				jbool(rules, L"image", si->image.expr != nullptr || si->images.normal.get() != nullptr);
				rules += L"}";
			}

			// --- The Windows menu ---
			std::wstring system, system_error;
			std::vector<SysItem> sys;
			std::vector<std::unique_ptr<Handler>> handlers;
			std::vector<Handler> quiet;
			if(where == L"taskbar")
			{
				system_error = L"taskbar";
			}
			else
			{
				g_reading_menus = true;
				Initializer::ensure_com();
				Target t;
				t.where = where;
				t.sample = sample_arg ? sample_arg : L"";
				ComPtr<IContextMenu> cm;
				auto hr = open_menu(t, cm, system_error);
				if(SUCCEEDED(hr) && cm)
				{
					auto menu = ::CreatePopupMenu();
					UINT flags = CMF_NORMAL | (background(where) ? 0 : CMF_CANRENAME | CMF_ITEMMENU);
					hr = seh_query(cm.Get(), menu, first_cmd, last_cmd, flags);
					if(SUCCEEDED(hr))
						read_menu(menu, cm.Get(), sys, {}, 0);
					else
						system_error = L"the menu couldn't be built";
					::DestroyMenu(menu);

					association_keys(t);
					std::vector<std::pair<string, const Handler *>> by_name;
					read_handlers(t, handlers, by_name, quiet);
					for(auto &item : sys)
						attribute(item, t, by_name, handlers);

					if(background(where))
					{
						std::vector<SysItem> view;
						read_view_menu(where == L"desktop", view);
						if(!view.empty())
						{
							SysItem sep;
							sep.type = 2;
							view.push_back(std::move(sep));
							for(auto &item : sys)
								view.push_back(std::move(item));
							sys = std::move(view);
						}
					}
				}
				else if(system_error.empty())
				{
					wchar_t buf[64];
					swprintf_s(buf, L"the menu couldn't be built (0x%08lx)", static_cast<unsigned long>(hr));
					system_error = buf;
				}
				g_reading_menus = false;
			}

			// Which rules reach which item, as build_main_system_menuitems decides.
			this_item self;
			// this.* is the Windows item; sel.* the sample's selection (tri, above).
			auto eval_tri = [&](Expression *e) -> Tri
			{
				auto k = known(true);
				return tri(e, k);
			};

			std::function<void(std::vector<SysItem> &, uint32_t)> match = [&](std::vector<SysItem> &items, uint32_t parent)
			{
				for(size_t pos = 0; pos < items.size(); pos++)
				{
					auto &item = items[pos];
					uint32_t self_uid = item.uid;
					if(item.type != 2 && !item.name.empty())
					{
						string raw = item.title.c_str();
						self = {};
						self.type = item.type;
						self.pos = static_cast<int>(pos);
						self.disabled = item.disabled;
						self.system = true;
						self.id = item.uid;
						self.parent = parent;
						self.title = raw.c_str();
						self.title_normalize = item.name.c_str();
						self.verb = item.verb.c_str();
						self.length = item.name.length<uint32_t>();
						context._this = &self;
						context.menu_count = static_cast<uint32_t>(items.size());
						// The menu it is in for the rules: the main menu once menu='/'
						// takes it there (to the end of the main menu's list).
						std::wstring path = item.path;

						for(size_t i = 0; i < statics.size(); i++)
						{
							auto si = statics[i];
							auto &ri = infos[i];
							if(!si || !ri.applies)
								continue;

							Tri m = Tri::Yes;
							// in=: the submenu the item is in; without it, only top-level items.
							if(si->location)
							{
								if(ri.in_dynamic)
									m = Tri::Maybe;
								else
								{
									std::wstring loc = ri.in;
									while(!loc.empty() && (loc.front() == L'/' || iswspace(loc.front())))
										loc.erase(0, 1);
									while(!loc.empty() && (loc.back() == L'/' || iswspace(loc.back())))
										loc.pop_back();
									bool ok;
									if(loc.empty())
										ok = path.empty();
									else if(loc == L"*")
										ok = true;
									else
									{
										if(loc.rfind(L"**", 0) == 0)
											loc.erase(0, 1);
										ok = !path.empty() && ::_wcsicmp(loc.c_str(), path.c_str()) == 0;
									}
									if(!ok)
										continue;
								}
							}
							else if(!path.empty())
								continue;

							bool where_defined = si->where.get() != nullptr;
							if(where_defined)
							{
								auto w = eval_tri(si->where);
								if(w == Tri::No)
									continue;
								if(w == Tri::Maybe)
									m = Tri::Maybe;
							}

							if(si->find)
							{
								if(ri.find_dynamic)
									m = Tri::Maybe;
								else if(!ri.find.empty())
								{
									string pattern = string(ri.find.c_str()).trim().tolower().move();
									FindPattern fp;
									if(!fp.split(pattern, L'|') && !where_defined)
										continue;
									if(!fp.find(&item.name))
										continue;
								}
								else if(!where_defined)
									continue;
							}
							else if(!where_defined)
								continue;

							// Kept in the main menu by menu='/': a later rule that moves
							// items is left out for it (ContextMenu.cpp).
							if(item.pinned && si->moveto)
								continue;

							if(m == Tri::Yes)
							{
								item.rules.push_back(static_cast<int>(i));
								if(ri.vis == L"hidden")
									item.hidden = true;
								// The menu it names, as the menu compares it: no '/' around.
								std::wstring target = ri.menu;
								while(!target.empty() && (target.front() == L'/' || iswspace(target.front())))
									target.erase(0, 1);
								while(!target.empty() && (target.back() == L'/' || iswspace(target.back())))
									target.pop_back();
								const bool to_root = !ri.menu_dynamic && target.empty() && ri.menu.find(L'/') != std::wstring::npos;
								if(to_root)
								{
									// "/" in "moved": the main menu, also when it already is there.
									item.pinned = true;
									if(item.moved.empty())
										item.moved = L"/";
									path.clear();
								}
								else if(!target.empty() && item.moved.empty())
									item.moved = ri.menu;
								if(!ri.title.empty() && item.renamed.empty())
									item.renamed = ri.title;
								// Hidden or moved: the item leaves this menu, so the rules
								// after this one don't see it here (as in ContextMenu.cpp).
								if(ri.vis == L"hidden" ||
								   (!target.empty() && ::_wcsicmp(target.c_str(), path.c_str()) != 0))
									break;
							}
							else
							{
								item.maybe.push_back(static_cast<int>(i));
								if(ri.vis == L"hidden")
									item.maybe_hidden = true;
							}
						}
						context._this = nullptr;
					}
					match(item.items, self_uid);
				}
			};
			match(sys, 0);

			// --- Menus of yours with nothing in them ---
			// ContextMenu::OnInitMenuPopup drops a menu of the config that ends up
			// empty, unless Windows' items are moved into it. Yes when a rule surely
			// moves one there, Maybe when one may.
			// A menu path as the menu compares it: the whole text, '/' around it left
			// out ("Pin/Unpin" is one title).
			auto path_hash = [](std::wstring path)
			{
				while(!path.empty() && (path.front() == L'/' || iswspace(path.front())))
					path.erase(0, 1);
				while(!path.empty() && (path.back() == L'/' || iswspace(path.back())))
					path.pop_back();
				return path.empty() ? 0u : MenuItemInfo::hashing(path.c_str());
			};
			auto moved_into = [&](uint32_t path) -> Tri
			{
				Tri found = Tri::No;
				std::function<void(const std::vector<SysItem> &)> look = [&](const std::vector<SysItem> &items)
				{
					for(auto &item : items)
					{
						if(item.type != 2 && !item.hidden)
						{
							if(!item.moved.empty() && path_hash(item.moved) == path)
								found = Tri::Yes;
							else if(found == Tri::No)
							{
								for(auto i : item.maybe)
								{
									if(i >= 0 && static_cast<size_t>(i) < infos.size() &&
									   (infos[i].menu_dynamic || (!infos[i].menu.empty() && path_hash(infos[i].menu) == path)))
										found = Tri::Maybe;
								}
							}
						}
						look(item.items);
					}
				};
				look(sys);
				return found;
			};
			std::vector<std::wstring> menu_path;
			std::function<void(ShellList &)> empties = [&](ShellList &list)
			{
				for(auto &n : list)
				{
					if(!n->item->is_menu())
						continue;
					const bool named = n->title.state == TitleInfo::Text;
					if(named)
						menu_path.push_back(n->title.text.empty() ? std::wstring() : std::wstring(n->title.text.c_str()));
					empties(n->children);
					if(n->shows != Tri::No && !n->item->for_each && n->title.state != TitleInfo::Dynamic)
					{
						Tri kids = Tri::No;
						bool kids_sample = false;
						for(auto &c : n->children)
						{
							if(c->item->is_separator())
								continue;
							kids_sample |= c->shows_sample;
							if(c->shows == Tri::Yes)
								kids = Tri::Yes;
							else if(c->shows == Tri::Maybe && kids == Tri::No)
								kids = Tri::Maybe;
						}
						if(kids != Tri::Yes)
						{
							std::wstring joined;
							for(auto &part : menu_path)
								joined += (joined.empty() ? L"" : L"/") + part;
							auto m = named ? moved_into(path_hash(joined)) : Tri::Maybe;
							if(m == Tri::Yes)
								kids = Tri::Yes;
							else if(m == Tri::Maybe)
								kids = Tri::Maybe;
						}
						if(kids == Tri::No)
						{
							n->shows = Tri::No;
							n->shows_sample = kids_sample;
							n->why = L"empty";
						}
						else if(kids == Tri::Maybe && n->shows == Tri::Yes)
						{
							n->shows = Tri::Maybe;
							n->why = L"empty";
						}
					}
					if(named)
						menu_path.pop_back();
				}
			};
			empties(shell_root);
			walk(shell_root);

			// --- The main menu in the order it shows ---
			// As ContextMenu::OnInitMenuPopup lays it out: your pos=top items, then
			// Windows' items, your items without pos= at the separator nearest the
			// middle (settings.position, the default), pos=bottom at the end,
			// pos=middle in the middle, then pos=N and pos=indexof() over the whole
			// menu. Windows' items that a rule hides or sends to another menu are left
			// out of the placing, then listed again right after the Windows item
			// before them, where the map shows them. modify(pos=) on Windows' items
			// isn't applied: they keep Windows' order.
			std::wstring order;
			{
				struct Slot
				{
					ShellNode *node = nullptr;
					size_t sys = 0;			// index in `sys` when node is null
					bool separator = false;
					const string *name = nullptr;
				};
				std::vector<Slot> items, top, autos, middle, bottom, custom;
				auto slot_of = [](ShellNode *n)
				{
					Slot s;
					s.node = n;
					s.separator = n->item->is_separator();
					s.name = &n->normalized;
					return s;
				};
				for(auto n : shell_config)
				{
					if(n->shows == Tri::No)
						continue;
					switch(n->spec.kind)
					{
						case Placement::Kind::Top: top.push_back(slot_of(n)); break;
						case Placement::Kind::Middle: middle.push_back(slot_of(n)); break;
						case Placement::Kind::Bottom: bottom.push_back(slot_of(n)); break;
						case Placement::Kind::Index:
						case Placement::Kind::IndexOf: custom.push_back(slot_of(n)); break;
						default: autos.push_back(slot_of(n)); break;
					}
				}
				auto present = [&](const SysItem &item)
				{
					if(item.hidden)
						return false;
					std::wstring m = item.moved;
					while(!m.empty() && (m.front() == L'/' || iswspace(m.front())))
						m.erase(0, 1);
					return m.empty();
				};
				items = top;
				for(size_t i = 0; i < sys.size(); i++)
				{
					if(present(sys[i]))
					{
						Slot s;
						s.sys = i;
						s.separator = sys[i].type == 2;
						s.name = &sys[i].name;
						items.push_back(s);
					}
				}

				auto findpos = [&]() -> size_t
				{
					const int c = static_cast<int>(items.size());
					int pos = c / 2, p = -1;
					for(int i = pos; i >= 0 && i < c; i--)
					{
						if(items[i].separator)
						{
							p = i;
							break;
						}
					}
					if(p < 0)
					{
						for(int i = pos; i < c; i++)
						{
							if(items[i].separator)
							{
								p = i;
								break;
							}
						}
					}
					return static_cast<size_t>(p >= 0 ? p : pos);
				};
				auto position = Position::Auto;
				if(auto e = context.Cache->dynamic.position.get(); e && is_safe(e))
				{
					try
					{
						position = context.parse_position(e);
					}
					catch(...)
					{
					}
				}
				size_t dynamic_pos = 0;
				switch(position)
				{
					case Position::Auto:
					case Position::None:
						// The taskbar's menu: at the top.
						if(where != L"taskbar")
							dynamic_pos = items.empty() ? 1 : findpos();
						break;
					case Position::Top: dynamic_pos = 0; break;
					case Position::Middle: dynamic_pos = findpos(); break;
					case Position::Bottom: dynamic_pos = items.size(); break;
					default: dynamic_pos = static_cast<size_t>(position); break;
				}
				if(dynamic_pos > items.size())
					dynamic_pos = items.size();
				items.insert(items.begin() + static_cast<std::ptrdiff_t>(dynamic_pos), autos.begin(), autos.end());
				items.insert(items.end(), bottom.begin(), bottom.end());
				items.insert(items.begin() + static_cast<std::ptrdiff_t>(items.size() / 2), middle.begin(), middle.end());

				struct Placed { ShellNode *spec; Slot last; };
				std::vector<Placed> placed;
				auto same_slot = [](const Slot &a, const Slot &b) { return a.node == b.node && (a.node || a.sys == b.sys); };
				for(auto &it : custom)
				{
					auto &sp = it.node->spec;
					if(sp.kind == Placement::Kind::Index)
					{
						if(sp.index < 0 || sp.index > static_cast<int>(items.size()))
							items.push_back(it);
						else
							items.insert(items.begin() + sp.index, it);
						continue;
					}
					Placed *group = nullptr;
					for(auto &pl : placed)
					{
						if(pl.spec->spec.offset == sp.offset && pl.spec->spec.def == sp.def && pl.spec->indexof.equals(it.node->indexof, false))
						{
							group = &pl;
							break;
						}
					}
					std::ptrdiff_t anchor = -1;
					std::ptrdiff_t at = static_cast<std::ptrdiff_t>(items.size());
					if(group)
					{
						for(size_t i = 0; i < items.size(); i++)
						{
							if(same_slot(items[i], group->last))
							{
								at = static_cast<std::ptrdiff_t>(i) + 1;
								break;
							}
						}
					}
					else
					{
						FindPattern fp;
						if(fp.split(it.node->indexof, L'|'))
						{
							for(const auto &pat : fp.matches)
							{
								for(size_t i = 0; i < items.size() && anchor < 0; i++)
								{
									if(items[i].name && fp.find(pat, items[i].name))
										anchor = static_cast<std::ptrdiff_t>(i);
								}
								if(anchor >= 0)
									break;
							}
						}
						const auto size = static_cast<std::ptrdiff_t>(items.size());
						if(anchor >= 0)
							at = std::clamp<std::ptrdiff_t>(anchor + sp.offset, 0, size);
						else if(sp.def == IndexOf::Top)
							at = 0;
						else if(sp.def == IndexOf::Middle)
							at = size / 2;
						else if(sp.def >= 0)
							at = std::min<std::ptrdiff_t>(sp.def, size);
					}
					items.insert(items.begin() + at, it);
					if(group)
						group->last = it;
					else
						placed.push_back({ it.node, it });
				}

				// Windows' items that left this menu: after the Windows item before them.
				for(size_t i = 0; i < sys.size(); i++)
				{
					if(present(sys[i]))
						continue;
					Slot s;
					s.sys = i;
					size_t at = items.size();
					bool found = false;
					for(size_t k = items.size(); k-- > 0 && !found;)
					{
						if(!items[k].node && items[k].sys < i)
						{
							at = k + 1;
							found = true;
						}
					}
					if(!found)
					{
						for(size_t k = 0; k < items.size(); k++)
						{
							if(!items[k].node)
							{
								at = k;
								break;
							}
						}
					}
					items.insert(items.begin() + static_cast<std::ptrdiff_t>(at), s);
				}

				for(auto &it : items)
				{
					order += order.empty() ? L"{" : L",{";
					size_t index = it.sys;
					if(it.node)
					{
						for(size_t k = 0; k < shell_root.size(); k++)
						{
							if(shell_root[k].get() == it.node)
								index = k;
						}
					}
					jtext(order, L"from", it.node ? L"shell" : L"system");
					jnum(order, L"index", static_cast<long long>(index));
					order += L"}";
				}
			}

			std::function<void(const std::vector<SysItem> &)> write = [&](const std::vector<SysItem> &items)
			{
				bool first = true;
				for(auto &item : items)
				{
					system += first ? L"{" : L",{";
					first = false;
					jtext(system, L"kind", item.type == 2 ? L"separator" : item.type == 1 ? L"menu" : L"item");
					jtext(system, L"title", item.title);
					jkey(system, L"name");
					jstr(system, item.name);
					jtext(system, L"path", item.path);
					jnum(system, L"uid", item.uid);
					auto id = item.type == 2 ? nullptr : id_name(item.uid);
					jtext(system, L"id", id ? id : L"");
					jtext(system, L"verb", item.verb);
					jbool(system, L"disabled", item.disabled);
					jbool(system, L"owner_drawn", item.owner_drawn);
					jtext(system, L"source", item.source);
					jtext(system, L"program", item.program);
					jtext(system, L"module", item.module);
					jtext(system, L"clsid", item.handler ? item.handler->clsid : std::wstring());
					jtext(system, L"handler", item.handler ? item.handler->name : std::wstring());
					auto list = [&](const wchar_t *key, const std::vector<int> &v)
					{
						jkey(system, key);
						system += L"[";
						for(size_t k = 0; k < v.size(); k++)
						{
							if(k)
								system += L",";
							system += std::to_wstring(v[k]);
						}
						system += L"]";
					};
					list(L"rules", item.rules);
					list(L"maybe", item.maybe);
					jbool(system, L"hidden", item.hidden);
					jbool(system, L"maybe_hidden", item.maybe_hidden);
					jtext(system, L"moved", item.moved);
					jtext(system, L"renamed", item.renamed);
					jkey(system, L"items");
					system += L"[";
					write(item.items);
					system += L"]}";
				}
			};
			write(sys);

			// The handlers that loaded but added nothing for this sample.
			std::wstring quiet_json;
			for(auto &h : quiet)
			{
				quiet_json += quiet_json.empty() ? L"{" : L",{";
				jtext(quiet_json, L"clsid", h.clsid);
				jtext(quiet_json, L"name", h.name);
				jtext(quiet_json, L"module", h.module);
				jtext(quiet_json, L"program", h.program);
				jbool(quiet_json, L"windows", h.windows);
				quiet_json += L"}";
			}

			json = L"{\"shell\":[" + shell + L"],\"system\":[" + system + L"],\"rules\":[" + rules + L"],\"quiet\":[" + quiet_json + L"]";
			// The main menu as it shows, your items and Windows' together: "from" shell or
			// system, "index" in that list. Your items it leaves out aren't in it.
			json += L",\"order\":[" + order + L"]";
			jtext(json, L"system_error", system_error);
			json += L"}";
			return true;
		}
	}
}

// DllGetClassObject asks this before handing out anything: see g_reading_menus.
bool MenuTreeReadingMenus()
{
	return Nilesoft::Shell::g_reading_menus;
}
