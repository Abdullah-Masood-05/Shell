#include <pch.h>
#include "Include/ContextMenu.h"
#include "Include/Warm.h"

// See Include/Warm.h.

namespace Nilesoft
{
	namespace Shell
	{
		namespace Warm
		{
			namespace
			{
				// Time after Explorer loads Shell before the preload starts, and the
				// longest it then waits for an icon overlay request or a menu to load
				// the config (loading it fills the cache other threads share, so the
				// preload doesn't). Unresponsive network drives can hold that up.
				constexpr DWORD PreloadDelay = 2000;
				constexpr DWORD ConfigWait = 5 * 60 * 1000;

				thread_local bool preloading = false;

				// settings.<member> as a bool; false while no config is loaded.
				bool enabled(auto_expr Settings:: *member) noexcept
				{
					try
					{
						auto initializer = Initializer::instance;
						auto cache = initializer ? initializer->cache : nullptr;
						if(!cache || !(cache->settings.*member))
							return false;

						Selections selected;
						Keyboard keyboard;
						Context context;
						context.Selections = &selected;
						context.Application = &initializer->application;
						context.Cache = cache;
						context.Keyboard = &keyboard;
						context.variables.global = &cache->variables.global;
						context.variables.runtime = &cache->variables.runtime;
						context.variables.local = nullptr;
						return context.eval_bool(cache->settings.*member);
					}
					catch(...)
					{
					}
					return false;
				}

				bool contains(const std::vector<GUID> &list, REFCLSID clsid)
				{
					for(auto &id : list)
					{
						if(::IsEqualGUID(id, clsid))
							return true;
					}
					return false;
				}

				bool contains(const std::vector<HMODULE> &list, HMODULE module)
				{
					for(auto m : list)
					{
						if(m == module)
							return true;
					}
					return false;
				}

				// Out-of-process handlers, held on the keep-alive thread. Its own
				// multithreaded apartment owns the proxies, so no window thread is
				// involved and none of them waits for the server to answer.
				namespace keepalive
				{
					struct held_t
					{
						GUID clsid;
						IClassFactory *factory;
					};

					SRWLOCK lock = SRWLOCK_INIT;
					std::vector<GUID> loaded;		// in-process handlers whose DLL is held
					std::vector<HMODULE> modules;	// those DLLs, one reference each
					std::vector<GUID> pending;		// out-of-process handlers to hold or check
					bool release = false;			// settings.keepalive was turned off
					HANDLE wake{};
					std::atomic_bool holding{ false };	// loaded or held has entries

					// Keep-alive thread only.
					std::vector<held_t> held;

					void hold(REFCLSID clsid) noexcept
					{
						for(auto it = held.begin(); it != held.end(); ++it)
						{
							if(!::IsEqualGUID(it->clsid, clsid))
								continue;

							// Still the running server? One that was restarted since
							// (an update of its package) is held again below.
							if(SUCCEEDED(it->factory->LockServer(TRUE)))
							{
								it->factory->LockServer(FALSE);
								return;
							}
							it->factory->Release();
							held.erase(it);
							break;
						}

						IClassFactory *factory{};
						if(FAILED(::CoGetClassObject(clsid, CLSCTX_LOCAL_SERVER, nullptr, IID_PPV_ARGS(&factory))) || !factory)
							return;

						if(SUCCEEDED(factory->LockServer(TRUE)))
						{
							held.push_back({ clsid, factory });
							holding = true;
						}
						else
						{
							factory->Release();
						}
					}

					void release_all() noexcept
					{
						for(auto &h : held)
						{
							h.factory->LockServer(FALSE);
							h.factory->Release();
						}
						held.clear();

						std::vector<HMODULE> dlls;
						::AcquireSRWLockExclusive(&lock);
						dlls.swap(modules);
						loaded.clear();
						holding = false;
						::ReleaseSRWLockExclusive(&lock);

						// COM keeps its own reference while a DLL is in use, so only idle ones unload.
						for(auto module : dlls)
							::FreeLibrary(module);
					}

					DWORD WINAPI proc(void *) noexcept
					{
						::SetThreadDescription(::GetCurrentThread(), L"Nilesoft Shell keepalive");
						if(FAILED(::CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
							return 0;

						for(;;)
						{
							::WaitForSingleObject(wake, INFINITE);

							std::vector<GUID> work;
							bool release_now = false;
							::AcquireSRWLockExclusive(&lock);
							work.swap(pending);
							std::swap(release_now, release);
							::ReleaseSRWLockExclusive(&lock);

							try
							{
								if(release_now)
									release_all();
								for(auto &clsid : work)
									hold(clsid);
							}
							catch(...)
							{
							}
						}
					}

					// Queue a server to hold (or to check that the held one is still
					// running), or with release, let all of them go.
					void post(const GUID *clsid, bool release_them = false) noexcept
					{
						static std::once_flag started;
						try
						{
							std::call_once(started, []
							{
								wake = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
								if(wake)
								{
									if(auto thread = ::CreateThread(nullptr, 0, proc, nullptr, 0, nullptr))
										::CloseHandle(thread);
								}
							});

							if(!wake)
								return;

							::AcquireSRWLockExclusive(&lock);
							if(release_them)
								release = true;
							else if(!contains(pending, *clsid))
								pending.push_back(*clsid);
							::ReleaseSRWLockExclusive(&lock);
							::SetEvent(wake);
						}
						catch(...)
						{
						}
					}
				}

				namespace preload
				{
					void query(IContextMenu *menu, UINT flags) noexcept
					{
						if(auto hmenu = ::CreatePopupMenu())
						{
							menu->QueryContextMenu(hmenu, 0, 1, 0x7FFF, flags);
							::DestroyMenu(hmenu);
						}
					}

					// The menu Windows builds for the item at path.
					void item(const wchar_t *path) noexcept
					{
						PIDLIST_ABSOLUTE pidl{};
						if(FAILED(::SHParseDisplayName(path, nullptr, &pidl, 0, nullptr)) || !pidl)
							return;

						IShellFolder *parent{};
						PCUITEMID_CHILD child{};
						if(SUCCEEDED(::SHBindToParent(pidl, IID_PPV_ARGS(&parent), &child)))
						{
							IContextMenu *menu{};
							if(SUCCEEDED(parent->GetUIObjectOf(nullptr, 1, &child, IID_IContextMenu, nullptr, reinterpret_cast<void **>(&menu))) && menu)
							{
								query(menu, CMF_NORMAL | CMF_ITEMMENU);
								menu->Release();
							}
							parent->Release();
						}
						::CoTaskMemFree(pidl);
					}

					// The menu Windows builds for the empty space of a folder, or of
					// the desktop when path is null.
					void background(const wchar_t *path) noexcept
					{
						IShellFolder *folder{};
						if(!path)
						{
							if(FAILED(::SHGetDesktopFolder(&folder)))
								return;
						}
						else
						{
							PIDLIST_ABSOLUTE pidl{};
							if(FAILED(::SHParseDisplayName(path, nullptr, &pidl, 0, nullptr)) || !pidl)
								return;
							auto hr = ::SHBindToObject(nullptr, pidl, nullptr, IID_PPV_ARGS(&folder));
							::CoTaskMemFree(pidl);
							if(FAILED(hr))
								return;
						}

						if(folder)
						{
							IContextMenu *menu{};
							if(SUCCEEDED(folder->CreateViewObject(nullptr, IID_IContextMenu, reinterpret_cast<void **>(&menu))) && menu)
							{
								query(menu, CMF_NORMAL);
								menu->Release();
							}
							folder->Release();
						}
					}

					// A handler that crashes Explorer during the preload would do so at
					// every start. So a running preload leaves a mark, a value named after
					// its process ID. One whose process is gone never finished, and the
					// preload is skipped while the config is unchanged since.
					constexpr auto MarkKey = L"Software\\Nilesoft\\Shell\\Preload";

					struct mark_t
					{
						uint64_t created;	// the process's creation time
						uint64_t stamp;		// the config it started with
					};

					uint64_t creation_time(HANDLE process) noexcept
					{
						FILETIME created{}, exited{}, kernel{}, user{};
						if(!::GetProcessTimes(process, &created, &exited, &kernel, &user))
							return 0;
						return (static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
					}

					// A mark of an Explorer that still runs: its preload is in progress.
					bool running(DWORD pid, uint64_t created) noexcept
					{
						auto process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
						if(!process)
							return false;
						DWORD code{};
						bool alive = creation_time(process) == created &&
							::GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
						::CloseHandle(process);
						return alive;
					}

					// A preload never finished with this config. Marks left with another
					// config are deleted.
					bool unfinished(uint64_t stamp)
					{
						HKEY key{};
						if(::RegOpenKeyExW(HKEY_CURRENT_USER, MarkKey, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &key) != ERROR_SUCCESS)
							return false;

						bool found = false;
						std::vector<std::wstring> old;
						for(DWORD i = 0;; i++)
						{
							wchar_t name[16]{};
							DWORD length = ARRAYSIZE(name), type{};
							mark_t mark{};
							DWORD size = sizeof(mark);
							auto rc = ::RegEnumValueW(key, i, name, &length, nullptr, &type, reinterpret_cast<BYTE *>(&mark), &size);
							if(rc == ERROR_MORE_DATA)
								continue;	// not a mark
							if(rc != ERROR_SUCCESS)
								break;
							if(type != REG_BINARY || size != sizeof(mark) || running(::wcstoul(name, nullptr, 10), mark.created))
								continue;
							if(mark.stamp == stamp)
								found = true;
							else
								old.emplace_back(name);
						}
						for(auto &name : old)
							::RegDeleteValueW(key, name.c_str());
						::RegCloseKey(key);
						return found;
					}

					void mark_name(wchar_t (&name)[16]) noexcept
					{
						::swprintf_s(name, L"%lu", ::GetCurrentProcessId());
					}

					bool set_mark(uint64_t stamp) noexcept
					{
						wchar_t name[16]{};
						mark_name(name);
						mark_t mark{ creation_time(::GetCurrentProcess()), stamp };

						HKEY key{};
						if(::RegCreateKeyExW(HKEY_CURRENT_USER, MarkKey, 0, nullptr, REG_OPTION_NON_VOLATILE,
											 KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
							return false;
						auto rc = ::RegSetValueExW(key, name, 0, REG_BINARY, reinterpret_cast<const BYTE *>(&mark), sizeof(mark));
						::RegCloseKey(key);
						return rc == ERROR_SUCCESS;
					}

					void clear_mark() noexcept
					{
						wchar_t name[16]{};
						mark_name(name);
						::RegDeleteKeyValueW(HKEY_CURRENT_USER, MarkKey, name);
					}

					DWORD WINAPI proc(void *) noexcept
					{
						preloading = true;
						::SetThreadDescription(::GetCurrentThread(), L"Nilesoft Shell preload");
						::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

						::Sleep(PreloadDelay);
						for(DWORD waited = 0; !Initializer::Status.Loaded && waited < ConfigWait; waited += 250)
							::Sleep(250);

						if(!Initializer::Status.Loaded || !enabled(&Settings::preload))
							return 0;

						try
						{
							auto stamp = Initializer::instance->config_stamp();
							if(unfinished(stamp))
							{
								Logger::Warning(L"Preload: skipped. One in an earlier File Explorer never finished (File Explorer crashed or "
												L"was closed while it ran), and the config is the same as then. It runs again once the "
												L"config changes, for example when preload is turned off and on again.");
								return 0;
							}
							if(!set_mark(stamp))
							{
								Logger::Warning(L"Preload: skipped. Its mark in HKEY_CURRENT_USER\\%s could not be written.", MarkKey);
								return 0;
							}
						}
						catch(...)
						{
							return 0;
						}

						if(SUCCEEDED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)))
						{
							try
							{
								// The loaded config file and its folder: always there, and
								// local, so nothing waits for a network drive.
								string file = Initializer::instance->application.Config;
								string folder = Path::Parent(file).move();

								auto start = ::GetTickCount64();
								item(file);
								item(folder);
								background(folder);
								background(nullptr);
								Logger::Info(L"Preload: Windows' menus for a file, a folder and the backgrounds built in %u ms.",
											 static_cast<uint32_t>(::GetTickCount64() - start));
							}
							catch(...)
							{
							}
							::CoUninitialize();
						}

						clear_mark();
						return 0;
					}
				}
			}

			void Created(REFCLSID clsid, DWORD context, IUnknown *object) noexcept
			{
				try
				{
					if(!object)
						return;

					if(!enabled(&Settings::keepalive))
					{
						if(keepalive::holding)
							keepalive::post(nullptr, true);
						return;
					}

					// A proxy: the handler runs in another process (or apartment, for
					// which hold() gets no class object, so nothing is held).
					IClientSecurity *proxy{};
					if(SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&proxy))) && proxy)
					{
						proxy->Release();
						if(context & CLSCTX_LOCAL_SERVER)
							keepalive::post(&clsid);
						return;
					}

					// In this process: hold a reference on the DLL the handler is in
					// (its vtable lies in it), so it stays loaded between menus.
					::AcquireSRWLockShared(&keepalive::lock);
					bool known = contains(keepalive::loaded, clsid);
					::ReleaseSRWLockShared(&keepalive::lock);
					if(known)
						return;

					HMODULE module{};
					auto vtable = *reinterpret_cast<const wchar_t *const *>(object);
					if(!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, vtable, &module))
						module = nullptr;

					bool twice = false;
					::AcquireSRWLockExclusive(&keepalive::lock);
					if(!contains(keepalive::loaded, clsid))
						keepalive::loaded.push_back(clsid);
					if(module)
					{
						twice = contains(keepalive::modules, module);
						if(!twice)
							keepalive::modules.push_back(module);
					}
					keepalive::holding = true;
					::ReleaseSRWLockExclusive(&keepalive::lock);

					// Held already, for another handler in the same DLL.
					if(twice)
						::FreeLibrary(module);
				}
				catch(...)
				{
				}
			}

			namespace
			{
				// Per thread, since a class factory proxy belongs to its thread's
				// apartment. A plain struct so nothing runs at thread exit, when that
				// apartment is gone (Explorer's menu threads live as long as it does).
				struct factories_t
				{
					struct { GUID clsid; IClassFactory *factory; } entries[16];
					uint32_t count;
				};
				thread_local factories_t factories{};

				void forget_factory(uint32_t i) noexcept
				{
					factories.entries[i].factory->Release();
					factories.entries[i] = factories.entries[--factories.count];
				}
			}

			bool CreateFromFactory(REFCLSID clsid, DWORD context, IUnknown *outer, REFIID riid, void **ppv, HRESULT &hr) noexcept
			{
				try
				{
					if(preloading || !ppv)
						return false;

					if(!enabled(&Settings::keepalive))
					{
						while(factories.count)
							forget_factory(factories.count - 1);
						return false;
					}

					for(uint32_t i = 0; i < factories.count; i++)
					{
						if(!::IsEqualGUID(factories.entries[i].clsid, clsid))
							continue;
						hr = factories.entries[i].factory->CreateInstance(outer, riid, ppv);
						if(SUCCEEDED(hr))
							return true;
						// Its server went away: get the factory of the one that starts.
						forget_factory(i);
						break;
					}

					if(factories.count == ARRAYSIZE(factories.entries))
						return false;

					IClassFactory *factory{};
					if(FAILED(::CoGetClassObject(clsid, context, nullptr, IID_PPV_ARGS(&factory))) || !factory)
						return false;
					hr = factory->CreateInstance(outer, riid, ppv);
					if(FAILED(hr) || !*ppv)
					{
						factory->Release();
						return false;
					}
					factories.entries[factories.count++] = { clsid, factory };
					Created(clsid, context, static_cast<IUnknown *>(*ppv));
					return true;
				}
				catch(...)
				{
				}
				return false;
			}

			void StartPreload() noexcept
			{
				static std::atomic_flag started = ATOMIC_FLAG_INIT;
				if(started.test_and_set())
					return;

				if(auto thread = ::CreateThread(nullptr, 0, preload::proc, nullptr, 0, nullptr))
					::CloseHandle(thread);
			}

			bool Preloading() noexcept
			{
				return preloading;
			}
		}
	}
}
