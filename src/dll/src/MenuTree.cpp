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

	Rules are matched the way ContextMenu::build_main_system_menuitems does,
	as far as that can be done without a selection: find= and in= are
	evaluated, where= too when it only uses this.*, id.*, str.*, title.* and
	constants (sel.*, window.* and the like make the match "maybe"), mode= is
	not tested (one item is selected).
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

				if(t.sample.empty())
				{
					error = L"no sample path";
					return E_INVALIDARG;
				}
				if(FAILED(hr = ::SHParseDisplayName(t.sample.c_str(), nullptr, &t.pidl, 0, nullptr)))
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
			void read_handlers(Target &t, std::vector<std::unique_ptr<Handler>> &handlers,
							   std::vector<std::pair<string, const Handler *>> &by_name)
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
							continue;

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
						}
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
						if(module.empty())
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

			// True when evaluating `e` needs nothing but the item (this.*), the
			// names of Windows' items (id.*, title.*), strings and constants.
			bool is_safe(Expression *e)
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
							if(!is_safe(x))
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
							if(!is_safe(x))
								return false;
						}
						return true;
					}
					case ExpressionType::BinaryOperator:
					{
						auto b = static_cast<BinaryExpression *>(e);
						return b && is_safe(b->Left) && is_safe(b->Right);
					}
					case ExpressionType::UnaryOperator:
					{
						auto u = static_cast<UnaryExpression *>(e);
						return u && is_safe(u->Operand);
					}
					case ExpressionType::Ternary:
					{
						auto t = static_cast<TernaryExpression *>(e);
						return t && is_safe(t->Condition) && is_safe(t->True) && is_safe(t->False);
					}
					case ExpressionType::Identifier:
					{
						auto f = static_cast<FuncExpression *>(e);
						if(!f || f->Array)
							return false;
						switch(f->Id[0])
						{
							case IDENT_THIS:
							case IDENT_ID:
							case IDENT_STR:
							case IDENT_TITLE:
							case IDENT_LOC:
							case IDENT_VIS:
							case IDENT_MODE:
							case IDENT_POS:
							case IDENT_SEP:
							case IDENT_TRUE:
							case IDENT_FALSE:
							case IDENT_NULL:
								break;
							default:
								return false;
						}
						for(auto x : f->Arguments)
						{
							if(!is_safe(x))
								return false;
						}
						return !f->Child || is_safe(f->Child);
					}
					default:
						return false;
				}
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
					return s.c_str();
				}
				catch(...)
				{
					dynamic = true;
					return {};
				}
			};

			// --- Shell's own items ---
			std::wstring shell;
			std::function<void(std::vector<NativeMenu *> &, uint32_t)> walk = [&](std::vector<NativeMenu *> &items, uint32_t depth)
			{
				bool first = true;
				for(auto item : items)
				{
					if(!item)
						continue;
					if(!Selections::match_types(item->fso, types, back, count))
						continue;
					shell += first ? L"{" : L",{";
					first = false;
					auto kind = item->type == NativeMenuType::Menu ? TitleInfo::Menu : TitleInfo::Item;
					auto title = DescribeTitle(item, depth, kind);
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
					jkey(shell, L"items");
					shell += L"[";
					walk(item->items, depth + 1);
					shell += L"]}";
				}
			};
			walk(context.Cache->dynamic.items, 0);

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
					read_handlers(t, handlers, by_name);
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
			std::function<Tri(Expression *)> eval_tri = [&](Expression *e) -> Tri
			{
				if(!e)
					return Tri::Yes;
				if(e->Type() == ExpressionType::BinaryOperator)
				{
					auto b = static_cast<BinaryExpression *>(e);
					if(b && (b->Operator == OperatorType::LogicalAnd || b->Operator == OperatorType::LogicalOr))
					{
						auto l = eval_tri(b->Left), r = eval_tri(b->Right);
						if(b->Operator == OperatorType::LogicalAnd)
							return l == Tri::No || r == Tri::No ? Tri::No : l == Tri::Yes && r == Tri::Yes ? Tri::Yes : Tri::Maybe;
						return l == Tri::Yes || r == Tri::Yes ? Tri::Yes : l == Tri::No && r == Tri::No ? Tri::No : Tri::Maybe;
					}
				}
				if(e->Type() == ExpressionType::UnaryOperator)
				{
					auto u = static_cast<UnaryExpression *>(e);
					if(u && (u->Operator == OperatorType::LogicalNot || u->Operator == OperatorType::UnaryNot))
					{
						auto v = eval_tri(u->Operand);
						return v == Tri::Maybe ? v : v == Tri::Yes ? Tri::No : Tri::Yes;
					}
				}
				if(!is_safe(e))
					return Tri::Maybe;
				try
				{
					return context.Eval(e).to_bool() ? Tri::Yes : Tri::No;
				}
				catch(...)
				{
					return Tri::Maybe;
				}
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
										ok = item.path.empty();
									else if(loc == L"*")
										ok = true;
									else
									{
										if(loc.rfind(L"**", 0) == 0)
											loc.erase(0, 1);
										ok = !item.path.empty() && ::_wcsicmp(loc.c_str(), item.path.c_str()) == 0;
									}
									if(!ok)
										continue;
								}
							}
							else if(!item.path.empty())
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

							if(m == Tri::Yes)
							{
								item.rules.push_back(static_cast<int>(i));
								if(ri.vis == L"hidden")
									item.hidden = true;
								if(!ri.menu.empty() && item.moved.empty())
									item.moved = ri.menu;
								if(!ri.title.empty() && item.renamed.empty())
									item.renamed = ri.title;
								// Hidden or moved: the item leaves this menu, so the rules
								// after this one don't see it here (as in ContextMenu.cpp).
								if(ri.vis == L"hidden" ||
								   (!ri.menu.empty() && ::_wcsicmp(ri.menu.c_str(), item.path.c_str()) != 0))
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

			json = L"{\"shell\":[" + shell + L"],\"system\":[" + system + L"],\"rules\":[" + rules + L"]";
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
