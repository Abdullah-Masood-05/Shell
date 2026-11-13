#include <pch.h>
#include "Include/ContextMenu.h"
#include "Include/TaskbarOrigin.h"
#include "Library/detours.h"
#include "RegistryConfig.h"
#include "ConfigCheck.h"
#include <UIAutomation.h>
#include <atomic>
using namespace Nilesoft::Shell;
using namespace Diagnostics;

//MS also offers EV Code Certificates, which enable immediate acceptance by SmartScreeen of Windows Defender
//https://blogs.msdn.microsoft.com/ie/2012/08/14/microsoft-smartscreen-extended-validation-ev-code-signing-certificates/

#ifdef _DEBUG
#include <crtdbg.h>
#endif
/*
Windows 10
Version Build
-------------
1507	10240
1511	10586
1607	14393
1703	15063
1709	16299
1803	17134
1809	17763
1903	18362
1909	18363
2004	19041
20H2	19042
21H1	19043
21H2	19044
22H2	19045
*/

//#pragma comment(lib, "mincore.lib")
//#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "UxTheme.lib")
#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "WindowsCodecs.lib")
#pragma comment(lib, "d2d1")
#pragma comment(lib, "dwrite")
#pragma comment(lib, "Winmm.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "Shlwapi.lib")

#if defined(_M_ARM64)
#pragma comment(lib, "plutosvg-arm64.lib")
#pragma comment(lib, "detours-arm64.lib")
#elif defined(_M_ARM)
#pragma comment(lib, "plutosvg-arm.lib")
#pragma comment(lib, "detours-arm.lib")
#elif defined(_M_X64)
#pragma comment(lib, "plutosvg-x64.lib")
#pragma comment(lib, "detours-x64.lib")
#elif defined(_M_IX86)
#pragma comment(lib, "plutosvg-x86.lib")
#pragma comment(lib, "detours-x86.lib")
#endif

//#pragma optimize("ts", on) 

/*
#if defined(_M_X64)
#pragma comment(linker, "/export:DllCanUnloadNow=_DllCanUnloadNow")
#else
#pragma comment(linker, "/export:DllCanUnloadNow=__DllCanUnloadNow@0")
#endif
*/

#ifdef _DEBUG

std::unordered_map< uint32_t, const wchar_t *> msg_map0 = {
	{ WM_DESTROY, L"WM_DESTROY" },
	{ WM_NCDESTROY, L"WM_NCDESTROY" },
	{ WM_CREATE, L"WM_CREATE" },
	{ WM_NCCREATE, L"WM_NCCREATE" },
	{ WM_PAINT, L"WM_PAINT" },
	{ WM_NCPAINT, L"WM_NCPAINT" },
	{ WM_PRINTCLIENT, L"WM_PRINTCLIENT" },
	{ WM_PRINT, L"WM_PRINT" },
	{ WM_NCCALCSIZE, L"WM_NCCALCSIZE" },
	{ WM_ERASEBKGND, L"WM_ERASEBKGND" },
	{ MN_SELECTITEM, L"MN_SELECTITEM" },
	{ WM_KEYDOWN, L"WM_KEYDOWN" },
	{ WM_WINDOWPOSCHANGED, L"WM_WINDOWPOSCHANGED" },
	{ WM_WINDOWPOSCHANGING, L"WM_WINDOWPOSCHANGING" },
	{ WM_NCHITTEST, L"WM_NCHITTEST" },
	{ WM_TIMER, L"WM_TIMER" },
	{ WM_SIZE, L"WM_SIZE" },
	{ WM_MOVE, L"WM_MOVE" },
	{ MN_SIZEWINDOW, L"MN_SIZEWINDOW" },
	{ MN_DBLCLK, L"MN_DBLCLK" },
	{ MN_CLOSEHIERARCHY, L"MN_CLOSEHIERARCHY" },
	{ MN_SETTIMERTOOPENHIERARCHY, L"MN_SETTIMERTOOPENHIERARCHY" },
	{ MN_FINDMENUWINDOWFROMPOINT, L"MN_FINDMENUWINDOWFROMPOINT" },
	{ MN_ENDMENU, L"MN_ENDMENU" },
	{ MN_CANCELMENUS, L"MN_CANCELMENUS" },
	{ MN_BUTTONDOWN, L"MN_BUTTONDOWN" },
	{ MN_BUTTONUP, L"MN_BUTTONUP" },
	{ WM_UAHDESTROYWINDOW, L"WM_UAHDESTROYWINDOW" },
	{ WM_UAHMEASUREMENUITEM, L"WM_UAHMEASUREMENUITEM" },
	{ WM_UAHINITMENUPOPUP, L"WM_UAHINITMENUPOPUP" },
	{ WM_UAHNCPAINTMENUPOPUP, L"WM_UAHNCPAINTMENUPOPUP" },
	{ WM_UAHDRAWMENU, L"WM_UAHDRAWMENU" },
	{ WM_NCUAHDRAWFRAME, L"WM_NCUAHDRAWFRAME" },
	{ WM_UAHDRAWMENUITEM, L"WM_UAHDRAWMENUITEM" },
	{ WM_ENTERMENULOOP, L"WM_ENTERMENULOOP" },
	{ WM_EXITMENULOOP, L"WM_EXITMENULOOP" },
	{ WM_INITMENU, L"WM_INITMENU" },
	{ WM_INITMENUPOPUP, L"WM_INITMENUPOPUP" },
	{ WM_UNINITMENUPOPUP, L"WM_UNINITMENUPOPUP" },
	{ WM_MENUSELECT, L"WM_MENUSELECT" },
	{ WM_NEXTMENU, L"WM_NEXTMENU" },
	{ WM_MENUCHAR, L"WM_MENUCHAR" },
	{ WM_MEASUREITEM, L"WM_MEASUREITEM" },
	{ WM_DRAWITEM, L"WM_DRAWITEM" },
	{ WM_MENUCOMMAND, L"WM_MENUCOMMAND" },
	{ WM_COMMAND, L"WM_COMMAND" },
	{ WM_SYSCOMMAND, L"WM_SYSCOMMAND" },
	{ WM_MENURBUTTONUP, L"WM_MENURBUTTONUP" },
	{ WM_CAPTURECHANGED, L"WM_CAPTURECHANGED" },
	{ WM_ENTERIDLE, L"WM_ENTERIDLE" },
	{ WM_SETCURSOR, L"WM_SETCURSOR" },
	{ WM_SHOWWINDOW , L"WM_SHOWWINDOW" },
	{ WM_PARENTNOTIFY, L"WM_PARENTNOTIFY"},
	{ WM_MOUSEACTIVATE, L"WM_MOUSEACTIVATE"},
	{ WM_CONTEXTMENU, L"WM_CONTEXTMENU"},
	{ WM_NOTIFY, L"WM_NOTIFY"},
	{ WM_SETFOCUS, L"WM_SETFOCUS"},
	{ WM_KILLFOCUS, L"WM_KILLFOCUS"},
	{ WM_GETOBJECT, L"WM_GETOBJECT"},
	{ WM_ACTIVATE, L"WM_ACTIVATE"},
	{ WM_NCACTIVATE, L"WM_NCACTIVATE"},
	{ WM_ACTIVATEAPP, L"WM_ACTIVATEAPP"},
	{ WM_CHANGEUISTATE, L"WM_CHANGEUISTATE"},
	{ WM_IME_SETCONTEXT, L"WM_IME_SETCONTEXT"},
	{ WM_IME_NOTIFY, L"WM_IME_NOTIFY"},
	{ WM_MOUSEMOVE, L"WM_MOUSEMOVE"},
	{WM_DEVICECHANGE, L"WM_DEVICECHANGE"},
	{WM_IME_REQUEST, L"WM_IME_REQUEST"},
	{MN_GETHMENU, L"MN_GETHMENU"},
	{WM_LBUTTONDOWN, L"WM_LBUTTONDOWN"},
	{WM_RBUTTONDOWN, L"WM_RBUTTONDOWN"},
	{WM_LBUTTONUP, L"WM_LBUTTONUP"},
	{WM_RBUTTONUP, L"WM_RBUTTONUP"},
	{WM_MOUSELEAVE,L"WM_MOUSELEAVE"},
	{WM_USER, L"WM_USER"},
	{WM_APP, L"WM_APP"},
	{ 0, nullptr }
};

#endif

#pragma region Entry Point

#define GET_X_LPARAM(lp) ((int)(short)LOWORD(lp))  // windowsx.h
#define GET_Y_LPARAM(lp) ((int)(short)HIWORD(lp))  // windowsx.h

HINSTANCE _hInstance{};
Initializer _initializer;
extern Logger &_log = Logger::Instance();
const Windows::Version *ver = &Windows::Version::Instance();

HANDLE watch_event = nullptr;

WindowsHook _taskbar_mouse;

//std::unordered_map<HMODULE, IATHook> _detours_ci;
IATHook iathook_NtUserTrackPopupMenuEx;
std::vector<IATHook> iathook_TrackPopupMenu; //OverrideFunction
//Detours<decltype(::TrackPopupMenu)> _TrackPopupMenu;
//Detours<decltype(::TrackPopupMenuEx)> _TrackPopupMenuEx;
Detours<decltype(::DllGetClassObject)> _DllGetClassObject;
Detours<decltype(::CoCreateInstance)> _CoCreateInstance;

uint32_t MN_CONTEXTMENU = 0;
std::unordered_map<HWND, Window> _window_taskbar;

LRESULT __stdcall TaskbarSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData);
LRESULT __stdcall TaskbarProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

__inline auto is_registered() -> bool
{
	//auto_regkey r;
	//return ERROR_SUCCESS == auto_regkey::exists_key(HKCR, HKCR_CLSID_ContextMenu);
	return RegistryConfig::IsRegistered();
}

struct
{
	string path;
	bool explorer{};
	bool contextmenuhandler{};
	HMODULE handle{};

	bool init()
	{
		handle = ::GetModuleHandleW(nullptr);
		if(!(explorer) && path.empty())
		{
			path = Path::Module(nullptr);
			explorer = path.ends_with(def_EXPLORER, true);
		}
		return explorer;
	}

	explicit operator bool() const { return explorer; }

} _loader;

static void start_winx_hotkey() noexcept; // winx_hotkey::start, below

struct taskbar_t
{
	inline static IComPtr<IUIAutomation> _IUIAutomation;
	inline static auto wShell_TrayWnd = ::RegisterWindowMessageW(Windows::WC_Shell_TrayWnd);
	inline static auto wShell_SecondaryTrayWnd = ::RegisterWindowMessageW(Windows::WC_Shell_SecondaryTrayWnd);

	static bool hook(HWND hTaskbar, UINT_PTR id)
	{
		Window window = Window::Find(Windows::WC_Composition_DesktopWindowContentBridge, hTaskbar);
		if(window && !window.is_prop(UxSubclass))
		{
			if(!_window_taskbar.contains(window))
			{
				if(window.subclass(TaskbarSubclassProc, id, hTaskbar))
				{
					window.prop(UxSubclass, CONTEXTMENUSUBCLASS);
					_window_taskbar[window] = window;
					return true;
				}
			}
		}
		return false;
	}

	static void try_hook(HWND hTaskbar, UINT_PTR id, int time = 500)
	{
		if(hTaskbar)
		{
			::SetTimer(hTaskbar, id, time, [](HWND hWnd, UINT, UINT_PTR id, DWORD)
			{
				static int atttempt = 0;
				if(hook(hWnd, id))
					atttempt = 50;
				if(atttempt++ >= 50)
					::KillTimer(hWnd, id);
			});
		}
	}

	static void hook_all()
	{
		::EnumWindows([](HWND hWnd, LPARAM)->BOOL {
			auto atom = Window::Atom(hWnd);
			if(atom == wShell_TrayWnd || atom == wShell_SecondaryTrayWnd)
			{
				Window window = hWnd;
				if(!window.is_prop(UxSubclass))
				{
					if(!_window_taskbar.contains(window))
					{
						auto proc = window.get_long(GWLP_WNDPROC);
						window.prop(UxSubclass, proc);
						window.set_long(GWLP_WNDPROC, TaskbarProc);
						_window_taskbar[window] = window;
						return true;
					}
				}
			}
			return TRUE;
		}, 0);
		start_winx_hotkey(); // after subclassing: it posts to TaskbarProc
	}

	static void hook_all(uint32_t delay)
	{
		::SetTimer(::GetShellWindow(), 0, delay, [](HWND hWnd, UINT, UINT_PTR id, DWORD)
		{						
			static int atttempt = 0;
			if(atttempt++ >= 10)
				::KillTimer(hWnd, id);
			
			taskbar_t::hook_all();
		});
	}
	
	static void unhook(HWND hWnd)
	{
		for(auto &window : _window_taskbar)
		{
			if(window.first == hWnd)
			{
				window.second.set_long(GWLP_WNDPROC, window.second.prop(UxSubclass));
				window.second.remove_prop(UxSubclass);
				_window_taskbar.erase(hWnd);
				break;
			}
		}
	}
	static void unhook_all()
	{
		for(auto &window : _window_taskbar)
		{
			window.second.set_long(GWLP_WNDPROC, window.second.prop(UxSubclass));
			window.second.remove_prop(UxSubclass);
			_window_taskbar.erase(window.first);
			break;
		}
		_window_taskbar.clear();
	}

	static bool is_allowed_area(HWND hTaskbar, const Point &pt)
	{
		// One right-click asks twice, in WM_MOUSEACTIVATE and then WM_CONTEXTMENU,
		// and each answer is a UI Automation call into the taskbar. The same
		// taskbar and point within 150 ms reuses the first answer.
		static thread_local struct
		{
			ULONGLONG time = 0;
			HWND taskbar = nullptr;
			Point pt{};
			bool result = false;
		} last;

		const auto now = ::GetTickCount64();
		if(last.time && now - last.time < 150 && last.taskbar == hTaskbar && last.pt.x == pt.x && last.pt.y == pt.y)
			return last.result;

		bool ret = false;

		if(!_IUIAutomation)
			_IUIAutomation.CreateInstance(__uuidof(CUIAutomation), CLSCTX_INPROC_SERVER);

		if(_IUIAutomation)
		{
			IComPtr<IUIAutomationElement> pIUIAutomationElement;
			if(SUCCEEDED(_IUIAutomation->ElementFromPoint(pt, pIUIAutomationElement)) && pIUIAutomationElement)
			{
				struct elem_t {
					BSTR type = nullptr;
					BSTR name = nullptr;
					BSTR id = nullptr;
					~elem_t() {
						freeString(type);
						freeString(name);
						freeString(id);
					}
					void freeString(BSTR s) {
						if(s) DLL::Invoke(L"OleAut32.dll", "SysFreeString", s);
					}
				} elem;
				
				pIUIAutomationElement->get_CurrentAutomationId(&elem.id);
				pIUIAutomationElement->get_CurrentClassName(&elem.type);
				pIUIAutomationElement->get_CurrentName(&elem.name);
				/*
				id=[TaskbarFrame], type=[Taskbar.TaskbarFrameAutomationPeer], name=[]
				id=[StartButton], type=[ToggleButton], name=[Start]
				id=[SearchButton], type=[ToggleButton], name=[Search]
				id=[SystemTrayIcon], type=[SystemTray.NormalButton], name=[Show Hidden Icons]
				id=[SystemTrayIcon], type=[SystemTray.AccentButton], name=[Network wifi-1 Internet access]
				id=[SystemTrayIcon], type=[SystemTray.OmniButton], name=[Clock 7:04 PM 3/20/2023]
				id=[SystemTrayIcon], type=[SystemTray.ShowDesktopButton], name=[Show Desktop]
				*/
				if(elem.name && elem.type && elem.id)
				{
					// UIA identifiers, not user text: compare ordinally, not with
					// the locale-sensitive lstrcmpiW.
					auto equals = [](const wchar_t *a, const wchar_t *b)
					{
						return ::CompareStringOrdinal(a, -1, b, -1, TRUE) == CSTR_EQUAL;
					};

					if(equals(elem.id, L"TaskbarFrame"))
						ret = !*elem.name && equals(elem.type, L"Taskbar.TaskbarFrameAutomationPeer");
					else if(is_secondary(hTaskbar))
					{
						if(equals(elem.id, L"SystemTrayIcon"))
							ret = equals(elem.type, L"SystemTray.OmniButton");
						//_log.info(L"id=[%s],\t[%s], [%s]", elem.id, elem.type, elem.name);
					}
				}
			}
		}

		last.time = now;
		last.taskbar = hTaskbar;
		last.pt = pt;
		last.result = ret;
		return ret;
	}

	static bool is_primary(HWND hTaskbar)
	{
		if(hTaskbar)
		{
			Window taskbar = hTaskbar;
			if(taskbar.hash() == WindowClass::WC_Shell_TrayWnd)
			{
				// Only the primary taskbar has a RebarWindow32
				return taskbar.find(Windows::WC_ReBarWindow32);
			}
		}
		return false;
	}

	static bool is_secondary(HWND hTaskbar)
	{
		return hTaskbar && (Window::class_hash(hTaskbar) == WindowClass::WC_Shell_SecondaryTrayWnd);
	}
};

HRESULT __stdcall CoCreateInstanceHook(REFCLSID rclsid, LPUNKNOWN pUnkOuter, DWORD dwClsContext, REFIID riid, LPVOID *ppv)
{
	auto is_local_server = (dwClsContext & CLSCTX_LOCAL_SERVER) != 0;
	auto is_inproc_server = (dwClsContext & CLSCTX_INPROC_SERVER) != 0;

	if(_initializer.cache && (is_inproc_server || is_local_server))
	{
		auto hr = E_NOINTERFACE; //CLASS_E_CLASSNOTAVAILABLE;
		auto process = true;

		if(rclsid == IID_FileExplorerContextMenu /*&& riid == {706461D1-AC5F-4730-BFE3-CAC6CAD5EF5E}*/)
		{
			if(_initializer.cache->settings.priority)
			{
				Context context;
				Object obj = context.Eval(_initializer.cache->settings.priority).move();
				if(obj.is_number())
					process = obj;
			}

			if(process)
				return hr;
		}
		else
		{
			/*auto xx = Guid(L"{f3d06e7c-1e45-4a26-847e-f9fcdee59be0}");

			if(xx.equals({riid,rclsid}))
			{
				MB(L"0000000000000");
			}*/

			//if(riid== IID_IExplorerCommandProvider)
			//	_log.info(L"IID_IExplorerCommandProvider %s", Guid(rclsid).to_string(2).c_str());

			//if(riid == IID_IExecuteCommand)
			//	_log.info(L"IID_IExecuteCommand %s", Guid(rclsid).to_string(2).c_str());
			/*
			if(riid == IID_IExplorerCommandState)
			{
				hr = _CoCreateInstance.invoke(rclsid, pUnkOuter, dwClsContext, riid, ppv);

				auto cc = (IExplorerCommand *)*ppv;

				IExplorerCommand *f = 0;
				cc->QueryInterface(IID_IExplorerCommand, (void**)&f);
				//printto;rotate90;rotate270;setwallpaper;slideshow

				LPWSTR p;
				f->GetTitle(0, &p);
				_log.info(L"IExplorerCommandState %s", p);
				return hr;
			}
		*/
			auto is_UWP = false;
			process = false;
			/*
			if(Guid(rclsid).equals(L"{E6950302-61F0-4FEB-97DB-855E30D4A991}"))
			{
				*ppv = new ExplorerCommandBase;
				return S_OK;
			}
			*/
			if(is_inproc_server)
				process = riid == IID_IContextMenu || riid == IID_IContextMenu2 || riid == IID_IContextMenu3 || riid == IID_IExplorerCommand;
			else
			{
				process = riid == IID_IExplorerCommand;
				is_UWP = is_local_server && process;
			}

			if(process)
			{
				Context context;
				this_item _this; context._this = &_this;
				_this.is_uwp = is_UWP;
				_this.clsid = Guid(rclsid).to_string(2);

				bool test = Keyboard::IsKeyDown(VK_MENU);
				if(test)
				{
					Timer t;
					t.start();
					hr = _CoCreateInstance.invoke(rclsid, pUnkOuter, dwClsContext, riid, ppv);
					t.stop();
					auto elapsed = (int)t.elapsed_milliseconds();
					_log.write(L"%d%s\t%s\t%s\r\n", elapsed, elapsed > 0 ? L"ms" : L"" , _this.clsid.c_str(), is_UWP ? L"UWP":L"");
					return hr;
				}

				for(auto si : _initializer.cache->statics)
				{
					if(si->clsid.empty() || (si->where && !context.eval_bool(si->where)))
						continue;

					if(si->has_clsid)
					{
						for(auto &id : si->clsid)
						{
							if(id.equals(rclsid))
							{
								if(test && *ppv)
								{
									((IUnknown *)*ppv)->Release();
									*ppv = nullptr;
								}
								return E_NOINTERFACE;
							}
						}
					}
				}

				if(test)
					return hr;
			}
		}
	}

	return _CoCreateInstance.invoke(rclsid, pUnkOuter, dwClsContext, riid, ppv);
}

STDAPI WINAPI DllGetClassObjectHook(REFCLSID rclsid, _In_ REFIID riid, LPVOID *ppv)
{
	auto hr = CLASS_E_CLASSNOTAVAILABLE;
	if(IID_FileExplorerContextMenu == rclsid/* || rclsid == IID_FileExplorerCommandBar*/)
		return hr;
	hr = _DllGetClassObject.invoke(rclsid, riid, ppv);
	return hr;
}

#define TPM_SYSMENU	0x0200L
// Whether a popup came from the taskbar is per thread now; see TaskbarOrigin.h.

BOOL WINAPI TrackPopupMenuProc(HMENU hMenu, uint32_t uFlags, int x, int y, int nReserved, HWND hWnd, const RECT *prcRect);
BOOL WINAPI TrackPopupMenuExProc(HMENU hMenu, uint32_t uFlags, int x, int y, HWND hWnd, LPTPMPARAMS lptpm);

// Out of line: NtUserTrackPopupMenu uses __try and cannot hold objects that
// need unwinding.
static __declspec(noinline) void log_taskbar_not_built(HWND hWnd) noexcept
{
	try
	{
		static std::atomic_bool logged{ false };
		if(!logged.exchange(true))
			_log.info(L"Taskbar menu: not built for window '%s' (no context for it); the stock menu is shown.",
					  Window::class_name(hWnd).c_str());
	}
	catch(...)
	{
	}
}

BOOL WINAPI NtUserTrackPopupMenu(HMENU hMenu, uint32_t uFlags, int x, int y, HWND hWnd, LPTPMPARAMS lptpm, int tfunc)
{
#ifdef _DIJA
	return (BOOL)XXX::OnContextMenu(hWnd, x, y, hMenu);;
#endif

	auto result = FALSE;
	auto invoked = false;

	auto invoke = [&](HMENU hmenu, uint32_t flag, Point pt)
	{
		if(!invoked)
		{
			if(tfunc == 0)
				result = ::TrackPopupMenu(hmenu, flag, pt.x, pt.y, 0, hWnd, nullptr);
			else if(iathook_NtUserTrackPopupMenuEx.installed())
				result = iathook_NtUserTrackPopupMenuEx.invoke<decltype(TrackPopupMenuExProc)>(hmenu, flag, pt.x, pt.y, hWnd, lptpm);
			else
				result = ::TrackPopupMenuEx(hmenu, flag, pt.x, pt.y, hWnd, lptpm);
			invoked = true;
		}
	};

	__try
	{
		__try
		{

			if(!is_registered())
			{
				__trace(L"TrackPopupMenu unregistered");
				__leave;
			}

			//cs.lock();
			auto has_inited = 0; Initializer::OnState(Nilesoft::Shell::in_taskbar());

			if(!_initializer.Status.Disabled)
			{
				// is injected from explorer
				if(!_initializer.Status.Loaded)
				{
					// A broken config falls back to the stock menu, but is retried
					// once it has been saved again (has_error(true) sees the change).
					if(_initializer.has_error(true) || has_inited)
						__leave; //goto skip;
					_initializer.init();
				}

				auto ctx = ContextMenu::CreateAndInitialize(hWnd, hMenu, { x, y }, _loader.explorer, _loader.contextmenuhandler);

				// A taskbar popup Shell declined to build falls through to the
				// menu it was given, which for the Windows 11 taskbar is empty.
				// Said once per session; see also "Taskbar menu:" in ContextMenu.
				if(ctx == nullptr && Nilesoft::Shell::in_taskbar())
					log_taskbar_not_built(hWnd);

				if(ctx != nullptr)
				{

					Flag<uint32_t> flag(uFlags);
					//Selections::point = { x, y };
					//	flag.remove(TPM_RETURNCMD);
					flag.remove(TPM_NONOTIFY);
					//	flag.remove(TPM_NOANIMATION);

				//	flag.add(TPM_HORPOSANIMATION);

				//	BOOL v = 1;
				//	SystemParametersInfoW(SPI_SETMENUANIMATION, 0, &v, SPIF_SENDCHANGE);
				//	v = 0;
				//	SystemParametersInfoW(SPI_GETMENUFADE, 0, &v, SPIF_SENDCHANGE);
					
					if(ctx->is_layoutRTL == 1)
						flag.add(TPM_LAYOUTRTL);
					else if(ctx->is_layoutRTL == 0)
						flag.remove(TPM_LAYOUTRTL);

					auto z = ctx->_theme.border.size + ctx->_theme.border.size;

					if(ctx->Selected.Window.parent == WINDOW_TASKBAR)
					{
						HWND hTaskbar = hWnd;
						if(ctx->Selected.Window.id == WINDOW_START)
						{
							// Windows 11 owns the Win+X menu by the taskbar window
							// itself; Windows 10 by LauncherTipWnd, so look there.
							auto cls = Window::class_hash(hWnd);
							if(cls != WindowClass::WC_Shell_TrayWnd && cls != WindowClass::WC_Shell_SecondaryTrayWnd)
								hTaskbar = ::WindowFromPoint({ x, y });
						}
						else
							hTaskbar = ::GetAncestor(hWnd, GA_ROOTOWNER);

						if(!hTaskbar)
							hTaskbar = hWnd;

						Monitor monitor(hTaskbar);
						monitor.info();

						Rect rcW = monitor.rcWork;
						Rect rcM = monitor.rcMonitor;
						Rect rc = hTaskbar;

						if(ctx->Selected.Window.id == WINDOW_START)
						{
							if(rc.top > 0)
								y = (rc.top + 5) - (z + ctx->_theme.border.padding.height());
							else if(rc.left == 0)
								y = rc.bottom;
							else
								x = rc.left;
						}
						else
						{
							if(rcM.top == rc.top && rcM.bottom == rc.bottom) // Vertical
							{
								if(rcM.left == rc.left)
									x = rc.right;
								else if(rcM.right == rc.right)
									x = rc.left - 20;
							}
							else if(rcM.left == rc.left && rcM.right == rc.right) // Horizontal
							{
								if(rcM.top == rc.top)
									y = rc.bottom;
								else if(rcM.bottom == rc.bottom)
									y = (rc.top + 5) - (z + ctx->_theme.border.padding.height());
							}
						}
					}
					else
					{
						y -= z;
					}
					
					invoke(ctx->MenuHandle(), flag, { x, y });
				
					// v = 0;
					// SystemParametersInfoW(SPI_SETMENUANIMATION, 0, &v, SPIF_SENDCHANGE);
					result = ctx->InvokeCommand(result);
					__leave;
				}
			}
		}
		except
		{
#ifdef _DEBUG
			_log.exception(__func__);
#endif
		}
	}
	__finally
	{
		invoke(hMenu, uFlags, { x, y });
		_loader.contextmenuhandler = false;
		// The taskbar flag used to be cleared here, which was the only place it
		// was cleared and not a place every taskbar menu passes through.
		// ScopedTaskbarOrigin in ShowTaskbarContextMenu owns it now.
		return result;
	}
}

BOOL WINAPI TrackPopupMenuProc(HMENU hMenu, uint32_t uFlags, int x, int y, [[maybe_unused]] int nReserved, HWND hWnd, [[maybe_unused]] const RECT *prcRect)
{
	//_log.write(L"%s\n", Window::class_name(hWnd).c_str());
	return NtUserTrackPopupMenu(hMenu, uFlags, x, y, hWnd, nullptr, 0);
}

//win32u.dll win10 1607	Redstone 1 14393
BOOL WINAPI TrackPopupMenuExProc(HMENU hMenu, uint32_t uFlags, int x, int y, HWND hWnd, LPTPMPARAMS lptpm)
{
	//_log.write(L"%s\n", Window::class_name(hWnd).c_str());
	return NtUserTrackPopupMenu(hMenu, uFlags, x, y, hWnd, lptpm, 1);
}

bool ShowTaskbarContextMenu(HWND hTaskbar, const Point &pt, uint32_t uMsg)
{
	auto ret = false;
	//if(taskbar_t::is_allowed_area(hTaskbar, pt))
	{
		struct menu {
			HMENU handle{};
			menu() { handle = ::CreatePopupMenu(); }
			~menu() { if(handle) ::DestroyMenu(handle); }
		}_menu;

		if(_menu.handle)
		{
			// Restored on the way out of this block, so the branch below that
			// invokes the saved native target directly - and therefore never
			// reaches the hook's __finally - cannot leave it set.
			Nilesoft::Shell::ScopedTaskbarOrigin taskbar_origin(true);

			//::SetForegroundWindow(hTaskbar);
			auto uFlags = TPM_RETURNCMD | TPM_RIGHTBUTTON | (::GetSystemMetrics(SM_MIDEASTENABLED) ? TPM_LAYOUTRTL : 0);
			if(uMsg == WM_CONTEXTMENU)
			{
				//_log.info(L"************");
				TrackPopupMenuExProc(_menu.handle, uFlags, pt.x, pt.y, hTaskbar, nullptr);
			}
			else 
			{
				iathook_NtUserTrackPopupMenuEx.invoke<decltype(TrackPopupMenuExProc)>(_menu.handle, uFlags, 0, 0, hTaskbar, nullptr);
			}
			::PostMessageW(hTaskbar, WM_NULL, 0, 0); // send benign message to window to make sure the menu goes away.
			ret = true;
		}
	}
	return ret;
}

/*
	settings.winx.hotkey (experimental): Win+X and a right-click on Start open
	Shell's Win+X menu (the type='winx' items) instead of Windows' own.

	On Windows 11 24H2 and later Windows' Win+X menu is a XAML popup, not a
	Win32 menu, so TrackPopupMenu never sees it and Shell cannot fill it the
	way it fills other menus (see WinX.h for the builds where it can). So the
	key and the click are taken before Windows acts on them, and Shell tracks
	an empty popup on the taskbar, as for its own Windows 11 taskbar menu,
	marked as the Win+X menu (ScopedWinXOrigin); NtUserTrackPopupMenu fills it.

	Whether to take them is decided ahead of time, on the taskbar's thread
	(a timer, every refresh_ms): settings.winx.hotkey is on and Shell would
	build the Win+X menu (exclude, visibility: TaskbarDeclined, asked as the
	Win+X menu). The answer is kept in `wanted`. A key or click Shell would not
	answer is never taken, so Windows' menu still opens.

	Win+X: a low-level keyboard hook on a thread of its own, installed only
	while `wanted` is true. Windows removes, without a word, a low-level hook
	that takes longer than LowLevelHooksTimeout (300 ms by default), so the
	hook only reads `wanted` and posts; it never waits on another thread. It
	eats the X (down, repeats and up) and posts WM_SHOW to the taskbar.
	Windows opens Start when the Windows key goes up with no other key since it
	went down; the eaten X does not count, so a Ctrl press is sent while the
	Windows key is still down (AutoHotkey's default menu mask key).

	The Start button is found with UI Automation. Microsoft's UIA threading
	rules say not to search a window's own UI from its UI thread, so the
	search (FindFirst) runs on a short-lived MTA thread that posts the result
	back. A right-click only asks what is under the cursor (ElementFromPoint),
	as the taskbar menu check already does.

	Right-click on Start: the taskbar's WM_MOUSEACTIVATE (TaskbarSubclassProc)
	asks on_start() first; the button-up is eaten and opens the menu.

	Threads started here hold a reference to shell.dll. The hook thread never
	gives its reference back: explorer.exe keeps shell.dll for its lifetime
	anyway (the taskbar is subclassed), and letting the last reference go on
	that thread would run DLL_PROCESS_DETACH there, away from the taskbar.
*/
namespace winx_hotkey
{
	static const UINT WM_SHOW = ::RegisterWindowMessageW(L"NilesoftShellPro.WinX.Show");
	static const UINT WM_INIT = ::RegisterWindowMessageW(L"NilesoftShellPro.WinX.Init");

	constexpr UINT_PTR timer_id = 0x4E57; // on Shell_TrayWnd
	constexpr UINT refresh_ms = 2000;

	// WM_SHOW's wParam
	enum : WPARAM { show_find = 0, show_at = 1, show_anywhere = 2 };

	static std::atomic_bool wanted{ false };
	static std::atomic_bool thread_started{ false };
	static std::atomic<HWND> primary{};		// the Shell_TrayWnd the timer runs on
	static thread_local bool showing = false;	// taskbar thread: our menu is open
	static thread_local bool finding = false;	// taskbar thread: waiting for find_start_proc

	constexpr UINT_PTR find_timer_id = 0x4E58;	// on Shell_TrayWnd
	constexpr UINT find_timeout_ms = 400;

	// On the taskbar's thread: settings.winx.hotkey is on.
	static bool hotkey_on() noexcept
	{
		try
		{
			if(!is_registered() || _initializer.Status.Disabled)
				return false;

			auto initializer = Initializer::instance;
			if(!initializer || !initializer->query() || !initializer->cache)
				return false;

			auto cache = initializer->cache;
			if(!cache->settings.winx.hotkey)
				return false;

			Selections selected;
			Keyboard keyboard;
			Context context;
			context.Selections = &selected;
			context.Application = &_initializer.application;
			context.Cache = cache;
			context.Keyboard = &keyboard;
			context.variables.global = &cache->variables.global;
			context.variables.runtime = &cache->variables.runtime;
			context.variables.local = nullptr;
			return context.eval_bool(cache->settings.winx.hotkey);
		}
		catch(...)
		{
		}
		return false;
	}

	// On the taskbar's thread: the hotkey is on and Shell would build the
	// Win+X menu on this taskbar.
	static bool would_show(HWND hTaskbar) noexcept
	{
		if(!hotkey_on())
			return false;
		Nilesoft::Shell::ScopedTaskbarOrigin taskbar_origin(true);
		Nilesoft::Shell::ScopedWinXOrigin winx_origin(true);
		return !ContextMenu::TaskbarDeclined(hTaskbar, _loader.explorer);
	}

	// Foreground for the menu's owner: a menu only closes on a click elsewhere
	// while its owner is in front. The eaten key may not count as input to
	// explorer, so if the plain call is refused, borrow the input state of the
	// thread that is in front.
	static void bring_to_front(HWND hWnd) noexcept
	{
		if(::SetForegroundWindow(hWnd))
			return;

		auto fg = ::GetForegroundWindow();
		auto fg_thread = fg ? ::GetWindowThreadProcessId(fg, nullptr) : 0;
		auto me = ::GetCurrentThreadId();
		if(fg_thread && fg_thread != me && ::AttachThreadInput(me, fg_thread, TRUE))
		{
			::SetForegroundWindow(hWnd);
			::AttachThreadInput(me, fg_thread, FALSE);
		}
	}

	// On the taskbar's thread: Shell's Win+X menu, its left edge at x and,
	// as for every Win+X menu, just above or beside the taskbar (NtUserTrackPopupMenu).
	static void show(HWND hTaskbar, Point pt) noexcept
	{
		struct menu {
			HMENU handle{};
			menu() { handle = ::CreatePopupMenu(); }
			~menu() { if(handle) ::DestroyMenu(handle); }
		} _menu;

		if(!hTaskbar || !_menu.handle || showing)
			return;

		// Wanted a moment ago, but not now (the config changed): nothing to show,
		// and the key is already gone; better nothing than an empty popup.
		if(!would_show(hTaskbar))
			return;

		showing = true;
		{
			Nilesoft::Shell::ScopedTaskbarOrigin taskbar_origin(true);
			Nilesoft::Shell::ScopedWinXOrigin winx_origin(true);

			bring_to_front(hTaskbar);

			auto uFlags = TPM_RETURNCMD | TPM_RIGHTBUTTON | (::GetSystemMetrics(SM_MIDEASTENABLED) ? TPM_LAYOUTRTL : 0);
			TrackPopupMenuExProc(_menu.handle, uFlags, pt.x, pt.y, hTaskbar, nullptr);
			::PostMessageW(hTaskbar, WM_NULL, 0, 0);
		}
		showing = false;
	}

	// A short-lived thread: finds the Start button of a taskbar with UI
	// Automation and posts WM_SHOW back with its position.
	static DWORD WINAPI find_start_proc(LPVOID param)
	{
		auto hTaskbar = static_cast<HWND>(param);
		WPARAM how = show_anywhere;
		LPARAM where = 0;

		if(SUCCEEDED(::CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
		{
			try
			{
				IComPtr<IUIAutomation> uia;
				uia.CreateInstance(__uuidof(CUIAutomation), CLSCTX_INPROC_SERVER);

				IComPtr<IUIAutomationElement> root;
				if(uia && SUCCEEDED(uia->ElementFromHandle(hTaskbar, root)) && root)
				{
					// A BSTR made by hand: its length in bytes sits just before the text.
					static const struct { uint32_t bytes; wchar_t text[12]; } id = { 11 * sizeof(wchar_t), L"StartButton" };
					VARIANT value{};
					value.vt = VT_BSTR;
					value.bstrVal = const_cast<BSTR>(id.text);

					IComPtr<IUIAutomationCondition> condition;
					IComPtr<IUIAutomationElement> button;
					RECT rc{};
					if(SUCCEEDED(uia->CreatePropertyCondition(UIA_AutomationIdPropertyId, value, condition)) && condition &&
					   SUCCEEDED(root->FindFirst(TreeScope_Descendants, condition, button)) && button &&
					   SUCCEEDED(button->get_CurrentBoundingRectangle(&rc)) && rc.right > rc.left)
					{
						how = show_at;
						where = MAKELPARAM(static_cast<short>(rc.left), static_cast<short>(rc.top));
					}
				}
			}
			catch(...)
			{
			}
			::CoUninitialize();
		}

		::PostMessageW(hTaskbar, WM_SHOW, how, where);

		HMODULE self{};
		::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
							 reinterpret_cast<LPCWSTR>(&find_start_proc), &self);
		::FreeLibraryAndExitThread(self, 0);
	}

	static void find_start(HWND hTaskbar) noexcept
	{
		if(finding)
			return;

		// The key is already eaten: if the search is slow or stuck, open the
		// menu at the cursor rather than not at all. A late answer is dropped.
		finding = true;
		::SetTimer(hTaskbar, find_timer_id, find_timeout_ms, [](HWND hWnd, UINT, UINT_PTR id, DWORD)
		{
			::KillTimer(hWnd, id);
			if(finding)
			{
				finding = false;
				show(hWnd, Point::CursorPos());
			}
		});

		HMODULE self{};
		if(::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&find_start_proc), &self))
		{
			if(auto thread = ::CreateThread(nullptr, 0, find_start_proc, hTaskbar, 0, nullptr))
			{
				::CloseHandle(thread);
				return;
			}
			::FreeLibrary(self);
		}
		::KillTimer(hTaskbar, find_timer_id);
		finding = false;
		show(hTaskbar, Point::CursorPos());
	}

	/* The keyboard hook (hook thread only) */

	static HHOOK hook{};
	static bool x_eaten = false;
	static DWORD x_time = 0;	// the last X-down eaten

	// Modifiers held before this key (in a low-level hook the async state
	// does not include the key being reported, which is not a modifier here).
	// Not tracked from the hook's own events: a key-up on the secure desktop
	// (Win+L) is never seen there.
	static bool held(int vk) noexcept
	{
		return (::GetAsyncKeyState(vk) & 0x8000) != 0;
	}

	static LRESULT CALLBACK keyboard(int code, WPARAM wParam, LPARAM lParam)
	{
		if(code == HC_ACTION && lParam)
		{
			auto key = reinterpret_cast<const KBDLLHOOKSTRUCT *>(lParam);
			if(key->vkCode == 'X' && !(key->flags & LLKHF_INJECTED))
			{
				const bool down = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
				if(!down)
				{
					if(x_eaten)
					{
						x_eaten = false;
						return 1;
					}
				}
				// Held down: auto-repeat, also after the Windows key went up
				// first. A press long after the last one is a new one whose
				// key-up was never seen.
				else if(x_eaten && key->time - x_time < 1000)
				{
					x_time = key->time;
					return 1;
				}
				else
				{
					x_eaten = false;
					if((held(VK_LWIN) || held(VK_RWIN)) && !held(VK_CONTROL) && !held(VK_SHIFT) &&
					   !(key->flags & LLKHF_ALTDOWN) && wanted.load())
					{
						auto hTaskbar = primary.load();
						if(hTaskbar && ::PostMessageW(hTaskbar, WM_SHOW, show_find, 0))
						{
							x_eaten = true;
							x_time = key->time;

							// Ctrl down and up while the Windows key is still down, so
							// that letting it go does not open Start.
							INPUT mask[2]{};
							mask[0].type = INPUT_KEYBOARD;
							mask[0].ki.wVk = VK_CONTROL;
							mask[0].ki.wScan = 0x1D;
							mask[1] = mask[0];
							mask[1].ki.dwFlags = KEYEVENTF_KEYUP;
							::SendInput(2, mask, sizeof(INPUT));
							return 1;
						}
					}
				}
			}
		}
		return ::CallNextHookEx(hook, code, wParam, lParam);
	}

	// Installs the hook while `wanted`, removes it when not, and puts it back
	// now and then, as Windows may have removed it (LowLevelHooksTimeout).
	static DWORD WINAPI hook_thread_proc(LPVOID)
	{
		MSG msg{};
		::PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE); // a message queue

		auto sync = []()
		{
			static ULONGLONG installed_at = 0;
			const auto now = ::GetTickCount64();
			const bool want = wanted.load() && is_registered();
			if(hook && (!want || now - installed_at > 60000))
			{
				::UnhookWindowsHookEx(hook);
				hook = nullptr;
				x_eaten = false;
			}
			if(want && !hook)
			{
				hook = ::SetWindowsHookExW(WH_KEYBOARD_LL, keyboard, _hInstance, 0);
				installed_at = now;
			}
		};

		sync();
		::SetTimer(nullptr, 0, refresh_ms, nullptr);
		while(::GetMessageW(&msg, nullptr, 0, 0) > 0)
		{
			if(msg.message == WM_TIMER)
				sync();
			::DispatchMessageW(&msg);
		}
		return 0; // never: the thread lives as long as explorer
	}

	static void start_hook_thread() noexcept
	{
		if(thread_started.exchange(true))
			return;

		// Kept for good: see the note at the top.
		HMODULE self{};
		if(!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&hook_thread_proc), &self))
		{
			thread_started = false;
			return;
		}

		if(auto thread = ::CreateThread(nullptr, 0, hook_thread_proc, nullptr, 0, nullptr))
			::CloseHandle(thread);
		else
		{
			::FreeLibrary(self);
			thread_started = false;
		}
	}

	// On the taskbar's thread, every refresh_ms.
	static void refresh(HWND hTaskbar) noexcept
	{
		if(showing)
			return;
		const bool want = would_show(hTaskbar);
		wanted = want;
		if(want)
			start_hook_thread();
	}

	// From the Windows 11 taskbar hook (taskbar_t::hook_all), after it has
	// subclassed the taskbar, and again whenever the taskbar is recreated:
	// the timer has to be set on the taskbar's own thread, so ask it to.
	static void start() noexcept
	{
		if(primary.load())
			return;
		auto hTaskbar = ::FindWindowW(Windows::WC_Shell_TrayWnd, nullptr);
		if(hTaskbar && ::GetPropW(hTaskbar, UxSubclass))
			::PostMessageW(hTaskbar, WM_INIT, 0, 0);
	}

	// TaskbarProc, on the taskbar's thread.
	static bool on_message(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, LRESULT &result) noexcept
	{
		if(uMsg == WM_INIT)
		{
			if(primary.load() == hWnd)
			{
				result = 0;
				return true;
			}

			// The timer calls into shell.dll: keep it loaded (once).
			static bool pinned = false;
			if(!pinned)
			{
				HMODULE self{};
				pinned = ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
											  reinterpret_cast<LPCWSTR>(&refresh), &self) != FALSE;
			}

			primary = hWnd;
			::SetTimer(hWnd, timer_id, refresh_ms, [](HWND hWnd, UINT, UINT_PTR, DWORD) { refresh(hWnd); });
			refresh(hWnd);
			result = 0;
			return true;
		}
		if(uMsg == WM_SHOW)
		{
			result = 0;
			if(showing)
			{
				::EndMenu(); // Win+X again closes it, as Windows' menu does
				return true;
			}
			if(wParam == show_find)
				find_start(hWnd);
			else if(finding) // the answer from find_start_proc, in time
			{
				::KillTimer(hWnd, find_timer_id);
				finding = false;
				if(wParam == show_at)
					show(hWnd, { static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) });
				else
					show(hWnd, Point::CursorPos());
			}
			return true;
		}
		if(uMsg == WM_DESTROY && primary.load() == hWnd)
		{
			// The taskbar is being recreated: hook_all runs again and start()
			// sets up the new one. Until then, take nothing.
			::KillTimer(hWnd, timer_id);
			::KillTimer(hWnd, find_timer_id);
			finding = false;
			wanted = false;
			primary = nullptr;
		}
		return false;
	}

	// The Start button under pt, from UI Automation (what is under the cursor
	// only, as taskbar_t::is_allowed_area asks).
	static bool start_button_at(const Point &pt, RECT &rc) noexcept
	{
		try
		{
			auto &uia = taskbar_t::_IUIAutomation;
			if(!uia)
				uia.CreateInstance(__uuidof(CUIAutomation), CLSCTX_INPROC_SERVER);

			IComPtr<IUIAutomationElement> element;
			if(!uia || FAILED(uia->ElementFromPoint(pt, element)) || !element)
				return false;

			BSTR id = nullptr;
			if(FAILED(element->get_CurrentAutomationId(&id)) || !id)
				return false;
			const bool is_start = ::CompareStringOrdinal(id, -1, L"StartButton", -1, TRUE) == CSTR_EQUAL;
			DLL::Invoke(L"OleAut32.dll", "SysFreeString", id);

			return is_start && SUCCEEDED(element->get_CurrentBoundingRectangle(&rc)) && rc.right > rc.left;
		}
		catch(...)
		{
		}
		return false;
	}

	// TaskbarSubclassProc, on a right button going down: Shell's Win+X menu
	// when the click is on the Start button and Shell would show it. The
	// button-up is eaten and opens the menu, as for the taskbar menu; it has
	// to land on the button, and any other button cancels.
	static bool on_start(HWND hWnd, HWND hTaskbar, const Point &pt) noexcept
	{
		static HWND target{};
		static RECT button{};

		if(!wanted.load())
			return false;

		// As for the taskbar menu: Ctrl+Win+right-click is Windows' own, and
		// HKCU\...\disable taskbar=1 turns Shell's taskbar menus off.
		Keyboard kb;
		if(kb.get_keys_excloude_contextmenu() > 0 && kb.key_ctrl() && kb.key_win())
			return false;
		int disabled_taskbar = 0;
		if(RegistryConfig::get(L"\\disable", L"taskbar", disabled_taskbar) && disabled_taskbar == 1)
			return false;

		if(!start_button_at(pt, button) || !would_show(hTaskbar))
			return false;

		target = hTaskbar;
		_taskbar_mouse.hook(WH_MOUSE, [](int nCode, WPARAM wParam, LPARAM lParam)->LRESULT
		{
			if(nCode == HC_ACTION && lParam)
			{
				auto mh = reinterpret_cast<const MOUSEHOOKSTRUCT *>(lParam);
				if(wParam == WM_RBUTTONUP)
				{
					_taskbar_mouse.unhook();
					if(::PtInRect(&button, mh->pt))
					{
						show(target, { button.left, button.top });
						return 1;
					}
				}
				else if(wParam == WM_LBUTTONDOWN || wParam == WM_MBUTTONDOWN || wParam == WM_NCLBUTTONDOWN)
				{
					_taskbar_mouse.unhook();
				}
			}
			return _taskbar_mouse.callnext(nCode, wParam, lParam);
		}, hWnd);
		return true;
	}
}

static void start_winx_hotkey() noexcept { winx_hotkey::start(); }

/*
	The Windows 11 taskbar has no Win32 menu of its own: its right-click menu
	is a XAML flyout. To show Shell's menu there, the right-click is eaten and
	a popup is tracked on an empty menu, which NtUserTrackPopupMenu fills. If
	Shell then declines to build it (settings.exclude matches the taskbar, or
	the config's visibility hides it), the empty menu is all that is shown.

	So the decision is made first, on the right button going down, and a
	right-click Shell would decline is left alone: Windows shows its own
	taskbar menu. Applies to every taskbar (Shell_TrayWnd and, on other
	monitors, Shell_SecondaryTrayWnd). Not used on Windows 10, whose taskbar
	is not hooked here: its menu is a real Win32 menu, which a declined
	popup falls through to.
*/

// Set while this thread sends the WM_CONTEXTMENU that follows a right-click
// the taskbar check already let through, so that it is not evaluated twice.
static thread_local bool _taskbar_checked = false;

static bool taskbar_declined(HWND hTaskbar) noexcept
{
	if(_taskbar_checked)
		return false;
	return ContextMenu::TaskbarDeclined(hTaskbar, _loader.explorer);
}

LRESULT __stdcall TaskbarProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	auto wndproc = (WNDPROC)::GetPropW(hWnd, UxSubclass);
	
	if(wndproc == nullptr)
		return ::DefWindowProcW(hWnd, uMsg, wParam, lParam);

	if(LRESULT result = 0; winx_hotkey::on_message(hWnd, uMsg, wParam, lParam, result))
		return result;

	if(uMsg == WM_CONTEXTMENU)
	{
		auto pt = Point::CursorPos();
		if(taskbar_t::is_allowed_area(hWnd, pt) && !taskbar_declined(hWnd))
		{
			if(ShowTaskbarContextMenu(hWnd, pt, wParam == 0 && lParam == 0 ? WM_RBUTTONDOWN : WM_CONTEXTMENU))
				return 0;
		}
	}
	else if(uMsg == WM_MOUSEACTIVATE)
	{
		auto msg = HIWORD(lParam);
		if(msg != WM_RBUTTONDOWN)
			return wndproc(hWnd, uMsg, wParam, lParam);

		auto pt = Point::CursorPos();

		if(taskbar_t::is_allowed_area(hWnd, pt))
		{
			int disabled_taskbar = 0;
			if(RegistryConfig::get(L"\\disable", L"taskbar", disabled_taskbar) && disabled_taskbar == 1)
				return wndproc(hWnd, uMsg, wParam, lParam);

			if(msg == WM_RBUTTONDOWN)
			{
				Keyboard kb;
				if(auto count = kb.get_keys_excloude_contextmenu(); count > 0)
				{
					if(kb.key_ctrl())
					{
						if(count == 2 && kb.key_win())
							return wndproc(hWnd, uMsg, wParam, lParam);

						if(count == 1 && !_initializer.has_error())
						{
							if(!_initializer.Status.Loaded)
								_initializer.OnState();
						}
					}
				}
			}

			if(_initializer.has_error())
			{
				_initializer.OnState();
				if(_initializer.has_error(true))
					return wndproc(hWnd, uMsg, wParam, lParam);
			}
			else
			{
				if(_initializer.Status.Disabled)
					return wndproc(hWnd, uMsg, wParam, lParam);
			}

			if(!is_registered())
			{
				return wndproc(hWnd, uMsg, wParam, lParam);
			}

			// Shell would not build this menu: leave the click to Windows.
			if(taskbar_declined(hWnd))
				return wndproc(hWnd, uMsg, wParam, lParam);

			::SetFocus(hWnd);
			_taskbar_checked = true;
			::SendMessageW(hWnd, WM_CONTEXTMENU, 0, 0);
			_taskbar_checked = false;

			return MA_ACTIVATEANDEAT;
		}
	}
	else if(uMsg == WM_SETTINGCHANGE)
	{
		if(SPI_SETWORKAREA == wParam && ::GetSystemMetrics(SM_CMONITORS) > 1)
		{
			taskbar_t::hook_all(1000);
		}
	}
	else if(uMsg == WM_DESTROY)
	{
		taskbar_t::unhook(hWnd);
		taskbar_t::hook_all(1000);
	}

	return wndproc(hWnd, uMsg, wParam, lParam);
}

LRESULT __stdcall TaskbarSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, [[maybe_unused]] UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
	if(uMsg == WM_CONTEXTMENU)
	{
		auto hTaskbar = reinterpret_cast<HWND>(dwRefData);
		auto pt = Point::CursorPos();
		if(taskbar_t::is_allowed_area(hTaskbar, pt) && !taskbar_declined(hTaskbar))
		{
			if(ShowTaskbarContextMenu(hTaskbar, pt, wParam == 0 && lParam == 0? WM_RBUTTONDOWN : WM_CONTEXTMENU))
				return 0;			
		}
	}
	else if(uMsg == WM_MOUSEACTIVATE)
	{
		auto lret = ::DefSubclassProc(hWnd, uMsg, wParam, lParam);
		auto msg = HIWORD(lParam);

		if(/*msg != WM_LBUTTONDOWN && */msg != WM_RBUTTONDOWN)
			return lret;

		auto hTaskbar = reinterpret_cast<HWND>(dwRefData);
		auto pt = Point::CursorPos();

		// settings.winx.hotkey: a right-click on Start opens Shell's Win+X menu.
		if(msg == WM_RBUTTONDOWN && winx_hotkey::on_start(hWnd, hTaskbar, pt))
			return MA_ACTIVATEANDEAT;

		if(taskbar_t::is_allowed_area(hTaskbar, pt))
		{
			int disabled_taskbar = 0;
			if(RegistryConfig::get(L"\\disable", L"taskbar", disabled_taskbar) && disabled_taskbar == 1)
				return lret;

			if(msg == WM_RBUTTONDOWN)
			{
				Keyboard kb;
				if(auto count = kb.get_keys_excloude_contextmenu(); count > 0)
				{
					if(kb.key_ctrl())
					{
						if(count == 2 && kb.key_win())
							return lret;

						if(count == 1 && !_initializer.has_error())
						{
							if(!_initializer.Status.Loaded)
								_initializer.OnState();
						}
					}
				}
			}

			if(_initializer.has_error())
			{
				if(msg != WM_RBUTTONDOWN)
					return lret;

				_initializer.OnState();

				if(_initializer.has_error(true))
					return lret;
			}
			else
			{
				if(_initializer.Status.Disabled)
					return lret;
			}

			if(!is_registered())
			{
				_taskbar_mouse.unhook();
				taskbar_t::unhook(hWnd);
			//	for(auto &window : _window_taskbar)
			//		window.second.remove_prop(UxSubclass);
				return lret;
			}

			if(msg == WM_RBUTTONDOWN)
			{
				// Shell would not build this menu: leave the click to Windows.
				if(taskbar_declined(hTaskbar))
					return lret;

				_taskbar_mouse.hook(WH_MOUSE, [](int nCode, WPARAM wParam, LPARAM lParam)->LRESULT
				{
					//auto lret = _taskbar_mouse.callnext(nCode, wParam, lParam);
					if(nCode == HC_ACTION && wParam == WM_RBUTTONUP)
					{
						auto mh = reinterpret_cast<MOUSEHOOKSTRUCT *>(lParam);
						Window window = ::GetParent(mh->hwnd);
						if(window.is_prop(UxSubclass))
						{
							_taskbar_mouse.unhook();
							::SetFocus(window);
							ShowTaskbarContextMenu(window, mh->pt, WM_CONTEXTMENU);
							return 1;
						}
					}
					return _taskbar_mouse.callnext(nCode, wParam, lParam);
				}, hWnd);
				
				return MA_ACTIVATEANDEAT;
			}

			//if(msg != WM_RBUTTONDOWN)
			//	::SetFocus(hWnd);

			/*
			if(msg == WM_RBUTTONDOWN)
			{
				::SetFocus(hWnd);
				//ShowTaskbarContextMenu(hWnd, pt, WM_RBUTTONDOWN);
				SendMessageW(hWnd, WM_CONTEXTMENU, 0, 0);
			}
			*/
		}
		return lret;
	}
	else if(uMsg == WM_SETTINGCHANGE)
	{
		if(SPI_SETWORKAREA == wParam && ::GetSystemMetrics(SM_CMONITORS) > 1)
		{
			taskbar_t::hook_all(1000);
		}
	}
	else if(uMsg == WM_DESTROY)
	{
		taskbar_t::unhook(hWnd);
		taskbar_t::hook_all(1000);
	}

	return ::DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

/*
	One set of hooks per process.

	A stale registration (say {...BAF3} still naming C:\Windows\nilesoft\shell.dll
	after an install in Program Files) maps two copies of shell.dll into
	Explorer. Each one detours CoCreateInstance and patches TrackPopupMenu(Ex)
	in every module, and with both in place no context menu appears at all.
	The hook logic itself is unchanged; this only decides, in DllMain, whether
	this copy installs it:

	  1. A copy that is not the one {...BAF1} resolves to (HKCR, as Explorer
	     sees it) stands down, when that registered file exists. Not applied in
	     shell.exe, which loads its own shell.dll whatever is registered.
	  2. A Nilesoft shell.dll that predates this guard (no HookGuard export) is
	     already mapped: it hooked in its own DllMain, so this copy stands down.
	  3. Copies with this guard agree through a per-process named section,
	     Local\NilesoftShell.Hooks.<pid>: whoever creates it hooks and writes
	     its path there; a later copy finds it, logs the owner and stands down.
	     If the section cannot be created at all, first come hooks.

	A copy that stands down logs why, declines DllGetClassObject and leaves
	unhooking alone on detach.
*/
namespace hook_guard
{
	bool owner = false;
	HANDLE section{};
	constexpr DWORD section_size = MAX_PATH * 2 * sizeof(wchar_t);

	// Exported (Shell.def): marks a shell.dll that follows rule 3.
	extern "C" UINT __stdcall HookGuard() { return 1; }

	static bool same_name(const wchar_t *path, const wchar_t *name)
	{
		auto slash = ::wcsrchr(path, L'\\');
		return ::CompareStringOrdinal(slash ? slash + 1 : path, -1, name, -1, TRUE) == CSTR_EQUAL;
	}

	// Rule 1: the DLL {...BAF1} resolves to, in this process's registry view;
	// empty if it is not registered or the file is missing (undeterminable).
	static string registered_path()
	{
		wchar_t path[MAX_PATH * 2]{};
		DWORD bytes = sizeof(path) - sizeof(wchar_t);
		if(ERROR_SUCCESS != ::RegGetValueW(HKEY_CLASSES_ROOT, L"CLSID\\" CLS_ContextMenu L"\\InprocServer32",
										   nullptr, RRF_RT_REG_SZ, nullptr, path, &bytes))
			return {};
		string value = path;
		value.trim(L" \t\"");
		auto attr = ::GetFileAttributesW(value.c_str());
		if(attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY))
			return {};
		return value.move();
	}

	// Rule 2: another Nilesoft shell.dll without the HookGuard export.
	static string legacy_instance()
	{
		for(auto module : Process::Modules(::GetCurrentProcess()))
		{
			if(module == _hInstance)
				continue;
			wchar_t path[MAX_PATH * 2]{};
			if(!::GetModuleFileNameW(module, path, MAX_PATH * 2) || !same_name(path, L"shell.dll"))
				continue;
			if(!::GetProcAddress(module, "DllGetClassObject") || ::GetProcAddress(module, "HookGuard"))
				continue;
			if(IsNilesoftModule(module))
				return path;
		}
		return {};
	}

	// True if this copy is to install the hooks.
	static bool acquire()
	{
		auto self = Path::Module(_hInstance).move();

		if(!_loader.path.ends_with(L"\\shell.exe"))
		{
			auto registered = registered_path().move();
			if(!registered.empty() && !RegistryConfig::SameFile(registered, self))
			{
				_log.warning(L"Not hooking in %s (pid %lu): this shell.dll ('%s') is not the registered one ('%s'). "
							 L"A stale registration loads it; run 'shell -status' and see \"conflicts\".",
							 _loader.path.c_str(), ::GetCurrentProcessId(), self.c_str(), registered.c_str());
				return false;
			}
		}

		if(auto legacy = legacy_instance().move(); !legacy.empty())
		{
			_log.warning(L"Not hooking in %s (pid %lu): an older Nilesoft shell.dll ('%s') is already loaded and hooked. "
						 L"Remove its registration (shell -status lists it under \"conflicts\") and restart Explorer.",
						 _loader.path.c_str(), ::GetCurrentProcessId(), legacy.c_str());
			return false;
		}

		wchar_t name[64]{};
		::swprintf_s(name, L"Local\\NilesoftShell.Hooks.%lu", ::GetCurrentProcessId());
		section = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, section_size, name);
		if(!section)
			return true;	// cannot tell: first come hooks

		const bool existed = ::GetLastError() == ERROR_ALREADY_EXISTS;
		auto view = static_cast<wchar_t *>(::MapViewOfFile(section, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, section_size));
		if(!existed)
		{
			if(view)
				::wcsncpy_s(view, section_size / sizeof(wchar_t), self.c_str(), _TRUNCATE);
			if(view)
				::UnmapViewOfFile(view);
			return true;
		}

		string other = view ? view : L"";
		if(view)
			::UnmapViewOfFile(view);
		::CloseHandle(section);
		section = nullptr;

		// shell.exe -config:tree builds Windows' context menu, which loads the registered
		// shell.dll as a menu handler next to shell.exe's own copy. Expected there, and
		// not worth a warning in shell.log every time the Menu map refreshes.
		if(_loader.path.ends_with(L"\\shell.exe"))
			return false;

		_log.warning(L"Not hooking in %s (pid %lu): another Nilesoft shell.dll ('%s') already hooked this process; "
					 L"this one is '%s'.",
					 _loader.path.c_str(), ::GetCurrentProcessId(), other.c_str(), self.c_str());
		return false;
	}

	static void release()
	{
		if(section)
			::CloseHandle(section);
		section = nullptr;
	}
}

//integrate
BOOL APIENTRY DllMain(HINSTANCE hInstance, DWORD dwReason, LPVOID)
{
	try
	{
		if(dwReason == DLL_PROCESS_ATTACH)
		{
			::DisableThreadLibraryCalls(hInstance);
			//InitCommonControls();
			/*INITCOMMONCONTROLSEX icex{};
			// Load the ToolTip class from the DLL.
			icex.dwSize = sizeof(icex);
			icex.dwICC = ICC_BAR_CLASSES;
			InitCommonControlsEx(&icex);
			*/

			_hInstance = hInstance;
			_initializer.HInstance = _hInstance;

#if _DEBUG
			// detect memory leaks
			_CrtSetDbgFlag(_CRTDBG_REPORT_FLAG
						   | _CRTDBG_CHECK_ALWAYS_DF
						   | _CRTDBG_LEAK_CHECK_DF
						   | _CRTDBG_ALLOC_MEM_DF);

			//::_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_DEBUG);
#endif
			_loader.init();

			if(!_loader.explorer && !_loader.path.ends_with(L"\\shell.exe"))
			{
				
				//	if(_loader.path.ends_with(L"\\firefox.exe") || _loader.path.ends_with(L"\\devenv.exe"))
				//		return 0;
				
				int disabled_3rdparty = 0;
				if(RegistryConfig::get(L"\\disable", L"3rdparty", disabled_3rdparty) && disabled_3rdparty == 1)
					return 0;

				/*if(RegistryConfig::IsFolderExtensions())
				{
					if(!hasImportTrackPopupMenu())
						return 0;
				}*/
			}

			//if(!loader)
			//	_log.info(L"Load '%s'", Path::Module(nullptr).c_str());

			if(_initializer.init(_hInstance))
			{
				// CoInitializeEx and the IGlobalOptions activation used to run
				// here. Both are COM calls under the loader lock, which is
				// documented as unsupported and a plausible cause of Explorer
				// hangs. They moved to Initializer::ensure_com, which runs on the
				// thread that builds menus.

				_initializer.process.hModule = _loader.handle;
				_initializer.process.path = Path::Module(_loader.handle).move();
				_initializer.process.name = Path::Title(_initializer.process.path).move();
				_initializer.process.id = ::GetCurrentProcessId();
				_initializer.process.handle = ::GetCurrentProcess();

				//_log.info(L"%s", _initializer.process.name.c_str());

				// Another copy of shell.dll already hooks this process (or will,
				// being the registered one): see hook_guard.
				hook_guard::owner = hook_guard::acquire();
				if(!hook_guard::owner)
					return TRUE;

				ContextMenu::RegisterLayer();

				iathook_NtUserTrackPopupMenuEx
					.init(L"user32.dll", "win32u.dll", "NtUserTrackPopupMenuEx", TrackPopupMenuExProc)
					.install();

				auto hook = []()
				{
					__trace(L"hook all the modules in '%s' process", _initializer.process.name.c_str());

					auto user32 = "user32.dll";

					// hooh all the modules in this process.
					for(auto hModule : Process::Modules(_initializer.process.handle))
					{
						if(_hInstance == hModule)
							continue;

						bool module_exists = false;
						for(const auto &m : iathook_TrackPopupMenu)
						{
							if(m._hModule == hModule)
							{
								module_exists = true;
								break;
							}
						}

						if(!module_exists)
						{
							iathook_TrackPopupMenu.emplace_back(hModule, user32, ::TrackPopupMenu, TrackPopupMenuProc).install();
							iathook_TrackPopupMenu.emplace_back(hModule, user32, ::TrackPopupMenuEx, TrackPopupMenuExProc).install();
						}
					}
				};

				//_TrackPopupMenu.init(::TrackPopupMenu, TrackPopupMenuProc).hook();
				//_TrackPopupMenuEx.init(::TrackPopupMenuEx, TrackPopupMenuExProc).hook();
				//_CoCreateInstance.init(::CoCreateInstance, CoCreateInstanceProc).hook();
				
				if(_loader.explorer)
				{
					//_log.info(_loader.path);

					if(_CoCreateInstance.Begin() == NO_ERROR)
					{
						//_DllGetClassObject.init(hModule, "DllGetClassObject", DllGetClassObjectHook).hook();
						_CoCreateInstance.init(::CoCreateInstance, CoCreateInstanceHook).hook();
						//auto m = &_detours_ci[hModule];
						//if(!m->installed())
						//m->init(hModule, "api-ms-win-core-com-l1-1-0.dll", ::CoCreateInstance, CoCreateInstanceProc).install();
						if(_CoCreateInstance.Commit() != NO_ERROR)
							_CoCreateInstance.forget();
					}

					if(!iathook_NtUserTrackPopupMenuEx.installed())
					{
						std::thread([=]() { ::Sleep(2000); hook(); }).detach();
					}

					if(ver->IsWindows11OrGreater())
					{

						/*
						auto keyCLSID = Registry::ClassesRoot.OpenSubKey(L"CLSID\\" CLS_FileExplorerContextMenu "\\TreatAs", false, false);
						if(!(keyCLSID && keyCLSID.GetString(nullptr).equals(CLS_ContextMenu)))
						{
							auto hModule = ::GetModuleHandleW(Windows_UI_FileExplorer);

							if(!hModule)
								hModule = ::LoadLibraryW(Windows_UI_FileExplorer);

							if(hModule)
							{
								auto r = _CoCreateInstance.Begin();
								_log.info(L"%x", r);
								//_DllGetClassObject.init(hModule, "DllGetClassObject", DllGetClassObjectHook).hook();
								_CoCreateInstance.init(::CoCreateInstance, CoCreateInstanceHook).hook();
								//auto m = &_detours_ci[hModule];
								//if(!m->installed())
								//m->init(hModule, "api-ms-win-core-com-l1-1-0.dll", ::CoCreateInstance, CoCreateInstanceProc).install();
								_CoCreateInstance.Commit();
							}
						}
						keyCLSID.Close();
						*/

						taskbar_t::hook_all(1000);
					}
				}
				else if(!iathook_NtUserTrackPopupMenuEx.install())
				{
					hook();
				}
			}
			//IsRegistered
			/*
			IID iid{};
			CLSIDFromString(L"{23170F69-40C1-278A-1000-000100020000}", &iid);
			Guid guid = iid;
			MB(guid.to_string(2).c_str());
			*/
			//UuidFromStringW

			return TRUE;
		}
		else if(dwReason == DLL_PROCESS_DETACH)
		{
			// A copy that stood down (hook_guard) installed nothing to remove.
			if(hook_guard::owner && !is_registered())
			{
				if(_loader.explorer)
				{
					// Perform any necessary cleanup.
					_taskbar_mouse.unhook();
					taskbar_t::unhook_all();
					//detour_NtUserTrackPopupMenuEx.uninstall(true);

					if(ver->IsWindows11OrGreater())
					{
						if(_CoCreateInstance.Begin() == NO_ERROR)
						{
							//_DllGetClassObject.unhook();
							_CoCreateInstance.unhook();
							_CoCreateInstance.Commit();
						}
					}
				}

				for(auto &d : iathook_TrackPopupMenu)
					d.uninstall(true);

				//_TrackPopupMenu.unhook();
				//_TrackPopupMenuEx.unhook();
				//_CoCreateInstance.unhook();

				ContextMenu::UnRegisterLayer();
			}

			taskbar_t::_IUIAutomation.release();
			hook_guard::release();

			_log.close();

			//::SendNotifyMessageW(HWND_BROADCAST, WM_NULL, 0, 0);	

			return TRUE;
		}
	}
	catch(...) // handle all exceptions
	{
#ifdef _DEBUG
		_log.exception(__func__);
		_log.close();
#endif
	}
	return FALSE;
}

/*
	Third-party file managers (settings.filemanagers, off unless the config turns it on).

	Explorer gives the hooked menu its selection through the folder view's
	IShellBrowser. Total Commander, XYplorer, Double Commander, Everything and other
	programs build Windows' file menu themselves and have no such view, so Shell's
	menu there knew nothing about the files. When the setting allows the program,
	DllGetClassObject hands the host a real (empty) context menu handler: the host
	calls IShellExtInit::Initialize with the selection's IDataObject, and the menu
	the host then shows on that thread reads the files from it
	(Selections::QuerySelected).

	- The data object is offered once, to the next menu on the same thread, and
	  only for a few seconds after the host prepared the handler; the handler
	  going away withdraws it. A later menu of the program's own (a toolbar, a
	  tab) never sees an old selection.
	- The handler adds no items and runs no commands.
	- DllCanUnloadNow says no while any of these objects are alive.
*/
namespace filemanager
{
	std::atomic<long> objects{ 0 };

	constexpr ULONGLONG offer_ms = 10000;

	// The selection offered to the next menu on this thread. A plain struct (no
	// destructor): what is left when a thread ends is one reference, never freed
	// under the loader lock.
	struct offer_t
	{
		const void *handler;
		IDataObject *data;
		ULONGLONG time;
	};
	thread_local offer_t offer{};

	static void withdraw(const void *handler = nullptr)
	{
		if(handler && offer.handler != handler)
			return;
		if(offer.data)
			offer.data->Release();
		offer = {};
	}

	class Handler final : public IShellExtInit, public IContextMenu
	{
		LONG _refs = 1;
		IDataObject *_data{};

	public:
		Handler() { objects++; }
		~Handler()
		{
			withdraw(this);
			if(_data)
				_data->Release();
			objects--;
		}

		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) noexcept override
		{
			if(!ppv)
				return E_POINTER;
			if(riid == IID_IUnknown || riid == IID_IShellExtInit)
				*ppv = static_cast<IShellExtInit *>(this);
			else if(riid == IID_IContextMenu)
				*ppv = static_cast<IContextMenu *>(this);
			else
			{
				*ppv = nullptr;
				return E_NOINTERFACE;
			}
			AddRef();
			return S_OK;
		}
		ULONG STDMETHODCALLTYPE AddRef() noexcept override { return static_cast<ULONG>(::InterlockedIncrement(&_refs)); }
		ULONG STDMETHODCALLTYPE Release() noexcept override
		{
			auto refs = ::InterlockedDecrement(&_refs);
			if(refs == 0)
				delete this;
			return static_cast<ULONG>(refs);
		}

		HRESULT STDMETHODCALLTYPE Initialize(PCIDLIST_ABSOLUTE, IDataObject *data, HKEY) noexcept override
		{
			if(_data)
				_data->Release();
			_data = data;
			if(_data)
				_data->AddRef();
			arm();
			return S_OK;
		}

		// Right before the host shows its menu: (re)offer the selection.
		HRESULT STDMETHODCALLTYPE QueryContextMenu(HMENU, UINT, UINT, UINT, UINT) noexcept override
		{
			arm();
			return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 0);
		}

		HRESULT STDMETHODCALLTYPE InvokeCommand(CMINVOKECOMMANDINFO *) noexcept override { return E_FAIL; }
		HRESULT STDMETHODCALLTYPE GetCommandString(UINT_PTR, UINT, UINT *, CHAR *, UINT) noexcept override { return E_NOTIMPL; }

	private:
		void arm()
		{
			withdraw();
			if(!_data)
				return;
			_data->AddRef();
			offer = { this, _data, ::GetTickCount64() };
		}
	};

	class Factory final : public IClassFactory
	{
		LONG _refs = 1;

	public:
		Factory() { objects++; }
		~Factory() { objects--; }

		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) noexcept override
		{
			if(!ppv)
				return E_POINTER;
			if(riid == IID_IUnknown || riid == IID_IClassFactory)
			{
				*ppv = static_cast<IClassFactory *>(this);
				AddRef();
				return S_OK;
			}
			*ppv = nullptr;
			return E_NOINTERFACE;
		}
		ULONG STDMETHODCALLTYPE AddRef() noexcept override { return static_cast<ULONG>(::InterlockedIncrement(&_refs)); }
		ULONG STDMETHODCALLTYPE Release() noexcept override
		{
			auto refs = ::InterlockedDecrement(&_refs);
			if(refs == 0)
				delete this;
			return static_cast<ULONG>(refs);
		}

		HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown *outer, REFIID riid, void **ppv) noexcept override
		{
			if(!ppv)
				return E_POINTER;
			*ppv = nullptr;
			if(outer)
				return CLASS_E_NOAGGREGATION;
			auto handler = new(std::nothrow) Handler();
			if(!handler)
				return E_OUTOFMEMORY;
			auto hr = handler->QueryInterface(riid, ppv);
			handler->Release();
			return hr;
		}

		HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) noexcept override
		{
			if(lock)
				objects++;
			else
				objects--;
			return S_OK;
		}
	};

	static HRESULT get_class_object(REFIID riid, void **ppv) noexcept
	{
		auto factory = new(std::nothrow) Factory();
		if(!factory)
			return E_OUTOFMEMORY;
		auto hr = factory->QueryInterface(riid, ppv);
		factory->Release();
		return hr;
	}

	// settings.filemanagers: true for every program but Explorer, or a program
	// name or a list of them (as in exclude.process), such as ['totalcmd64', 'xyplorer'].
	static bool enabled()
	{
		auto cache = _initializer.cache;
		if(!cache || !cache->settings.filemanagers)
			return false;

		try
		{
			Selections selected;
			Keyboard keyboard;
			Context context;
			context.Selections = &selected;
			context.Application = &_initializer.application;
			context.Cache = cache;
			context.Keyboard = &keyboard;
			context.variables.global = &cache->variables.global;
			context.variables.runtime = &cache->variables.runtime;
			context.variables.local = nullptr;

			Object value = context.Eval(cache->settings.filemanagers).move();
			if(value.is_null())
				return false;

			auto &name = _initializer.process.name;
			auto matches = [&](const Object &o)
			{
				string want = o.to_string().trim().move();
				if(want.empty() || name.empty())
					return false;
				if(want.ends_with(L".exe"))
					want = want.substr(0, want.length() - 4).move();
				return name.equals(want);
			};

			if(value.is_array())
			{
				auto ptr = value.ptr();
				int count = ptr[0];
				for(int i = 0; i < count; i++)
				{
					if(matches(ptr[i + 1]))
						return true;
				}
				return false;
			}
			if(value.is_string())
				return matches(value);
			return value.to_bool();
		}
		catch(...)
		{
		}
		return false;
	}
}

// The selection a third-party file manager gave Shell's handler for the menu
// about to show on this thread, once (Selections::QuerySelected). Null when there
// is none. The caller releases it.
IDataObject *FileManagerTakeDataObject()
{
	auto &offer = filemanager::offer;
	if(!offer.data)
		return nullptr;
	IDataObject *data = nullptr;
	if(::GetTickCount64() - offer.time < filemanager::offer_ms)
	{
		data = offer.data;
		offer.data = nullptr;
	}
	filemanager::withdraw();
	return data;
}

//IID_FolderExtensions
//#pragma comment(linker, "/export:DllGetClassObject=DllGetClassObject")
_Check_return_
bool MenuTreeReadingMenus(); // MenuTree.cpp

STDAPI DllGetClassObject(_In_ REFCLSID rclsid, [[maybe_unused]] _In_ REFIID riid, [[maybe_unused]] _Outptr_ LPVOID FAR *ppv)
{
	auto hr = E_NOTIMPL;// CLASS_E_CLASSNOTAVAILABLE,E_NOTIMPL, E_OUTOFMEMORY, and E_UNEXPECTED,E_INVALIDARG
	Guid iid = rclsid;
	//Guid iid2 = riid;
	if(ppv) *ppv = nullptr;

	__try
	{
		// shell -config:tree is reading menus in this process: this copy joins none.
		if(MenuTreeReadingMenus())
			return CLASS_E_CLASSNOTAVAILABLE;

		if(iid.equals(IID_FolderExtensions))
			return E_NOTIMPL;

		if(!iid.equals({ IID_ContextMenu, IID_IconOverlay }))
			return hr;

		if(Initializer::Status.Disabled)
			return hr;

		// Without the hooks this copy cannot show anything; the owner does.
		if(!hook_guard::owner)
			return hr;

		if(!is_registered())
			return hr;
		
		// Only a context-menu request (made on the thread about to show a menu)
		// may retry a broken config; icon-overlay requests can arrive on any
		// thread at any time and must not trigger a parse.
		if(_initializer.has_error(rclsid == IID_ContextMenu))
			return hr;

		if(rclsid == IID_ContextMenu)
		{
			_loader.contextmenuhandler = true;
			Selections::point.GetCursorPos();
		}

		if(!_initializer.Status.Loaded)
			_initializer.init();

		// A third-party file manager the config lets in: a real handler, so the
		// host hands over the selection (see filemanager above).
		if(rclsid == IID_ContextMenu && !_loader.explorer && ppv && _initializer.Status.Loaded &&
		   !_initializer.has_error(false) && filemanager::enabled())
		{
			hr = filemanager::get_class_object(riid, ppv);
		}
	}
	except
	{
#ifdef _DEBUG
		_log.exception(__func__);
#endif
	}
	_log.close();

	return hr;
}

//DllCanUnloadNow. COM calls this function to determine whether the object is serving any clients. 
//If not, the system can unload the DLL and free the associated memory.
// Check if we can unload the component from the memory.
// NOTE: The component can be unloaded from the memory when its reference 
// count is zero (i.e. nobody is still using the component).
//#pragma comment(linker, "/export:DllCanUnloadNow=DllCanUnloadNow")
__control_entrypoint(DllExport)
STDAPI DllCanUnloadNow(void)
{
	// A file manager still holds one of our handlers or the class factory.
	if(filemanager::objects > 0)
		return S_FALSE;

	if(!_loader.explorer || !is_registered())
		return S_OK;
	return S_FALSE;
	/*
	if(ContextMenu::Processes.size() == 0)
	{
		int p = 0;
		EnumThreadWindows(GetCurrentThreadId(), [](HWND, LPARAM lp) {
		(*((int *)lp))++;
			return 0;
		}, (LPARAM)&p);

		//if(p)MBF(L"%d", p);
		return p > 0 ? S_FALSE : S_OK;
	}

	//if(_loader.explorer)
	//	return release() ? S_OK : S_FALSE;
	return ContextMenu::Processes.size() == 0 ? S_OK : S_FALSE;
	//return S_OK;
	*/
}

// shell -config:check <root.nss>: see ConfigCheck.h.
//
// Runs the real parser on `root` with a private CACHE. The parser reaches the
// cache through Initializer::instance->cache, so that pointer is set for the
// duration; which is why this refuses to run inside Explorer or once a live
// configuration exists (it would be swapped out under a menu). In shell.exe,
// its only caller, there is never one.
static HRESULT config_check(const wchar_t *root, const wchar_t *where, ConfigCheckSink sink, ConfigTitleSink title_sink, void *context,
							const wchar_t *sample = nullptr, ConfigTreeSink tree_sink = nullptr)
{
	if(!root || !*root || !sink)
		return E_INVALIDARG;

	if(_loader.explorer || _initializer.cache || _initializer.Status.Loaded)
		return E_ACCESSDENIED;

	std::vector<ParseDiagnostic> diagnostics;
	std::vector<Parser::TitleInfo> titles;
	std::wstring tree;
	bool ok = false;
	bool where_ok = true;

	// Restores the global state whichever way the parse ends.
	struct scope_t
	{
		std::unique_ptr<CACHE> cache = std::make_unique<CACHE>();
		scope_t(std::vector<ParseDiagnostic> *list)
		{
			_initializer.cache = cache.get();
			Parser::Diagnostics = list;
			// Reported through the sink instead; the user's shell.log is not
			// the place for diagnostics about a scratch copy.
			Logger::Muted = true;
		}
		~scope_t()
		{
			Logger::Muted = false;
			Parser::Diagnostics = nullptr;
			_initializer.cache = nullptr;
			Initializer::MAP_MUID.clear();
		}
	};

	try
	{
		Initializer::ensure_com();

		scope_t scope(&diagnostics);
		scope.cache->glyph.name = FontCache::Default;
		// As init() does: the parser verifies names such as title.copy
		// against these, and expressions may query installed packages.
		_initializer.load_mui();
		scope.cache->Packages.load();

		Parser parser(root);
		ok = parser.Load() && !parser.HasError();
		// While the cache and the MUI titles are still there.
		if(ok && title_sink)
			where_ok = parser.ReportTitles(titles, where);
		if(ok && tree_sink)
			where_ok = parser.ReportTree(tree, where, sample);
	}
	catch(...)
	{
		return E_FAIL;
	}

	bool reported_error = false;
	for(auto &d : diagnostics)
	{
		reported_error |= d.error;
		sink(context, d.error, d.file.c_str(), static_cast<UINT>(d.line), static_cast<UINT>(d.column), d.message.c_str());
	}

	if(!ok && !reported_error)
		sink(context, TRUE, root, 0, 0, L"the configuration could not be loaded");

	if(!where_ok)
		return E_INVALIDARG;

	if(tree_sink && ok)
		tree_sink(context, tree.c_str());

	for(auto &t : titles)
	{
		CONFIG_TITLE info{};
		info.state = t.state == Parser::TitleInfo::Text ? CONFIG_TITLE_TEXT :
			t.state == Parser::TitleInfo::Empty ? CONFIG_TITLE_EMPTY : CONFIG_TITLE_DYNAMIC;
		info.kind = t.kind == Parser::TitleInfo::Menu ? CONFIG_TITLE_MENU :
			t.kind == Parser::TitleInfo::Item ? CONFIG_TITLE_ITEM : CONFIG_TITLE_MODIFY;
		info.depth = t.depth;
		info.file = t.file.c_str();
		info.line = static_cast<UINT>(t.line);
		info.column = static_cast<UINT>(t.column);
		info.source = t.source.c_str();
		info.title = t.text.c_str();
		title_sink(context, &info);
	}

	return ok ? S_OK : S_FALSE;
}

STDAPI ConfigCheck(const wchar_t *root, ConfigCheckSink sink, void *context)
{
	return config_check(root, nullptr, sink, nullptr, context);
}

// shell -config:check <root.nss> -titles: see ConfigCheck.h.
STDAPI ConfigCheckTitles(const wchar_t *root, const wchar_t *where, ConfigCheckSink sink, ConfigTitleSink titles, void *context)
{
	if(!titles)
		return E_INVALIDARG;
	return config_check(root, where, sink, titles, context);
}

// shell -config:tree <root.nss> -context:<where>: see ConfigCheck.h.
STDAPI ConfigTree(const wchar_t *root, const wchar_t *where, const wchar_t *sample, ConfigCheckSink sink, ConfigTreeSink tree, void *context)
{
	if(!tree || !where || !*where)
		return E_INVALIDARG;
	return config_check(root, where, sink, nullptr, context, sample, tree);
}

// Register the COM server and the context menu handler.
HRESULT Register(bool reg)
{
	try
	{
		if(reg)
		{
			Path::Delete(_log.path());
			//_log.reset();
		}

		if(!ver->IsWindows7OrGreater())
		{
			//windows compatibility
			_log.error(L"%s", string::Extract(_hInstance, IDS_WINDOWS_COMPATIBILITY).c_str());
			return ERROR_EXE_MACHINE_TYPE_MISMATCH;//ERROR_DS_VERSION_CHECK_FAILURE
		}

		auto is_elevated = Security::Elevation::IsElevated();

		if(!is_elevated)
		{
			// Missing administrative privileges!
			string msg = string::Extract(_hInstance, IDS_ADMIN_PRIVILEGES).move();
			//You will need to provide administrator permission to run this Shell
			_log.error(L"%s", msg.c_str());
			return ERROR_ACCESS_DENIED;
		}

		string path = Path::Module(_hInstance).move();

		// Registration no longer widens the install folder's ACL (it tried to
		// grant BUILTIN\Users GENERIC_ALL on the folder holding this DLL); see
		// the matching comment in exe/src/Main.cpp.

		string msg;

		if(reg)
		{
			//_log.create();
			REGOP regop{};
			regop.REGISTER = regop.CONTEXTMENU = regop.ICONOVERLAY = true;

			if(!RegistryConfig::Register(path, regop))
			{
				msg = string::Extract(_hInstance, IDS_REGISTER_NOT_SUCCESS).move();
				_log.error(L"%s", msg.c_str());
				return S_FALSE;
			}
			msg = string::Extract(_hInstance, IDS_REGISTER_SUCCESS).move();
		}
		else
		{
			// Machine registration only: a per-user one is not ours to remove here.
			if(!RegistryConfig::IsRegistered(RegScope::Machine))
				return S_OK;

			if(!RegistryConfig::Unregister())
			{
				msg = string::Extract(_hInstance, IDS_UNREGISTER_NOT_SUCCESS).move();
				_log.error(L"%s", msg.c_str());
				return S_FALSE;;
			}
			msg = string::Extract(_hInstance, IDS_UNREGISTER_SUCCESS).move();
		}

		_log.info(L"%s", msg.c_str());

		_log.close();

		return S_OK;
	}
	catch(...)
	{
#ifdef _DEBUG
		_log.exception(__func__);
#endif
	}
	_log.close();
	return S_FALSE;
}
/*
//  Register the COM server and the context menu handler.
__control_entrypoint(DllExport)
STDAPI DllRegisterServer()
{
	return Register(true);
}

__control_entrypoint(DllExport)
STDAPI DllUnregisterServer()
{
	return Register(false);
}
*/
/*
STDAPI DllInstall(BOOL bInstall, _In_opt_ PCWSTR pszCmdLine)
{
	MBF(L"%d %s", bInstall, pszCmdLine);
	return E_NOTIMPL;
}
*/

/*
#include <Shldisp.h>

CoInitialize(NULL);
// Create an instance of the shell class
IShellDispatch4 *pShellDisp = NULL;
HRESULT sc = CoCreateInstance(CLSID_Shell, NULL, CLSCTX_SERVER, IID_IDispatch, (LPVOID *) &pShellDisp );
// Show the desktop
sc = pShellDisp->ToggleDesktop();
// Restore the desktop
sc = pShellDisp->ToggleDesktop();
pShellDisp->Release();
 */

#pragma endregion

uint32_t ImmersiveColor::GetColorByColorType(uint32_t colorType)
{
	auto colorSet = GetImmersiveUserColorSetPreference(false, false);
	return GetImmersiveColorFromColorSetEx(colorSet, colorType, false, 0);
}

uint32_t ImmersiveColor::GetColorByName(const wchar_t *name)
{
	auto colorSet = GetImmersiveUserColorSetPreference(false, false);
	auto colorType = GetImmersiveColorTypeFromName(name);
	return GetImmersiveColorFromColorSetEx(colorSet, colorType, false, 0);
}

uint32_t ImmersiveColor::GetImmersiveUserColorSetPreference(bool bForceCheckRegistry, bool bSkipCheckOnFail)
{
	return DLL::Invoke<uint32_t>(hUxTheme, 98, bForceCheckRegistry, bSkipCheckOnFail);
}

uint32_t ImmersiveColor::GetImmersiveColorFromColorSetEx(uint32_t dwImmersiveColorSet, uint32_t dwImmersiveColorType, bool bIgnoreHighContrast, uint32_t dwHighContrastCacheMode)
{
	return DLL::Invoke<uint32_t>(hUxTheme, 95, dwImmersiveColorSet, dwImmersiveColorType, bIgnoreHighContrast, dwHighContrastCacheMode);
}

uint32_t ImmersiveColor::GetImmersiveColorTypeFromName(const wchar_t *name)
{
	return DLL::Invoke<uint32_t>(hUxTheme, 96, name);
}

uint32_t ImmersiveColor::GetImmersiveColorSetCount()
{
	return DLL::Invoke<uint32_t>(hUxTheme, 94);
}

bool ImmersiveColor::IsSupported()
{
	return  ::IsCompositionActive() && DLL::IsFunc(hUxTheme, 95);
}
