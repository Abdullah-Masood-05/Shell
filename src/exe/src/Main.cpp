
#include <System.h>
#include <Globals.h>
#include <Resource.h>
#include <dwmapi.h>

#include <vector>
#include <commctrl.h>
#include "Control.h"
#include <shlwapi.h>
#include <shlobj.h>
#include <Library/PlutoVGWrap.h>
#include <RegistryConfig.h>
#include <ConfigLocation.h>
#include <ConfigCheck.h>

//#pragma comment(lib, "mincore.lib")
#pragma comment(lib, "UxTheme.lib")
#pragma comment(lib, "Comctl32")
#pragma comment(lib, "dwmapi")
#pragma comment(lib, "shlwapi.lib")

#if defined(_M_ARM64)
	#pragma comment(lib, "plutosvg-arm64.lib")
//#elif defined(_M_ARM)
//	#pragma comment(lib, "plutosvg-arm.lib")
#elif defined(_M_X64)
	#pragma comment(lib, "plutosvg-x64.lib")
#else
	#pragma comment(lib, "plutosvg-x86.lib")
#endif

using namespace Nilesoft;
using namespace Nilesoft::Windows::Forms;
using namespace Nilesoft::Shell;

constexpr const wchar_t dll_name[] = L"shell.dll";

BOOL CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

#define ID_MAINWINDOW 100

// define id for close, minimize & demo buttons
#define ID_CLOSE 0x001
#define ID_MINIMIZE 0x002

#define ID_REG 0x003
#define ID_UNREG 0x004
#define ID_RESTART 0x005
#define ID_WEB 0x006
#define ID_DONATE 0x007
#define ID_EMAIL 0x008
#define ID_GITHUB 0x009
#define ID_DOCS 0x00d

/////////
#define SetWindowStyle(hwnd, style)	 ::SetWindowLongW((hwnd), GWL_STYLE, (style))
#define SetWindowExStyle(hwnd, style)	::SetWindowLongW((hwnd), GWL_EXSTYLE, (style))

#define GET_X_LPARAM(lp)    ((int)(short)LOWORD(lp))  // windowsx.h
#define GET_Y_LPARAM(lp)    ((int)(short)HIWORD(lp))  // windowsx.h

int __stdcall btn_on_paint(UI::Control *s, int, WPARAM, LPARAM);

LRESULT __stdcall WindowProc(HWND, UINT, WPARAM, LPARAM);

bool is_elevated = false;

// Global Variables:
HINSTANCE _hInstance;    // current instance
Logger *_log;
HANDLE _fonthandle = nullptr;
HFONT _hfont_icon = nullptr;
HFONT _hfont_icon2 = nullptr;
UINT _dpi = 96;
RECT rc_window{};
RECT rc_reg{};
RECT rc_unreg{};
POINT p{};
UI::Window *main_window = nullptr;
HBITMAP hbitmap_logo = nullptr;
//0xAA0000FF
COLORREF color_background = 0xffffff;

template<typename T = long>
T dpi(auto value) { return static_cast<T>((value * _dpi) / 96); }

string loadstring(UINT id, HMODULE hmodule = nullptr)
{
	string str(MAX_PATH);
	auto size = ::LoadStringW(hmodule, id, str.buffer(), MAX_PATH);
	if(size > MAX_PATH)
	{
		size = ::LoadStringW(hmodule, id, str.buffer(size + 1), size);
	}
	return str.release(size).move();
}


// Enables one privilege in this process's token. AdjustTokenPrivileges reports
// success even when the privilege is not held at all; that case is signalled
// only through GetLastError() == ERROR_NOT_ALL_ASSIGNED.
//   https://learn.microsoft.com/windows/win32/api/securitybaseapi/nf-securitybaseapi-adjusttokenprivileges
static bool EnablePrivilege(const wchar_t *name)
{
	bool result = false;
	HANDLE hToken {};
	if(::OpenProcessToken(::GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
	{
		LUID luid {};
		if(::LookupPrivilegeValueW(nullptr, name, &luid))
		{
			TOKEN_PRIVILEGES tp {};
			tp.PrivilegeCount = 1;
			tp.Privileges[0].Luid = luid;
			tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
			if(::AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), nullptr, nullptr))
				result = ::GetLastError() == ERROR_SUCCESS;
		}
		::CloseHandle(hToken);
	}
	return result;
}

/*
	Borrows one access right on one protected key, and gives it back.

	HKLM\SOFTWARE\Classes\CLSID\{86ca1aa0-34aa-4e8b-a509-50c905bae2a2} belongs to
	Windows: on a stock machine its owner is NT SERVICE\TrustedInstaller and
	Administrators hold ReadKey, so creating TreatAs under it fails with
	ERROR_ACCESS_DENIED even when elevated.

	The previous fallback took ownership and then granted GENERIC_ALL,
	CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE, to Administrators, SYSTEM *and
	BUILTIN\Users*, and never put anything back. That key decides which handler
	owns the Windows 11 Explorer context menu, so afterwards any unprivileged
	user could repoint TreatAs/InprocServer32 at a DLL of their own and have it
	loaded into every other user's Explorer, administrators included.

	This instead:
	  - snapshots the owner and DACL,
	  - takes ownership as BUILTIN\Administrators (SeTakeOwnershipPrivilege),
	  - grants Administrators exactly `temporary_rights`, NO_INHERITANCE,
	  - and restores the DACL and then the original owner afterwards, whether or
	    not the caller's operation succeeded. Handing ownership back to
	    TrustedInstaller, a SID we are not a member of, needs SeRestorePrivilege.

	The caller performs its operation by opening the key again after acquire():
	access is computed when a handle is opened, so a handle opened before the
	DACL was widened would not carry the borrowed right.

	  https://learn.microsoft.com/windows/win32/api/aclapi/nf-aclapi-setsecurityinfo
	  https://learn.microsoft.com/windows/win32/secauthz/privilege-constants
*/
class BorrowedKeyAccess
{
public:
	BorrowedKeyAccess() = default;
	BorrowedKeyAccess(const BorrowedKeyAccess &) = delete;
	BorrowedKeyAccess &operator=(const BorrowedKeyAccess &) = delete;

	~BorrowedKeyAccess()
	{
		// Best-effort retry for early-return paths. If restoration still fails
		// the snapshot is kept (and logged) rather than discarded.
		if(!restore())
			restore();
		if(_dacl_changed || _owner_taken)
			_log->error(L"BorrowedKeyAccess: could not restore owner/DACL (dacl=%d owner=%d)",
						_dacl_changed ? 1 : 0, _owner_taken ? 1 : 0);
	}

	bool acquire(HKEY root, const wchar_t *subkey, ACCESS_MASK temporary_rights,
				 REGSAM reg_view = KEY_WOW64_64KEY)
	{
		if(!restore())
			return false;

		if(!EnablePrivilege(SE_TAKE_OWNERSHIP_NAME) || !EnablePrivilege(SE_RESTORE_NAME))
			return false;

		// WRITE_OWNER is granted through SeTakeOwnershipPrivilege. This handle is
		// kept: it is what puts the original owner back.
		if(ERROR_SUCCESS != ::RegOpenKeyExW(root, subkey, 0,
											READ_CONTROL | WRITE_OWNER | reg_view, &_restore_key))
			return false;

		// Snapshot first. Everything below is undone from this.
		if(ERROR_SUCCESS != ::GetSecurityInfo(_restore_key, SE_REGISTRY_KEY,
											  OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
											  &_saved_owner, nullptr, &_saved_dacl, nullptr, &_saved))
		{
			restore();
			return false;
		}

		PSID admins {};
		SID_IDENTIFIER_AUTHORITY sia { SECURITY_NT_AUTHORITY };
		if(!::AllocateAndInitializeSid(&sia, 2, SECURITY_BUILTIN_DOMAIN_RID,
									   DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &admins))
		{
			restore();
			return false;
		}

		if(ERROR_SUCCESS != ::SetSecurityInfo(_restore_key, SE_REGISTRY_KEY, OWNER_SECURITY_INFORMATION,
											  admins, nullptr, nullptr, nullptr))
		{
			::FreeSid(admins);
			restore();
			return false;
		}
		_owner_taken = true;

		// As owner we implicitly hold READ_CONTROL | WRITE_DAC. Do not ask for
		// `temporary_rights` here: the DACL does not grant them yet.
		if(ERROR_SUCCESS != ::RegOpenKeyExW(root, subkey, 0,
											READ_CONTROL | WRITE_DAC | reg_view, &_work_key))
		{
			::FreeSid(admins);
			restore();
			return false;
		}

		// Exactly what the one operation needs, to one principal, inherited by
		// nothing. Never BUILTIN\Users.
		EXPLICIT_ACCESSW ea {};
		ea.grfAccessMode = GRANT_ACCESS;
		ea.grfAccessPermissions = temporary_rights;
		ea.grfInheritance = NO_INHERITANCE;
		ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
		ea.Trustee.TrusteeType = TRUSTEE_IS_GROUP;
		ea.Trustee.ptstrName = reinterpret_cast<wchar_t *>(admins);

		PACL widened {};
		bool ok = false;
		if(ERROR_SUCCESS == ::SetEntriesInAclW(1, &ea, _saved_dacl, &widened))
		{
			ok = ERROR_SUCCESS == ::SetSecurityInfo(_work_key, SE_REGISTRY_KEY,
													DACL_SECURITY_INFORMATION,
													nullptr, nullptr, widened, nullptr);
			_dacl_changed = ok;
			::LocalFree(widened);
		}

		::FreeSid(admins);
		if(!ok)
			restore();
		return ok;
	}

	// DACL first, then owner: giving ownership back first would cost us the
	// implicit WRITE_DAC the DACL restore depends on.
	bool restore()
	{
		if(_dacl_changed && _work_key)
		{
			if(ERROR_SUCCESS == ::SetSecurityInfo(_work_key, SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION,
												  nullptr, nullptr, _saved_dacl, nullptr))
				_dacl_changed = false;
		}

		if(_owner_taken && !_dacl_changed && _restore_key && _saved_owner)
		{
			if(ERROR_SUCCESS == ::SetSecurityInfo(_restore_key, SE_REGISTRY_KEY, OWNER_SECURITY_INFORMATION,
												  _saved_owner, nullptr, nullptr, nullptr))
				_owner_taken = false;
		}

		if(_dacl_changed || _owner_taken)
			return false;

		release();
		return true;
	}

	// The key itself was deleted: there is no persistent security descriptor
	// left to restore, and closing the handles completes the deletion.
	void release_deleted()
	{
		_dacl_changed = false;
		_owner_taken = false;
		release();
	}

private:
	void release()
	{
		if(_work_key) { ::RegCloseKey(_work_key); _work_key = nullptr; }
		if(_restore_key) { ::RegCloseKey(_restore_key); _restore_key = nullptr; }
		if(_saved) { ::LocalFree(_saved); _saved = nullptr; }
		_saved_owner = nullptr;
		_saved_dacl = nullptr;
	}

	HKEY _restore_key = nullptr;             // READ_CONTROL | WRITE_OWNER
	HKEY _work_key = nullptr;                // READ_CONTROL | WRITE_DAC
	PSECURITY_DESCRIPTOR _saved = nullptr;   // one allocation backing both below
	PSID _saved_owner = nullptr;
	PACL _saved_dacl = nullptr;
	bool _owner_taken = false;
	bool _dacl_changed = false;
};

// Machine registration addresses HKLM explicitly rather than the merged HKCR
// view, which can resolve per-user for the calling context.
//   https://learn.microsoft.com/windows/win32/sysinfo/hkey-classes-root-key
static constexpr wchar_t TreatAsParent[] = L"SOFTWARE\\Classes\\CLSID\\" CLS_FileExplorerContextMenu;
static constexpr wchar_t TreatAsKey[] = L"SOFTWARE\\Classes\\CLSID\\" CLS_FileExplorerContextMenu L"\\TreatAs";

enum class TreatAsState { absent, ours, foreign, inaccessible };

// Defaults to the machine redirect; the per-user one is the same key under HKCU.
static TreatAsState QueryTreatAs(HKEY root = HKEY_LOCAL_MACHINE, const wchar_t *treat_as_key = TreatAsKey)
{
	auto_regkey key {};
	auto rc = ::RegOpenKeyExW(root, treat_as_key, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, key);
	if(rc == ERROR_FILE_NOT_FOUND || rc == ERROR_PATH_NOT_FOUND)
		return TreatAsState::absent;
	if(rc != ERROR_SUCCESS)
		return TreatAsState::inaccessible;

	DWORD subkeys = 0, values = 0;
	if(ERROR_SUCCESS != ::RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &subkeys, nullptr,
										   nullptr, &values, nullptr, nullptr, nullptr, nullptr))
		return TreatAsState::inaccessible;

	// RegGetValueW terminates the string even if it was stored without one,
	// which older builds of this code did.
	wchar_t value[64] {};
	DWORD bytes = sizeof(value);
	rc = ::RegGetValueW(key, nullptr, nullptr, RRF_RT_REG_SZ, nullptr, value, &bytes);
	if(rc == ERROR_FILE_NOT_FOUND || rc == ERROR_MORE_DATA || rc == ERROR_UNSUPPORTED_TYPE)
		return TreatAsState::foreign;
	if(rc != ERROR_SUCCESS)
		return TreatAsState::inaccessible;

	if(subkeys == 0 && values == 1 &&
	   ::CompareStringOrdinal(value, -1, CLS_ContextMenu, -1, TRUE) == CSTR_EQUAL)
		return TreatAsState::ours;
	return TreatAsState::foreign;
}

static LSTATUS CreateTreatAs()
{
	auto_regkey parent {};
	auto rc = ::RegOpenKeyExW(HKEY_LOCAL_MACHINE, TreatAsParent, 0, KEY_CREATE_SUB_KEY | KEY_WOW64_64KEY, parent);
	if(rc != ERROR_SUCCESS)
		return rc;

	auto_regkey key {};
	DWORD disposition = 0;
	rc = ::RegCreateKeyExW(parent, L"TreatAs", 0, nullptr, REG_OPTION_NON_VOLATILE,
						   KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, key, &disposition);
	if(rc != ERROR_SUCCESS)
		return rc;

	// Someone else's redirect: leave it alone.
	if(disposition != REG_CREATED_NEW_KEY)
		return QueryTreatAs() == TreatAsState::ours ? ERROR_SUCCESS : ERROR_ALREADY_EXISTS;

	// sizeof includes the terminator.
	rc = ::RegSetValueExW(key, nullptr, 0, REG_SZ,
						  reinterpret_cast<const BYTE *>(CLS_ContextMenu), sizeof(CLS_ContextMenu));
	if(rc != ERROR_SUCCESS)
	{
		key.close();
		::RegDeleteKeyExW(HKEY_LOCAL_MACHINE, TreatAsKey, KEY_WOW64_64KEY, 0);
	}
	return rc;
}

static LSTATUS RemoveTreatAsIfOurs()
{
	switch(QueryTreatAs())
	{
		case TreatAsState::absent:
			return ERROR_SUCCESS;
		case TreatAsState::ours:
			return ::RegDeleteKeyExW(HKEY_LOCAL_MACHINE, TreatAsKey, KEY_WOW64_64KEY, 0);
		case TreatAsState::foreign:
			return ERROR_ALREADY_EXISTS;
		default:
			return ERROR_ACCESS_DENIED;
	}
}

// Adds or removes the TreatAs redirect that makes Shell the Windows 11 primary
// context menu. Only a redirect pointing at our CLSID is ever removed.
static bool disable_modern(bool _register)
{
	auto state = QueryTreatAs();

	if(_register)
	{
		if(state == TreatAsState::ours)
			return true;
		if(state != TreatAsState::absent)
		{
			_log->warning(L"TreatAs redirect exists but is not ours (or is unreadable); left unchanged.");
			return false;
		}

		auto rc = CreateTreatAs();
		if(rc != ERROR_ACCESS_DENIED)
			return rc == ERROR_SUCCESS;

		BorrowedKeyAccess borrowed;
		if(!borrowed.acquire(HKEY_LOCAL_MACHINE, TreatAsParent, KEY_CREATE_SUB_KEY))
			return false;

		rc = CreateTreatAs();
		const bool restored = borrowed.restore();
		return restored && rc == ERROR_SUCCESS;
	}

	if(state == TreatAsState::absent || state == TreatAsState::foreign)
		return true;

	auto rc = RemoveTreatAsIfOurs();
	if(rc != ERROR_ACCESS_DENIED)
		return rc == ERROR_SUCCESS;

	// TreatAs created under a restored parent inherits Administrators ReadKey
	// only, so deleting it needs DELETE borrowed on the key itself.
	BorrowedKeyAccess borrowed;
	if(!borrowed.acquire(HKEY_LOCAL_MACHINE, TreatAsKey, DELETE))
		return false;

	rc = RemoveTreatAsIfOurs();
	if(rc == ERROR_SUCCESS)
	{
		borrowed.release_deleted();
		return true;
	}
	borrowed.restore();
	return false;
}

/*
	Per-user Windows 11 redirect, written under HKCU only (no elevation).

	Explorer builds the Windows 11 menu with CoCreateInstance({86ca1aa0-...}).
	For a medium-integrity caller such as Explorer, COM resolves a CLSID through
	the merged HKEY_CLASSES_ROOT view, where a subkey of
	HKCU\Software\Classes\CLSID\{x} shadows the same subkey under HKLM. The
	widely used "classic context menu" tweak depends on exactly that: an empty
	HKCU\...\{86ca1aa0-...}\InprocServer32 hides the system server, activation
	fails and Explorer falls back to the classic menu. That HKCU key belongs to
	the user, so unlike the HKLM one (owned by TrustedInstaller, see
	BorrowedKeyAccess) there is no ACL to work around.

	Two things are written there:
	  TreatAs = {BAE3934B-...F1}
	      The redirect machine mode writes under HKLM, resolved per user. COM
	      looks TreatAs up before the server, so activation goes to our CLSID
	      (registered in HKCU by -register -user), which loads shell.dll; its
	      DllGetClassObject declines the request and Explorer shows the classic
	      menu, which Shell takes over.
	  InprocServer32 = ""   (only if the user has no such key already)
	      The classic-menu tweak itself, as a fallback in case a COM build
	      ignored a per-user TreatAs: activation then simply fails, Explorer
	      falls back to the classic menu and loads shell.dll through its
	      ContextMenuHandlers entry.
	Either way the result is the classic menu with shell.dll loaded. The marker
	value records that the empty InprocServer32 is ours, so unregistering
	removes exactly what was added and leaves a user's own tweak alone.
*/
static constexpr wchar_t UserTreatAsParent[] = L"Software\\Classes\\CLSID\\" CLS_FileExplorerContextMenu;
static constexpr wchar_t UserTreatAsKey[] = L"Software\\Classes\\CLSID\\" CLS_FileExplorerContextMenu L"\\TreatAs";
static constexpr wchar_t UserTreatAsInproc[] = L"Software\\Classes\\CLSID\\" CLS_FileExplorerContextMenu L"\\InprocServer32";
static constexpr wchar_t UserTreatAsMarker[] = L"NilesoftShell.InprocServer32";

static TreatAsState QueryUserTreatAs()
{
	return QueryTreatAs(HKEY_CURRENT_USER, UserTreatAsKey);
}

static bool user_treat(bool _register)
{
	const HKEY hkcu = HKEY_CURRENT_USER;
	auto state = QueryUserTreatAs();

	if(_register)
	{
		if(state == TreatAsState::foreign || state == TreatAsState::inaccessible)
		{
			_log->warning(L"Per-user TreatAs redirect exists but is not ours (or is unreadable); left unchanged.");
			return false;
		}

		if(state == TreatAsState::absent && !RegistryConfig::SetString(hkcu, UserTreatAsKey, nullptr, CLS_ContextMenu))
			return false;

		if(!RegistryConfig::KeyExists(hkcu, UserTreatAsInproc) &&
		   RegistryConfig::SetString(hkcu, UserTreatAsInproc, nullptr, L""))
		{
			RegistryConfig::SetString(hkcu, UserTreatAsParent, UserTreatAsMarker, L"1");
		}
		return true;
	}

	bool ok = true;
	if(state == TreatAsState::ours)
		ok = RegistryConfig::DeleteTree(hkcu, UserTreatAsKey);
	else if(state == TreatAsState::inaccessible)
		ok = false;

	string marker;
	if(RegistryConfig::GetString(hkcu, UserTreatAsParent, UserTreatAsMarker, marker))
	{
		// Ours, and still exactly as written: one empty default value.
		string server;
		if(RegistryConfig::GetString(hkcu, UserTreatAsInproc, nullptr, server) && server.empty())
		{
			RegistryConfig::DeleteValue(hkcu, UserTreatAsInproc, nullptr);
			RegistryConfig::DeleteKeyIfEmpty(hkcu, UserTreatAsInproc);
		}
		RegistryConfig::DeleteValue(hkcu, UserTreatAsParent, UserTreatAsMarker);
	}

	// Only if nothing else (e.g. a user's own tweak) is left in it.
	RegistryConfig::DeleteKeyIfEmpty(hkcu, UserTreatAsParent);
	return ok;
}

// -register -user / -unregister -user. Writes below HKEY_CURRENT_USER only, so
// it works without elevation and never asks for it. On failure `msg` holds
// the text to show, or is empty when there was nothing to unregister.
static bool RegistrationUser(const REGOP &reg, const string &dll_path, string &msg)
{
	auto ver = &Windows::Version::Instance();

	if(reg.REGISTER)
	{
		if(!RegistryConfig::RegisterUser(dll_path))
		{
			msg = string::Extract(IDS_REGISTER_NOT_SUCCESS).move();
			return false;
		}

		_log->info(L"Registered for the current user (HKCU\\Software\\Classes). "
				   L"The icon overlay and folder extensions can only be registered "
				   L"machine-wide (HKLM) and were skipped.");

		if(RegistryConfig::IsRegistered(RegScope::Machine))
		{
			_log->warning(L"Shell is also registered for all users. For this user the HKCU "
						  L"registration ('%s') shadows the machine one ('%s').",
						  dll_path.c_str(), RegistryConfig::InprocServerPath(RegScope::Machine).c_str());
		}

		if(ver->IsWindows11OrGreater() && reg.TREAT && !user_treat(true))
			_log->warning(L"Could not set the per-user Windows 11 TreatAs redirect.");

		msg = string::Extract(IDS_REGISTER_SUCCESS).move();
		return true;
	}

	// Always remove our per-user redirect, with or without -treat, as machine
	// mode does: one left behind would point the Windows 11 menu at a CLSID
	// this user no longer has. Only what user_treat(true) wrote is removed.
	if(!user_treat(false))
		_log->warning(L"Could not remove the per-user Windows 11 TreatAs redirect.");

	if(!RegistryConfig::IsRegistered(RegScope::User))
		return false;

	if(!RegistryConfig::UnregisterUser())
	{
		msg = string::Extract(IDS_UNREGISTER_NOT_SUCCESS).move();
		return false;
	}

	msg = string::Extract(IDS_UNREGISTER_SUCCESS).move();
	return true;
}

//printf("Please wait shell we'll process your command \n");
// r		register
// u		unregister
// s		silent
// restart	restart explorer
// shx	cmh integrated Shell Extensions ContextMenuHandlers
// Register the COM server and the context menu handler.
bool Registration(REGOP reg)
{
	auto ver = &Windows::Version::Instance();

	try
	{
		string dir = IO::Path::Parent(IO::Path::Module(_hInstance));
		string dll_path = IO::Path::Combine(dir, dll_name).move();

		string path = IO::Path::Module(_hInstance).move();
		//string dir = IO::Path::Parent(path).move();

		if(reg.REGISTER)
		{
			// Registration used to call Permission::SetFile(dir) here, i.e. try to
			// grant BUILTIN\Users GENERIC_ALL (inheritable) on the install folder.
			// That folder holds shell.dll, which Explorer loads into every
			// session, so a writable folder is a privilege escalation. It only
			// ever failed because SetFile opens without
			// FILE_FLAG_BACKUP_SEMANTICS and cannot open a directory. Removed,
			// not repaired.
			_log->close();
			//logger->reset();
			//IO::Path::Delete(logger->path());
		}

		if(!ver->IsWindows7OrGreater())
		{
			//windows compatibility
			_log->error(string::Extract(IDS_WINDOWS_COMPATIBILITY));
			return false;
		}

		string msg;

		if(reg.REGISTER || reg.UNREGISTER)
		{
			if(reg.USER)
			{
				// Per-user: HKCU only, no elevation needed or requested.
				if(!RegistrationUser(reg, dll_path, msg))
				{
					if(!msg.empty())
					{
						_log->error(msg);
						if(!reg.SILENT)
							MessageBox::Show(msg, APP_FULLNAME, MessageBoxIcon::Error, MessageBoxButtons::OK);
					}
					return false;
				}
			}
			else if(!is_elevated)
			{
				// Missing administrative privileges!
				msg = string::Extract(IDS_ADMIN_PRIVILEGES).move();
				_log->warning(msg);
				if(!reg.SILENT)
				{
					//You will need to provide administrator permission to run this Shell
					MessageBox::Show(msg, APP_FULLNAME, MessageBoxIcon::Warning);
				}
				return false;
			}
			else if(reg.REGISTER)
			{
				//logger->create();

				// Zero-initialised: FOLDEREXTENSIONS used to be left indeterminate
				// here, so whether {...BAF3} was (re)written depended on the stack.
				REGOP regop{};
				regop.CONTEXTMENU = regop.ICONOVERLAY = true;

				if(!RegistryConfig::Register(dll_path, regop))
				{
					msg = string::Extract(IDS_REGISTER_NOT_SUCCESS).move();
					_log->error(msg);
					if(!reg.SILENT)
					{
						MessageBox::Show(msg, APP_FULLNAME,
										 MessageBoxIcon::Error, MessageBoxButtons::OK);
					}
					return false;
				}
				
// HKEY_CLASSES_ROOT\LibraryFolder\ShellEx\ContextMenuHandlers
// HKEY_CLASSES_ROOT\LibraryFolder\background\shellex\ContextMenuHandlers
				// is windows 11 or later
				if(ver->IsWindows11OrGreater() && reg.TREAT)
				{
					//SOFTWARE\\Classes\\CLSID"
					if(!disable_modern(true))
						_log->warning(L"Could not set the Windows 11 TreatAs redirect.");
				}

				if(RegistryConfig::IsRegistered(RegScope::User))
				{
					_log->warning(L"This user also has a per-user (HKCU) registration ('%s'); "
								  L"for this user it shadows the machine one.",
								  RegistryConfig::InprocServerPath(RegScope::User).c_str());
				}

				msg = string::Extract(IDS_REGISTER_SUCCESS).move();
			}
			else
			{
				// Always remove our TreatAs redirect on unregister, with or without
				// -treat: a redirect left pointing at a CLSID that is no longer
				// registered breaks the Windows 11 context menu. disable_modern
				// only deletes it when it points at our CLSID, and a missing key
				// is a no-op.
				auto remove_treatas = [&]()
				{
					if(ver->IsWindows11OrGreater() && !disable_modern(false))
						_log->warning(L"Could not remove the Windows 11 TreatAs redirect.");
				};

				if(!RegistryConfig::IsRegistered(RegScope::Machine))
				{
					// Clean up a stale redirect left behind by an earlier
					// uninstall, but still report "not registered".
					remove_treatas();
					return false;
				}

				if(!RegistryConfig::Unregister())
				{
					msg = string::Extract(IDS_UNREGISTER_NOT_SUCCESS).move();
					_log->error(msg);
					if(!reg.SILENT)
					{
						MessageBox::Show(msg, APP_FULLNAME,
										 MessageBoxIcon::Error, MessageBoxButtons::OK);
					}
					return false;
				}

				remove_treatas();

				msg = string::Extract(IDS_UNREGISTER_SUCCESS).move();
			}

			if(ver->IsWindows8OrGreater())
				_log->info(msg);

			if(reg.RESTART)
			{
				if(!reg.SILENT)
				{
					msg += L"\n\n";
					msg += string::Extract(IDS_RESTART_EXPLORERQ).move();
					reg.RESTART = MessageBox::Show(msg, APP_FULLNAME,
											   MessageBoxIcon::Information,
											   MessageBoxButtons::OKCancel) == DialogResult::OK;
				}
			}

			if(reg.RESTART)
			{
				if(ver->IsWindows8OrGreater())
					_log->info(string::Extract(IDS_RESTART_EXPLORER).move());

				if(Windows::Explorer::Restart())
					::Sleep(1000);
			}
			else
			{
				// https://docs.microsoft.com/en-us/windows/desktop/shell/reg-shell-exts#predefined-shell-objects
				// If you do not call SHChangeNotify, the change might not be recognized until the system is rebooted.
				//::SHChangeNotify(SHCNE_ASSOCCHANGED, 0, 0, 0);
				//Windows::Explorer::Refresh();
			}
			return true;
		}
		else if(reg.RESTART)
		{
			if(ver->IsWindows8OrGreater())
				_log->info(string::Extract(IDS_RESTART_EXPLORER).move());
			return Windows::Explorer::Restart();
		}
	}
	catch(...)
	{
#ifdef _DEBUG
		logger->exception(__func__);
#endif
	}
	return false;
}

bool Register(REGOP reg, HWND hwnd = nullptr)
{
	string path = IO::Path::Combine(IO::Path::Parent(IO::Path::Module(nullptr)), dll_name).move();
	DLL dll(path, true);

	// Per-user registration (-user) needs no elevation.
	if((reg.REGISTER || reg.UNREGISTER) && !reg.USER)
	{
		if(!is_elevated)
		{
			// Missing administrative privileges!
			string msg = loadstring(IDS_ADMIN_PRIVILEGES, dll).move();
			_log->error(msg);
			if(!reg.SILENT)
			{
				//You will need to provide administrator permission to run this Shell
				::MessageBoxW(hwnd, msg, APP_FULLNAME, MB_ICONWARNING);
			}
			return false;
		}
	}
	
	if(!dll)
	{
		auto ernf = L"shell.dll not found.";
		_log->error(ernf);
		if(!reg.SILENT)
			MessageBox::Show(ernf, APP_FULLNAME, MessageBoxIcon::Warning);
		return false;
	}

	auto ret = Registration(reg);
	_log->close();

	return ret;
}

// Process exit codes of the non-interactive commands (-config:*, -status).
enum CliExit : int { cli_ok = 0, cli_failed = 1, cli_usage = 2 };

/*
	Text output for the non-interactive commands.

	shell.exe is a GUI-subsystem program and has no console of its own, so:
	  - with -out:<path>, the text is written to that file (UTF-8, replaced);
	  - else, if stdout is a pipe or a file, it is written there directly as
	    UTF-8. That is the case for a parent that redirected the child's
	    stdout (a settings app reading it through a pipe) and for
	    `shell -status > status.json` in cmd;
	  - else it attaches to the parent's console, if any, and writes UTF-16
	    with WriteConsoleW. cmd.exe does not wait for GUI programs, so this
	    text can land after the next prompt; use a pipe, a redirection or
	    `start /wait` when that matters.
*/
class CliOutput
{
	HANDLE _handle = INVALID_HANDLE_VALUE;
	bool _owned = false;
	bool _console = false;
	bool _failed = false;

public:
	explicit CliOutput(const wchar_t *path)
	{
		if(path && *path)
		{
			_handle = ::CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
									CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			_owned = true;
			return;
		}

		auto out = ::GetStdHandle(STD_OUTPUT_HANDLE);
		if(out && out != INVALID_HANDLE_VALUE)
		{
			auto type = ::GetFileType(out);
			if(type == FILE_TYPE_PIPE || type == FILE_TYPE_DISK)
			{
				_handle = out;
				return;
			}
		}

		if(::AttachConsole(ATTACH_PARENT_PROCESS))
		{
			_handle = ::CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
									FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
			_owned = true;
			_console = true;
		}
	}

	CliOutput(const CliOutput &) = delete;
	CliOutput &operator=(const CliOutput &) = delete;

	~CliOutput()
	{
		if(_owned && valid())
			::CloseHandle(_handle);
		if(_console)
			::FreeConsole();
	}

	bool valid() const { return _handle && _handle != INVALID_HANDLE_VALUE; }

	// False if there was nowhere to write or a write failed.
	bool ok() const { return valid() && !_failed; }

	void write(const wchar_t *text)
	{
		if(!text || !*text)
			return;
		if(!valid())
		{
			_failed = true;
			return;
		}

		auto length = static_cast<int>(::wcslen(text));
		if(_console)
		{
			DWORD written = 0;
			if(!::WriteConsoleW(_handle, text, static_cast<DWORD>(length), &written, nullptr))
				_failed = true;
			return;
		}

		auto bytes = ::WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
		if(bytes <= 0)
		{
			_failed = true;
			return;
		}

		std::string utf8(static_cast<size_t>(bytes), '\0');
		::WideCharToMultiByte(CP_UTF8, 0, text, length, utf8.data(), bytes, nullptr, nullptr);

		for(size_t offset = 0; offset < utf8.size();)
		{
			DWORD written = 0;
			if(!::WriteFile(_handle, utf8.data() + offset, static_cast<DWORD>(utf8.size() - offset), &written, nullptr) || !written)
			{
				_failed = true;
				return;
			}
			offset += written;
		}
	}

	void line(const wchar_t *text = nullptr)
	{
		write(text);
		write(L"\r\n");
	}
};

// Appends `text` as a JSON string literal, quotes included. Output is
// UTF-16 here and converted to UTF-8 on write, so only what JSON requires
// is escaped: '"', '\', control characters, and unpaired surrogates
// (which have no UTF-8 form, so they are kept as \u escapes).
static void json_quote(string &json, const wchar_t *text)
{
	json += L'"';
	for(auto ch = text ? text : L""; *ch; ch++)
	{
		auto c = *ch;
		switch(c)
		{
			case L'"': json += L"\\\""; continue;
			case L'\\': json += L"\\\\"; continue;
			case L'\b': json += L"\\b"; continue;
			case L'\f': json += L"\\f"; continue;
			case L'\n': json += L"\\n"; continue;
			case L'\r': json += L"\\r"; continue;
			case L'\t': json += L"\\t"; continue;
		}

		bool lone = false;
		if(IS_HIGH_SURROGATE(c))
			lone = !IS_LOW_SURROGATE(ch[1]);
		else if(IS_LOW_SURROGATE(c))
			lone = true; // a valid low surrogate is consumed with its high one below

		if(c < 0x20 || lone)
		{
			wchar_t escaped[8]{};
			::swprintf_s(escaped, L"\\u%04x", static_cast<unsigned>(c));
			json += escaped;
		}
		else if(IS_HIGH_SURROGATE(c))
		{
			json += c;
			json += *++ch;
		}
		else
		{
			json += c;
		}
	}
	json += L'"';
}

static void json_quote_or_null(string &json, const string &text)
{
	if(text.empty())
		json += L"null";
	else
		json_quote(json, text);
}

static const wchar_t *json_bool(bool value)
{
	return value ? L"true" : L"false";
}

static string InstallDirectory()
{
	return IO::Path::Parent(IO::Path::Module(nullptr)).move();
}

/*
	-config:init, -config:reset, -config:path.

	None of them needs elevation: they only read the install folder and only
	write below %APPDATA%\Nilesoft\Shell (see ConfigLocation). shell.dll never
	writes there itself; it only picks the per-user shell.nss up once it
	exists, and reloads on its own when it changes.
*/
namespace ConfigCommand
{
	struct CopyStats
	{
		uint32_t copied{}, skipped{}, failed{};
	};

	static bool is_directory(const wchar_t *path)
	{
		auto attr = ::GetFileAttributesW(path);
		return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
	}

	static bool exists(const wchar_t *path)
	{
		return ::GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
	}

	// Copies one file. An existing target is kept unless `overwrite`. The copy
	// is left writable: these are files the user is meant to edit.
	static void copy_file(const string &src, const string &dst, bool overwrite, CopyStats &stats)
	{
		auto attr = ::GetFileAttributesW(dst);
		if(attr != INVALID_FILE_ATTRIBUTES)
		{
			if(!overwrite || (attr & FILE_ATTRIBUTE_DIRECTORY))
			{
				stats.skipped++;
				return;
			}
			if(attr & FILE_ATTRIBUTE_READONLY)
				::SetFileAttributesW(dst, attr & ~FILE_ATTRIBUTE_READONLY);
		}

		if(!::CopyFileW(src, dst, FALSE))
		{
			_log->error(L"config: cannot copy '%s' to '%s' (error %lu)", src.c_str(), dst.c_str(), ::GetLastError());
			stats.failed++;
			return;
		}

		attr = ::GetFileAttributesW(dst);
		if(attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_READONLY))
			::SetFileAttributesW(dst, attr & ~FILE_ATTRIBUTE_READONLY);
		stats.copied++;
	}

	// Copies a folder tree. Reparse points (junctions, symbolic links) are
	// skipped rather than followed. With `skip_backups`, top-level and nested
	// folders named backup-* are left out, so a backup never contains the
	// earlier ones (or itself).
	static void copy_tree(const string &src, const string &dst, bool overwrite, CopyStats &stats, bool skip_backups = false)
	{
		if(!::CreateDirectoryW(dst, nullptr) && ::GetLastError() != ERROR_ALREADY_EXISTS)
		{
			_log->error(L"config: cannot create '%s' (error %lu)", dst.c_str(), ::GetLastError());
			stats.failed++;
			return;
		}

		WIN32_FIND_DATAW fd{};
		auto find = ::FindFirstFileExW(ConfigLocation::Join(src, L"*"), FindExInfoBasic, &fd,
									   FindExSearchNameMatch, nullptr, 0);
		if(find == INVALID_HANDLE_VALUE)
			return;

		do
		{
			string name = fd.cFileName;
			if(name.equals(L".") || name.equals(L".."))
				continue;
			if(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
				continue;

			auto from = ConfigLocation::Join(src, name).move();
			auto to = ConfigLocation::Join(dst, name).move();

			if(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			{
				if(skip_backups && name.starts_with(L"backup-"))
					continue;
				copy_tree(from, to, overwrite, stats, skip_backups);
			}
			else
			{
				copy_file(from, to, overwrite, stats);
			}
		} while(::FindNextFileW(find, &fd));

		::FindClose(find);
	}

	// <user dir>\backup-YYYYMMDD-HHMMSS, made unique if needed.
	static string backup_path(const string &user_dir)
	{
		SYSTEMTIME st{};
		::GetLocalTime(&st);
		string name;
		name.format(L"backup-%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

		auto path = ConfigLocation::Join(user_dir, name).move();
		for(int i = 2; exists(path) && i < 100; i++)
		{
			string unique;
			unique.format(L"%s-%d", name.c_str(), i);
			path = ConfigLocation::Join(user_dir, unique).move();
		}
		return path.move();
	}

	/*
		The localization block of the default shell.nss (src/bin/shell.nss),
		from its "// localization" line to its "// or import lang" line; keep
		the two the same (nsscore's tests compare them).

		An installer upgrade keeps a shell.nss the user changed, so an older one
		(no language import) can sit next to newer imports whose titles are
		loc.<name>. shell.dll shows those in English then; add_localization()
		gives the per-user copy this block so translations load too.
	*/
	static constexpr const char *localization_block[] = {
		"// localization",
		"// English is always loaded first so untranslated keys fall back to it, then",
		"// the best match for sys.lang (e.g. \"de-DE\") is loaded on top of it:",
		"//   exact locale (de-DE.nss) -> base language (de.nss) -> en.nss",
		"// path.exists() resolves relative paths against the host process's current",
		"// directory (not this file), so the path must be absolute: use app.dir.",
		"$loc_path=app.dir + '\\imports\\lang\\'",
		"$loc_lang=if(str.contains(sys.lang, '-'), str.left(sys.lang, str.find(sys.lang, '-')), sys.lang)",
		"// Norwegian: Windows reports nb-NO (Bokmal) or nn-NO (Nynorsk); the file is no.nss",
		"// Chinese: Hong Kong, Macao and zh-Hant-* read Traditional (zh-TW.nss); every other",
		"// zh-* (zh-SG, zh-Hans-*, plain zh) reads Simplified (zh-CN.nss)",
		"$loc_zh=if(str.contains(sys.lang, 'Hant') || (!str.contains(sys.lang, 'Hans') && (str.contains(sys.lang, '-HK') || str.contains(sys.lang, '-MO'))), 'zh-TW', 'zh-CN')",
		"$loc_base=if(loc_lang == 'nb' || loc_lang == 'nn', 'no', if(loc_lang == 'zh', loc_zh, loc_lang))",
		"import lang loc_path + \"en.nss\"",
		"import lang if(path.exists(loc_path + sys.lang + \".nss\"),",
		"               loc_path + sys.lang + \".nss\",",
		"               if(path.exists(loc_path + loc_base + \".nss\"),",
		"                  loc_path + loc_base + \".nss\",",
		"                  loc_path + \"en.nss\"))",
		"",
		"// or import lang 'imports/lang/en.nss'",
	};

	// shell.dll's ConfigCheck on `root`: false only if it ran and found errors.
	static bool parses(const string &root)
	{
		auto dll_path = IO::Path::Combine(InstallDirectory(), dll_name).move();
		// Kept loaded until the process exits, which is right after this.
		auto module = ::LoadLibraryExW(dll_path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
		auto proc = module ? reinterpret_cast<ConfigCheckProc>(::GetProcAddress(module, CONFIG_CHECK_EXPORT)) : nullptr;
		if(!proc)
			return true;
		ConfigCheckSink ignore = [](void *, BOOL, const wchar_t *, UINT, UINT, const wchar_t *) {};
		return proc(root, ignore, nullptr) != S_FALSE;
	}

	static std::string_view trim_left(std::string_view s)
	{
		while(!s.empty() && (s.front() == ' ' || s.front() == '\t'))
			s.remove_prefix(1);
		return s;
	}

	// `s` starts with the word `word` (ASCII, any case), followed by a blank,
	// a quote, '{' or the end of the line.
	static bool starts_with_word(std::string_view s, std::string_view word)
	{
		if(s.size() < word.size())
			return false;
		for(size_t i = 0; i < word.size(); i++)
		{
			if(::tolower(static_cast<unsigned char>(s[i])) != word[i])
				return false;
		}
		if(s.size() == word.size())
			return true;
		auto c = s[word.size()];
		return c == ' ' || c == '\t' || c == '\'' || c == '"' || c == '{' || c == '\r' || c == '\n';
	}

	// Reads a whole (small) file; false if it cannot.
	static bool read_bytes(const string &path, std::string &text)
	{
		text.clear();
		auto handle = ::CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
		if(handle == INVALID_HANDLE_VALUE)
			return false;
		bool ok = false;
		LARGE_INTEGER size{};
		if(::GetFileSizeEx(handle, &size) && size.QuadPart >= 0 && size.QuadPart < 16 * 1024 * 1024)
		{
			text.resize(static_cast<size_t>(size.QuadPart));
			DWORD read = 0;
			ok = text.empty() || (::ReadFile(handle, text.data(), static_cast<DWORD>(text.size()), &read, nullptr) && read == text.size());
		}
		::CloseHandle(handle);
		return ok;
	}

	static bool write_bytes(const string &path, std::string_view text)
	{
		auto handle = ::CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if(handle == INVALID_HANDLE_VALUE)
			return false;
		DWORD written = 0;
		bool ok = ::WriteFile(handle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
			written == text.size();
		::CloseHandle(handle);
		if(!ok)
			::DeleteFileW(path);
		return ok;
	}

	/*
		Gives `config` (the per-user shell.nss, just copied from the install
		folder or kept from before) the localization block of the default
		shell.nss when it has no language import of its own. That is an older
		shell.nss: the installer keeps one the user changed, and -config:init
		copies it next to the newer imports, whose titles are loc.<name>.
		shell.dll shows those in English anyway (Parser::resolve_loc); this
		makes them follow the Windows display language again.

		The block goes right after the top-level settings { } block (where the
		default has it), else before the first top-level import, else at the
		end, so the imports after it see the strings. Nothing else in the file
		changes: the user's own lines and imports stay as they are. The file as
		it was is kept as shell.nss.old (.old.2, ... if that exists), and the
		new one only replaces it if shell.dll loads it without errors. A file
		that is not UTF-8/ANSI is left alone. Returns true if it was changed.
	*/
	static bool add_localization(CliOutput &out, const string &config)
	{
		std::string text;
		if(!read_bytes(config, text))
			return false;

		// UTF-16 (BOM FF FE / FE FF, or NULs in the text): leave it.
		if(text.size() < 2 || text.find('\0') != text.npos ||
		   (static_cast<unsigned char>(text[0]) == 0xFF && static_cast<unsigned char>(text[1]) == 0xFE) ||
		   (static_cast<unsigned char>(text[0]) == 0xFE && static_cast<unsigned char>(text[1]) == 0xFF))
			return false;

		size_t start = text.starts_with("\xEF\xBB\xBF") ? 3 : 0;
		const char *eol = text.find("\r\n") != text.npos ? "\r\n" : "\n";

		size_t first_import = text.npos;	// start of the first top-level import line
		size_t settings_end = text.npos;	// just after the top-level settings { } block
		bool in_settings = false, settings_open = false;
		bool in_comment = false;
		int depth = 0;	// of { } and ( ), outside strings and comments
		for(size_t pos = start; pos < text.size();)
		{
			auto end = text.find('\n', pos);
			auto next = end == text.npos ? text.size() : end + 1;
			auto line = trim_left(std::string_view(text).substr(pos, next - pos));
			auto line_start = pos;
			pos = next;

			if(!in_comment && depth == 0)
			{
				// Already loads strings: import lang/loc ..., or a lang/loc { } block.
				if(starts_with_word(line, "import"))
				{
					auto rest = trim_left(line.substr(6));
					if(starts_with_word(rest, "lang") || starts_with_word(rest, "loc"))
						return false;
					if(first_import == text.npos)
						first_import = line_start;
				}
				else if(starts_with_word(line, "lang") || starts_with_word(line, "loc"))
					return false;
				else if(settings_end == text.npos && !in_settings && starts_with_word(line, "settings"))
					in_settings = true;
			}

			// Track comments, strings and nesting through the line.
			char quote = 0;
			for(size_t i = 0; i < line.size(); i++)
			{
				auto c = line[i];
				auto n = i + 1 < line.size() ? line[i + 1] : '\0';
				if(in_comment)
				{
					if(c == '*' && n == '/')
					{
						in_comment = false;
						i++;
					}
				}
				else if(quote)
				{
					if(c == quote)
						quote = 0;
				}
				else if(c == '/' && n == '/')
					break;
				else if(c == '/' && n == '*')
				{
					in_comment = true;
					i++;
				}
				else if(c == '\'' || c == '"' || c == '`')
					quote = c;
				else if(c == '{' || c == '(')
				{
					depth++;
					settings_open |= in_settings;
				}
				else if((c == '}' || c == ')') && depth > 0)
					depth--;
			}

			if(in_settings && settings_open && depth == 0)
			{
				settings_end = next;
				in_settings = false;
			}
		}

		// The file as it was is kept there (never overwriting an earlier one).
		auto backup = (config + L".old").move();
		for(int i = 2; exists(backup) && i < 100; i++)
			backup.format(L"%s.old.%d", config.c_str(), i);
		if(exists(backup))
			return false;

		std::string block = "// Added by shell -config:init; the file as it was is ";
		std::wstring_view backup_name(backup.c_str());
		backup_name.remove_prefix(backup_name.find_last_of(L"\\/") + 1);
		for(auto c : backup_name)
			block += c < 0x80 ? static_cast<char>(c) : '?';
		block += eol;
		for(auto line : localization_block)
		{
			block += line;
			block += eol;
		}

		std::string patched = text;
		if(settings_end != text.npos && (first_import == text.npos || settings_end <= first_import))
		{
			// settings { ... }\n + \n<block>, then whatever followed.
			if(settings_end == patched.size() && !patched.ends_with('\n'))
			{
				patched += eol;
				settings_end = patched.size();
			}
			patched.insert(settings_end, eol + block);
		}
		else if(first_import != text.npos)
			patched.insert(first_import, block + eol);
		else
		{
			if(!patched.ends_with('\n'))
				patched += eol;
			patched += eol + block;
		}

		if(!write_bytes(backup, text))
		{
			_log->error(L"config: cannot keep a copy of '%s'; the localization block was not added", config.c_str());
			return false;
		}

		// Next to the original, so its relative imports resolve the same way.
		auto temp = (config + L".tmp").move();
		if(!write_bytes(temp, patched))
		{
			::DeleteFileW(backup);
			_log->error(L"config: cannot write '%s' (error %lu)", temp.c_str(), ::GetLastError());
			return false;
		}

		if(!parses(temp))
		{
			::DeleteFileW(temp);
			::DeleteFileW(backup);
			_log->warning(L"config: '%s' does not load with the localization block added; left as it was", config.c_str());
			return false;
		}

		if(!::MoveFileExW(temp, config, MOVEFILE_REPLACE_EXISTING))
		{
			auto error = ::GetLastError();
			::DeleteFileW(temp);
			::DeleteFileW(backup);
			_log->error(L"config: cannot add the localization block to '%s' (error %lu)", config.c_str(), error);
			return false;
		}

		string msg;
		msg.format(L"'%s' loaded no language file: added the localization block of the default shell.nss "
				   L"(its own lines and imports are unchanged; the file as it was is '%s').",
				   config.c_str(), backup.c_str());
		out.line(msg);
		return true;
	}

	// Copies <install>\shell.nss and <install>\imports\ into the per-user folder.
	static int seed(CliOutput &out, const string &user_dir, bool overwrite)
	{
		auto install_dir = InstallDirectory().move();
		auto src_config = ConfigLocation::Join(install_dir, ConfigLocation::FileName).move();
		if(!ConfigLocation::IsFile(src_config))
		{
			string msg;
			msg.format(L"error: '%s' not found.", src_config.c_str());
			out.line(msg);
			return cli_failed;
		}

		auto rc = ::SHCreateDirectoryExW(nullptr, user_dir, nullptr);
		if(rc != ERROR_SUCCESS && rc != ERROR_ALREADY_EXISTS && rc != ERROR_FILE_EXISTS)
		{
			string msg;
			msg.format(L"error: cannot create '%s' (error %d).", user_dir.c_str(), rc);
			out.line(msg);
			return cli_failed;
		}

		CopyStats stats;
		auto dst_config = ConfigLocation::Join(user_dir, ConfigLocation::FileName).move();
		copy_file(src_config, dst_config, overwrite, stats);

		auto src_imports = ConfigLocation::Join(install_dir, ConfigLocation::ImportsName).move();
		if(is_directory(src_imports))
			copy_tree(src_imports, ConfigLocation::Join(user_dir, ConfigLocation::ImportsName), overwrite, stats);

		// Copied or kept: an older shell.nss next to these imports gets their
		// strings, in the Windows display language (see add_localization).
		if(ConfigLocation::IsFile(dst_config))
			add_localization(out, dst_config);

		string msg;
		msg.format(L"%s: %u copied, %u kept (already present), %u failed.",
				   user_dir.c_str(), stats.copied, stats.skipped, stats.failed);
		out.line(msg);
		if(stats.skipped && !overwrite)
			out.line(L"Existing files were kept; use -force to overwrite them.");
		return stats.failed ? cli_failed : cli_ok;
	}

	static int init(CliOutput &out, bool force)
	{
		auto user_dir = ConfigLocation::UserDirectory().move();
		if(user_dir.empty())
		{
			out.line(L"error: the per-user folder (%APPDATA%) cannot be determined.");
			return cli_failed;
		}
		return seed(out, user_dir, force);
	}

	static int reset(CliOutput &out)
	{
		auto user_dir = ConfigLocation::UserDirectory().move();
		if(user_dir.empty())
		{
			out.line(L"error: the per-user folder (%APPDATA%) cannot be determined.");
			return cli_failed;
		}

		if(is_directory(user_dir))
		{
			// Everything currently in the folder (earlier backups excepted) is
			// copied aside first; nothing is overwritten unless that worked.
			auto backup = backup_path(user_dir).move();
			CopyStats stats;
			copy_tree(user_dir, backup, false, stats, true);
			if(stats.failed)
			{
				string msg;
				msg.format(L"error: backup to '%s' failed (%u files); nothing was reset.", backup.c_str(), stats.failed);
				out.line(msg);
				return cli_failed;
			}

			if(stats.copied)
			{
				string msg;
				msg.format(L"Backed up %u files to '%s'.", stats.copied, backup.c_str());
				out.line(msg);
			}
			else
			{
				// Nothing to keep; do not leave an empty backup folder behind.
				::RemoveDirectoryW(backup);
			}
		}

		return seed(out, user_dir, true);
	}

	static int path(CliOutput &out)
	{
		auto location = ConfigLocation::Resolve(InstallDirectory());
		if(location.source == ConfigLocation::Source::none)
		{
			string msg;
			msg.format(L"error: no config file found ('%s' does not exist).", location.path.c_str());
			out.line(msg);
			return cli_failed;
		}
		out.line(location.path);
		return cli_ok;
	}

	/*
		-config:check <root.nss>

		Parses <root.nss> and its imports with the real parser, which lives in
		shell.dll: the DLL next to this exe is loaded and its ConfigCheck
		export called (see ConfigCheck.h). It parses into a private cache, so
		no Explorer state is involved, and writes nothing to the registry.
		Prints {"ok":bool,"root":"...","errors":[...],"warnings":[...]},
		each entry {"file","line","column","message"}. Exit code 0 if ok,
		1 if there are errors (or the check could not run).
	*/
	struct CheckResult
	{
		string errors, warnings;
		string titles;
		uint32_t empty_titles{};
	};

	static void CALLBACK check_sink(void *context, BOOL is_error, const wchar_t *file,
									UINT line, UINT column, const wchar_t *message)
	{
		auto result = static_cast<CheckResult *>(context);
		auto &list = is_error ? result->errors : result->warnings;
		list += list.empty() ? L"\r\n    " : L",\r\n    ";
		list += L"{ \"file\": ";
		json_quote(list, file);
		string numbers;
		numbers.format(L", \"line\": %u, \"column\": %u, \"message\": ", line, column);
		list += numbers;
		json_quote(list, message);
		list += L" }";
	}

	/*
		With -titles[:<where>], also "where", "empty_titles": <count> and
		"titles": [...], every menu/item title in menu order (only those whose
		type= shows them in <where>: file, folder, back, desktop or taskbar),
		each {"kind","depth","state","title","source","file","line","column"}:
		kind is "menu", "item" or "modify"; depth 0 is the top level; state is
		"text", "empty" (a blank menu entry) or "dynamic" (depends on the
		selection, not evaluated); source is "loc.<name>"/"title.<name>" or "".
		Each "empty" title is also a warning. See ConfigCheck.h.
	*/
	static void CALLBACK title_sink(void *context, const CONFIG_TITLE *t)
	{
		auto result = static_cast<CheckResult *>(context);
		auto &list = result->titles;
		list += list.empty() ? L"\r\n    " : L",\r\n    ";
		string head;
		head.format(L"{ \"kind\": \"%s\", \"depth\": %u, \"state\": \"%s\", \"title\": ",
					t->kind == CONFIG_TITLE_MENU ? L"menu" : t->kind == CONFIG_TITLE_ITEM ? L"item" : L"modify",
					t->depth,
					t->state == CONFIG_TITLE_TEXT ? L"text" : t->state == CONFIG_TITLE_EMPTY ? L"empty" : L"dynamic");
		list += head;
		json_quote(list, t->title);
		list += L", \"source\": ";
		json_quote(list, t->source);
		list += L", \"file\": ";
		json_quote(list, t->file);
		string numbers;
		numbers.format(L", \"line\": %u, \"column\": %u }", t->line, t->column);
		list += numbers;
		if(t->state == CONFIG_TITLE_EMPTY)
		{
			result->empty_titles++;
			string message = L"this menu title is empty";
			if(t->source && *t->source)
				message.append_format(L" (%s)", t->source);
			check_sink(context, FALSE, t->file, t->line, t->column, message);
		}
	}

	// The <where> of -titles:<where>, as ConfigCheckTitles takes it.
	static bool valid_where(const string &where)
	{
		if(where.empty())
			return true;
		for(auto name : { L"file", L"folder", L"dir", L"back", L"background", L"desktop", L"taskbar" })
		{
			if(where.equals(name, true))
				return true;
		}
		return false;
	}

	static int check(CliOutput &out, const string &root_arg, bool titles, const string &where)
	{
		CheckResult result;
		HRESULT hr = E_FAIL;

		string root = root_arg;
		if(wchar_t full[MAX_PATH * 4]{}; ::GetFullPathNameW(root_arg, ARRAYSIZE(full), full, nullptr))
		{
			root = full;
			// Expand 8.3 names, so reported paths match what the caller uses.
			if(wchar_t long_path[MAX_PATH * 4]{}; ::GetLongPathNameW(full, long_path, ARRAYSIZE(long_path)))
				root = long_path;
		}

		auto dll_path = IO::Path::Combine(InstallDirectory(), dll_name).move();
		// Kept loaded until the process exits, which is right after this.
		auto module = ::LoadLibraryExW(dll_path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
		auto proc = module ? reinterpret_cast<ConfigCheckProc>(::GetProcAddress(module, CONFIG_CHECK_EXPORT)) : nullptr;
		auto titles_proc = module && titles ?
			reinterpret_cast<ConfigCheckTitlesProc>(::GetProcAddress(module, CONFIG_CHECK_TITLES_EXPORT)) : nullptr;
		if(!proc || (titles && !titles_proc))
		{
			string msg;
			msg.format(L"cannot load the parser from '%s' (error %lu)", dll_path.c_str(), ::GetLastError());
			check_sink(&result, TRUE, dll_path, 0, 0, msg);
		}
		else
		{
			hr = titles ? titles_proc(root, where, check_sink, title_sink, &result) : proc(root, check_sink, &result);
			if(FAILED(hr))
			{
				string msg;
				msg.format(L"the check could not be run (0x%08lx)", static_cast<unsigned long>(hr));
				check_sink(&result, TRUE, root, 0, 0, msg);
			}
		}

		auto ok = hr == S_OK;
		string json = L"{\r\n  \"ok\": ";
		json += json_bool(ok);
		json += L",\r\n  \"root\": ";
		json_quote(json, root);
		json += L",\r\n  \"errors\": [";
		json += result.errors;
		json += result.errors.empty() ? L"]" : L"\r\n  ]";
		json += L",\r\n  \"warnings\": [";
		json += result.warnings;
		json += result.warnings.empty() ? L"]" : L"\r\n  ]";
		if(titles)
		{
			json += L",\r\n  \"where\": ";
			json_quote(json, where.empty() ? string(L"all") : where);
			string count;
			count.format(L",\r\n  \"empty_titles\": %u", result.empty_titles);
			json += count;
			json += L",\r\n  \"titles\": [";
			json += result.titles;
			json += result.titles.empty() ? L"]" : L"\r\n  ]";
		}
		json += L"\r\n}";
		out.line(json);
		return ok ? cli_ok : cli_failed;
	}

	static int run(const string &command, const string &root, bool force, bool titles, const string &where, const wchar_t *out_path)
	{
		CliOutput out(out_path);

		int ret;
		if(command.equals(L"check"))
		{
			if(root.empty() || !valid_where(where))
			{
				out.line(L"usage: shell -config:check <root.nss> [-titles[:file|folder|back|desktop|taskbar]] [-out:<file>]");
				return cli_usage;
			}
			ret = check(out, root, titles, where);
		}
		else if(command.equals(L"init"))
			ret = init(out, force);
		else if(command.equals(L"reset"))
			ret = reset(out);
		else if(command.equals(L"path"))
			ret = path(out);
		else
		{
			out.line(L"usage: shell -config:init [-force] | -config:reset | -config:path | -config:check <root.nss> [-titles[:<where>]] [-out:<file>]");
			return cli_usage;
		}

		// The result could not be reported: say so through the exit code.
		if(ret == cli_ok && !out.ok())
			ret = cli_failed;
		return ret;
	}
}

/*
	-status: one JSON object describing the installation, for the settings GUI.
	Only reads (registry, file system); needs no elevation.

	{
	  "version": "1.9.19",
	  "elevated": false,
	  "installDir": "C:\\Program Files\\Nilesoft Shell",
	  "dll": { "path": "...\\shell.dll", "exists": true },
	  "registration": {
	    "mode": "none" | "user" | "machine" | "both",
	    "machine": { "registered": bool, "dll": "<InprocServer32>" | null },
	    "user":    { "registered": bool, "dll": "<InprocServer32>" | null }
	  },
	  "win11": {
	    "applicable": bool,            // Windows 11 or later
	    "classicMenu": bool,           // the modern menu is redirected for this user
	    "machineTreatAs": state,       // HKLM\SOFTWARE\Classes\CLSID\{86ca1aa0-...}\TreatAs
	    "userTreatAs": state,          // the same under HKCU\Software\Classes
	    "userInprocServer32": bool,    // HKCU ...\{86ca1aa0-...}\InprocServer32 present
	    "userInprocServer32Ours": bool // ... and written by -register -user -treat
	  },                               // state: "absent" | "ours" | "foreign" | "inaccessible"
	  "config": {
	    "path": "...\\shell.nss",
	    "source": "registry" | "appdata" | "install" | "none",
	    "exists": bool,
	    "registryValue": "<HKCU config value, rooted>" | null
	  },
	  "userConfigDir": "%APPDATA%\\Nilesoft\\Shell" (expanded) | null,
	  "conflicts": [                   // our CLSIDs naming a DLL other than installDir\shell.dll
	    { "clsid": "{BAE3934B-...-3FC599A1BAF3}",
	      "role": "contextMenu" | "iconOverlay" | "folderExtensions",
	      "scope": "machine" | "user" | "machineWow64" | "userWow64",
	      "dll": "c:\\windows\\nilesoft\\shell.dll", "exists": bool }
	  ],
	  "loadedInExplorer": [            // Nilesoft shell.dll files mapped into explorer.exe
	    { "path": "...\\shell.dll", "ours": bool }
	  ],
	  "log": { "path": "<the shell.log shell.dll writes>", "exists": bool }
	}
*/
namespace StatusCommand
{
	struct ClassesStore
	{
		HKEY root;
		const wchar_t *classes;
		const wchar_t *scope;
	};

	// Every store Explorer (or a 32-bit process) could resolve our CLSIDs from.
	// const, not constexpr: HKEY_LOCAL_MACHINE is a reinterpret_cast.
	static const ClassesStore classes_stores[] =
	{
		{ HKEY_LOCAL_MACHINE, L"Software\\Classes", L"machine" },
		{ HKEY_CURRENT_USER, L"Software\\Classes", L"user" },
		{ HKEY_LOCAL_MACHINE, L"Software\\Classes\\WOW6432Node", L"machineWow64" },
		{ HKEY_CURRENT_USER, L"Software\\Classes\\WOW6432Node", L"userWow64" },
	};

	// "conflicts": registrations of our CLSIDs that name another shell.dll.
	// Each one makes Explorer load a second copy next to (or instead of) ours.
	static string conflicts(const string &dll_path)
	{
		string items;
		for(const auto &store : classes_stores)
		{
			for(const auto &role : RegistryConfig::Roles)
			{
				auto server = RegistryConfig::ServerPath(store.root, store.classes, role.clsid).move();
				if(server.empty() || RegistryConfig::SameFile(server, dll_path))
					continue;

				items += items.empty() ? L"\r\n    { \"clsid\": " : L",\r\n    { \"clsid\": ";
				json_quote(items, role.clsid);
				items += L", \"role\": ";
				json_quote(items, role.name);
				items += L", \"scope\": ";
				json_quote(items, store.scope);
				items += L", \"dll\": ";
				json_quote(items, server);
				items += L", \"exists\": ";
				items += json_bool(ConfigLocation::IsFile(server));
				items += L" }";
			}
		}
		return items.empty() ? string(L"[]") : (L"[" + items + L"\r\n  ]").move();
	}

	// "loadedInExplorer": the Nilesoft shell.dll files mapped into any
	// explorer.exe of this session, read from a Toolhelp module snapshot.
	// Read-only; Explorer is never touched. An explorer.exe that cannot be
	// opened (another user's) is skipped.
	static string loaded_in_explorer(const string &dll_path)
	{
		std::vector<string> paths;

		auto processes = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if(processes != INVALID_HANDLE_VALUE)
		{
			PROCESSENTRY32W pe{ sizeof(pe) };
			for(auto more = ::Process32FirstW(processes, &pe); more; more = ::Process32NextW(processes, &pe))
			{
				if(::CompareStringOrdinal(pe.szExeFile, -1, L"explorer.exe", -1, TRUE) != CSTR_EQUAL)
					continue;

				// May fail with ERROR_BAD_LENGTH while modules are loading; once more.
				auto modules = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pe.th32ProcessID);
				if(modules == INVALID_HANDLE_VALUE && ::GetLastError() == ERROR_BAD_LENGTH)
					modules = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pe.th32ProcessID);
				if(modules == INVALID_HANDLE_VALUE)
					continue;

				MODULEENTRY32W me{ sizeof(me) };
				for(auto m = ::Module32FirstW(modules, &me); m; m = ::Module32NextW(modules, &me))
				{
					if(::CompareStringOrdinal(me.szModule, -1, dll_name, -1, TRUE) != CSTR_EQUAL)
						continue;

					bool seen = false;
					for(const auto &known : paths)
						seen |= ::CompareStringOrdinal(known, -1, me.szExePath, -1, TRUE) == CSTR_EQUAL;
					if(seen)
						continue;

					auto image = ::LoadLibraryExW(me.szExePath, nullptr,
												  LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
					auto nilesoft = IsNilesoftModule(image);
					if(image)
						::FreeLibrary(image);
					if(nilesoft)
						paths.emplace_back(me.szExePath);
				}
				::CloseHandle(modules);
			}
			::CloseHandle(processes);
		}

		string items;
		for(const auto &loaded : paths)
		{
			items += items.empty() ? L"\r\n    { \"path\": " : L",\r\n    { \"path\": ";
			json_quote(items, loaded);
			items += L", \"ours\": ";
			items += json_bool(RegistryConfig::SameFile(loaded, dll_path));
			items += L" }";
		}
		return items.empty() ? string(L"[]") : (L"[" + items + L"\r\n  ]").move();
	}

	static const wchar_t *state_name(TreatAsState state)
	{
		switch(state)
		{
			case TreatAsState::absent: return L"absent";
			case TreatAsState::ours: return L"ours";
			case TreatAsState::foreign: return L"foreign";
			default: return L"inaccessible";
		}
	}

	static string build()
	{
		auto install_dir = InstallDirectory().move();
		auto dll_path = IO::Path::Combine(install_dir, dll_name).move();
		auto machine = RegistryConfig::IsRegistered(RegScope::Machine);
		auto user = RegistryConfig::IsRegistered(RegScope::User);
		auto location = ConfigLocation::Resolve(install_dir);
		auto user_dir = ConfigLocation::UserDirectory().move();

		auto win11 = Windows::Version::Instance().IsWindows11OrGreater();
		auto machine_treat = QueryTreatAs();
		auto user_treat_state = QueryUserTreatAs();
		auto user_inproc = RegistryConfig::KeyExists(HKEY_CURRENT_USER, UserTreatAsInproc);
		string value;
		auto user_inproc_ours = user_inproc &&
			RegistryConfig::GetString(HKEY_CURRENT_USER, UserTreatAsParent, UserTreatAsMarker, value);
		// The classic-menu tweak: a server key with no (or an empty) path.
		auto user_inproc_blank = user_inproc &&
			(!RegistryConfig::GetString(HKEY_CURRENT_USER, UserTreatAsInproc, nullptr, value) || value.empty());

		// Whether Explorer shows this user the classic menu (which Shell takes
		// over) instead of the Windows 11 one. COM resolves the CLSID through
		// the merged view, where the HKCU key shadows HKLM's: a per-user TreatAs
		// or empty InprocServer32 decides it, else the machine TreatAs does.
		auto classic = win11 &&
			(user_treat_state == TreatAsState::ours || user_inproc_blank ||
			 (user_treat_state == TreatAsState::absent && machine_treat == TreatAsState::ours));

		string json;
		json += L"{\r\n  \"version\": ";
		json_quote(json, APP_VERSION);
		json += L",\r\n  \"elevated\": ";
		json += json_bool(is_elevated);
		json += L",\r\n  \"installDir\": ";
		json_quote(json, install_dir);

		json += L",\r\n  \"dll\": { \"path\": ";
		json_quote(json, dll_path);
		json += L", \"exists\": ";
		json += json_bool(ConfigLocation::IsFile(dll_path));
		json += L" }";

		json += L",\r\n  \"registration\": {\r\n    \"mode\": ";
		json_quote(json, RegistryConfig::RegistrationMode());
		json += L",\r\n    \"machine\": { \"registered\": ";
		json += json_bool(machine);
		json += L", \"dll\": ";
		json_quote_or_null(json, RegistryConfig::InprocServerPath(RegScope::Machine));
		json += L" },\r\n    \"user\": { \"registered\": ";
		json += json_bool(user);
		json += L", \"dll\": ";
		json_quote_or_null(json, RegistryConfig::InprocServerPath(RegScope::User));
		json += L" }\r\n  }";

		json += L",\r\n  \"win11\": {\r\n    \"applicable\": ";
		json += json_bool(win11);
		json += L",\r\n    \"classicMenu\": ";
		json += json_bool(classic);
		json += L",\r\n    \"machineTreatAs\": ";
		json_quote(json, state_name(machine_treat));
		json += L",\r\n    \"userTreatAs\": ";
		json_quote(json, state_name(user_treat_state));
		json += L",\r\n    \"userInprocServer32\": ";
		json += json_bool(user_inproc);
		json += L",\r\n    \"userInprocServer32Ours\": ";
		json += json_bool(user_inproc_ours);
		json += L"\r\n  }";

		json += L",\r\n  \"config\": {\r\n    \"path\": ";
		json_quote(json, location.path);
		json += L",\r\n    \"source\": ";
		json_quote(json, ConfigLocation::SourceName(location.source));
		json += L",\r\n    \"exists\": ";
		json += json_bool(location.source != ConfigLocation::Source::none);
		json += L",\r\n    \"registryValue\": ";
		json_quote_or_null(json, location.configured);
		json += L"\r\n  }";

		json += L",\r\n  \"userConfigDir\": ";
		json_quote_or_null(json, user_dir);

		json += L",\r\n  \"conflicts\": ";
		json += conflicts(dll_path);
		json += L",\r\n  \"loadedInExplorer\": ";
		json += loaded_in_explorer(dll_path);

		// Where shell.dll, running as this user, writes its log.
		auto log_path = Logger::ResolvePath(dll_path, false).move();
		json += L",\r\n  \"log\": { \"path\": ";
		json_quote(json, log_path);
		json += L", \"exists\": ";
		json += json_bool(ConfigLocation::IsFile(log_path));
		json += L" }";
		json += L"\r\n}";
		return json.move();
	}

	static int run(const wchar_t *out_path)
	{
		CliOutput out(out_path);
		out.line(build());
		return out.ok() ? cli_ok : cli_failed;
	}
}

/*
class Console
{
	FILE *_handle = nullptr;
	HWND _hwnd = nullptr;

	bool init()
	{
		if(!_handle)
		{
			if(::AttachConsole(ATTACH_PARENT_PROCESS))
			{
				_hwnd = ::GetConsoleWindow();
				if(_hwnd) if(0 == ::freopen_s(&_handle, "CONOUT$", "w", stdout))
					::setvbuf(stdout, nullptr, _IONBF, 0);
			}
		}
		return _handle;
	}

public:
	Console()
	{
		init();
	}

	~Console()
	{
		if(_handle)
		{
			if(_hwnd)
				::PostMessageW(_hwnd, WM_KEYUP, VK_RETURN, 0);
			::fflush(_handle);
			::fclose(_handle);
			::FreeConsole();
		}
	}

	Console &write(const wchar_t *buff = nullptr)
	{
		wprintf(buff);
		return *this;
	}

	Console &writel(const wchar_t *buff = nullptr)
	{
		wprintf(L"%s%c", buff, L'\n');
		return *this;
	}
};

void check()
{
	Console c;
	c.writel(L"\n\nNilesoft Shell\n");

	_log->info(L"BEGIN CHECK");

	auto ver = &Windows::Version::Instance();

	auto clsid =[](const string &s)->string
	{
		return (L"CLSID\\" + s).move();
	};

	auto clsid_shex = [=](const string &s)->string
	{
		return (s + L"\\shellex\\ContextMenuHandlers\\nilesoft.shell").move();
	};

	auto exists_key = [=](HKEY k, const string &name)
	{
		if(!Registry::Exists(k, name))
		{
			wprintf(L"[warning] %s\n", name.c_str());
			_log->warning(name);
		}
	};

	auto exists_value_name = [=](HKEY k, const string &subkey, const string &name)
	{
		if(!Registry::ExistsValue(k, subkey, name))
		{
			wprintf(L"[warning] %s\\%s\n", name.c_str(), name.c_str());
			_log->warning(subkey + L"\\" + name);
		}
	};

	exists_key(HKCR, clsid(CLS_ContextMenu));
	exists_key(HKCR, clsid(CLS_FolderExtensions));
	exists_key(HKCR, clsid(CLS_IconOverlay));

	if(ver->IsWindows11OrGreater())
	{
		exists_key(HKCR, string().format(L"CLSID\\%s\\TreatAs", string::ToString(IID_FileExplorerContextMenu, 2).c_str()));
	}

	exists_key(HKCR, clsid_shex(L"*"));
	exists_key(HKCR, clsid_shex(L"Directory"));
	exists_key(HKCR, clsid_shex(L"Directory\\Background"));
	exists_key(HKCR, clsid_shex(L"Drive"));
	exists_key(HKCR, clsid_shex(L"DesktopBackground"));

	exists_value_name(HKLM, HKLM_APPROVED, CLS_ContextMenu);
	
	_log->info(L"END CHECK");
}
*/

struct Theme
{
	int mode = 1;

	struct 
	{
		uint32_t nor = 0xF1F1F1;
		uint32_t sel = 0xF1F1F1;
		uint32_t dis = 0x808080;
	}text;

	struct
	{
		uint32_t nor = 0x000000;
		uint32_t sel = 0x181818;
		uint32_t dis = 0x000000;
	}back;

	struct{
		uint32_t color = 0x000000;
		uint32_t border = 0x404040;
		uint32_t size = 1;
	}frame;
};

Theme m_theme;

HTHEME _hTheme = nullptr;

// shell.exe without arguments opens the app, Nilesoft Shell Pro, when NilesoftShellPro.exe
// sits next to it (the installer and the portable zip both put it there). It is started
// with no arguments; a running copy brings its window forward itself. Returns false when
// the app isn't there or can't be started, and shell.exe then shows its own window.
static bool LaunchApp()
{
	string dir = IO::Path::Parent(IO::Path::Module(nullptr)).move();
	string app = IO::Path::Combine(dir, L"NilesoftShellPro.exe").move();

	auto attributes = ::GetFileAttributesW(app.c_str());
	if(attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY))
		return false;

	// CreateProcess may write to the command line, so it gets its own buffer.
	std::wstring command_line = L"\"" + std::wstring(app.c_str()) + L"\"";
	STARTUPINFOW si{ sizeof(si) };
	PROCESS_INFORMATION pi{};
	if(!::CreateProcessW(app.c_str(), command_line.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi))
	{
		_log->error(L"cannot start '%s' (error %lu)", app.c_str(), ::GetLastError());
		return false;
	}

	// Let the app take the foreground (or bring an already open window forward).
	::AllowSetForegroundWindow(pi.dwProcessId);
	::CloseHandle(pi.hThread);
	::CloseHandle(pi.hProcess);
	return true;
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int)
{
	//auto argc = __argc;
	//auto argv = __wargv;

	is_elevated = Security::Elevation::IsElevated();

	COM_INITIALIZER com_initializer(true);

    CommandLine cmdline;
	REGOP reg{};

	_hInstance = hInstance;
	_log = &Logger::Instance();

	if(cmdline.ShowHelp)
	{
		//::DialogBoxParamW(hInstance, MAKEINTRESOURCEW(IDD_CMDLINE), nullptr, (DLGPROC)WndProc, 0);
		auto hWnd = ::CreateDialogParamW(_hInstance, MAKEINTRESOURCEW(IDD_CMDLINE), NULL, (DLGPROC)WndProc, 0);
		if(hWnd)
		{
			// CenterWindow(hwnd);
			::ShowWindow(hWnd, SW_SHOW);

			MSG msg;
			BOOL bRet;
			while((bRet = ::GetMessageW(&msg, nullptr, 0, 0)) != 0)
			{
				if(bRet == -1)
				{
					// Handle the error and possibly exit
					break;
				}
				else if(!IsWindow(hWnd) || !::IsDialogMessageW(hWnd, &msg))
				{
					::TranslateMessage(&msg);
					::DispatchMessage(&msg);
				}
			}
			::DestroyWindow(hWnd);
		}
		_log->close();
		return 0;
	}
	else if(cmdline.empty())
    {
		// Double-click: open the app if it's installed next to shell.exe.
		if(LaunchApp())
		{
			_log->close();
			return 0;
		}

		auto shell_window = ::FindWindowExW(nullptr, nullptr, UI::WC_Window, APP_FULLNAME);
		if (shell_window)
		{
			::SetForegroundWindow(shell_window);
			::SetActiveWindow(shell_window);
			//::SwitchToThisWindow(shell_window, TRUE);
			return 0;
		}

		auto win = Windows::Version::Instance();

		if(win.IsWindowsVersionOrGreater(10, 0, 14393))
			_dpi = DLL::User32<uint32_t>("GetSystemDpiForProcess", ::GetCurrentProcess());
		else
			_dpi = 96;// DC(GetDesktopWindow()).GetDeviceCapsY();

		_hTheme = ::OpenThemeData(nullptr, L"MENU");


        UI::App app(_hInstance, WindowProc, m_theme.frame.color,
                    LoadIcon(_hInstance, MAKEINTRESOURCE(IDI_RPMICON)), CS_HREDRAW | CS_VREDRAW);

        app.Init();

        RECT rc_screen{};
        ::GetClientRect(::GetDesktopWindow(), &rc_screen);

        rc_window.right = dpi(430);
        rc_window.bottom = dpi(220);

        rc_window.left = (rc_screen.right - rc_window.right) / 2;
        rc_window.top = (rc_screen.bottom - rc_window.bottom) / 2;

        main_window = new UI::Window(APP_FULLNAME, rc_window, 0, nullptr, 0);

        if(!main_window || !main_window->Handle)
        {
            return FALSE;
        }

		string path = IO::Path::Module(hInstance);
		path = path.substr(0, path.length() - 3).append(L"dll");

		DLL d(path, true);
		if(d)
		{
			auto hRes = ::FindResourceW(d, L"FONTICON", RT_RCDATA);
			if(hRes)
			{
				auto cjSize = ::SizeofResource(d, hRes);
				auto hResData = ::LoadResource(d, hRes);
				if(hResData)
				{
					auto lpFileView = ::LockResource(hResData);
					if(lpFileView)
					{
						DWORD numFonts = 0;
						_fonthandle = ::AddFontMemResourceEx(lpFileView, cjSize, nullptr, &numFonts);
						if(_fonthandle)
						{
							_hfont_icon = ::CreateFontW(-dpi(20), 0, 0, 0, FW_NORMAL, 0, 0, 0,
														SYMBOL_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
														ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Nilesoft.Shell");
						}
						UnlockResource(lpFileView);
					}
					::FreeResource(hResData);
				}
			}
		}

        main_window->Color.Background = color_background;

        int btn_w = dpi(250), btn_h = dpi(30), left = dpi(150);
		int offset_2 = dpi(2);

        rc_reg.top = dpi(40);
        rc_unreg.top = rc_reg.top + offset_2 + btn_h;

        rc_reg.bottom = btn_h;
        rc_unreg.bottom = btn_h;

        rc_reg.right = btn_w;
        rc_unreg.right = btn_w;

        rc_reg.left = left;
        rc_unreg.left = rc_reg.left;
		
		std::string dd = "<svg viewBox='0 0 512 512' width='100%' height='100%'><path fill='#14DCFE' d='M200 100L300 0l106 106-100 100z'/><path fill='#14A0FF' d='M100 200l100-100 212 212-100 100z'/><path fill='#1465FF' d='M106 406l100-100 106 106-100 100z'/></svg>";
		PlutoVG pluto(dd.data(), static_cast<int>(dd.length()), dpi(96), dpi(96), 96);
		hbitmap_logo = pluto.tobitmap();

		auto btn_close = new UI::Button(L"\uE256", { rc_window.right - dpi(27), dpi(1), dpi(26), dpi(26) }, ID_CLOSE, main_window, BS_OWNERDRAW, _hfont_icon, L"Close");
        
		auto btn_reg = new UI::Button(L"Register\tCtrl+R", rc_reg, ID_REG, main_window, BS_OWNERDRAW);
        auto btn_unreg = new UI::Button(L"Unregister\tCtrl+U", rc_unreg, ID_UNREG, main_window, BS_OWNERDRAW);
        auto btn_restart = new UI::Button(L"Restart Explorer\tCtrl+X", { rc_reg.left, rc_unreg.top + offset_2 + btn_h, btn_w, btn_h }, ID_RESTART, main_window, BS_OWNERDRAW);

        auto tt = btn_restart->Rect.top + dpi(30) + btn_h + offset_2;
        auto tl = btn_restart->Rect.left;

        auto btn_web = new UI::Button(L"\uE11F", { tl, tt, btn_h, btn_h }, ID_WEB, main_window, BS_OWNERDRAW, _hfont_icon,  L"Website Ctrl+W");
       
        tl += btn_h + offset_2;
        auto btn_email = new UI::Button(L"\uE115", { tl, tt, btn_h, btn_h }, ID_EMAIL, main_window, BS_OWNERDRAW, _hfont_icon, L"Email Ctrl+E");
       
        tl += btn_h + offset_2;
        auto btn_bug = new UI::Button(L"\uE22B", { tl, tt, btn_h, btn_h }, ID_GITHUB, main_window, BS_OWNERDRAW, _hfont_icon, L"Github Ctrl+G");

		//tl += (dpi(50) - btn_h) + btn_h + dpi(12);
		tl += btn_h + offset_2;
        auto btn_donate = new UI::Button(L"\uE1A8", { tl, tt, btn_h, btn_h }, ID_DONATE, main_window, BS_OWNERDRAW, _hfont_icon, L"Donate Ctrl+D");


        main_window->SetColor({ btn_reg, btn_unreg,btn_restart,btn_donate,btn_web,btn_email,btn_bug }, 
							  m_theme.text.nor, m_theme.back.nor, m_theme.text.sel, m_theme.back.sel);//0xeeeee0
        main_window->SetColor({ btn_close }, 0xFFFFFF, m_theme.back.nor, m_theme.text.nor, 0x2311E8);//E81123

		btn_reg->OnDraw = btn_unreg->OnDraw = btn_restart->OnDraw = btn_on_paint;

        auto ret = app.Run(main_window);

		delete main_window; 
		_log->close();

		if(_hfont_icon)
			::DeleteObject(_hfont_icon);

		if(_fonthandle)
			::RemoveFontMemResourceEx(_fonthandle);
		
		if(_hTheme)
		{
			::CloseThemeData(_hTheme);
			_hTheme = nullptr;
		}

		return ret;
    }
    else
    {
		bool _check = false;
		//bool runglyphs = false;
        string cmd;
		// -config:<init|reset|path>, -force, -out:<file>
		bool config = false, force = false;
		string config_command, out_path;
		// -config:check <root.nss>: the first plain argument after it.
		string config_root;
		bool want_root = false;
		// -config:check <root.nss> -titles[:<where>]
		bool titles = false;
		string titles_where;
		// -status [-out:<file>]
		bool status = false;
        for(auto op : cmdline)
        {
            if(op->Kind == '-' || op->Kind == '/')
            {
				if(op->has_name({ L"g", L"glyphs" }))
				{
					//runglyphs = true;
					//break;
				}
				else if(op->has_name(L"titles"))
				{
					titles = true;
					titles_where = op->Value;
				}
				else  if(op->has_name({ L"r", L"register", L"e", L"enable" }))
				{
					reg.REGISTER = true;;
                }
                else if(op->has_name({ L"u", L"unregister", L"d", L"disable" }))
                {
					reg.UNREGISTER = true;
                }
                else if(op->has_name({ L"t", L"treat" }))
                {
					reg.TREAT = true;
                }
				else if(op->has_name({ L"f", L"force" }))
				{
					reg.FOLDEREXTENSIONS=true;
					// Also: let -config:init overwrite existing files.
					force = true;
				}
				else if(op->has_name(L"config"))
				{
					config = true;
					config_command = op->Value;
					want_root = config_command.equals(L"check");
				}
				else if(op->has_name(L"out"))
				{
					out_path = op->Value;
				}
				else if(op->has_name(L"status"))
				{
					status = true;
				}
                else if(op->has_name(L"user"))
                {
					// Per-user (HKCU) register/unregister; no elevation.
					reg.USER = true;
                }
                else if(op->has_name({ L"s", L"silent" }))
                {
					reg.SILENT=true;
                }
                else if(op->has_name(L"restart"))
                {
					reg.RESTART = true;
                }
                else if(op->has_name(L"cmd"))
                {
                    cmd = op->Value;
                }
            }
			else if(want_root && config_root.empty())
			{
				// A plain argument. The option parser splits it at the first ':',
				// which every drive-letter path has, so take it unsplit.
				config_root = op->Argument;
			}
			else if(op->has_name(L"check"))
			{
				_check = true;
			}
        }

		if(_check)
		{
			//check();
			return 0;
		}

		// Read-only, never elevated, never interactive; handled on its own.
		if(status)
		{
			int ret = cli_usage;
			if(config)
			{
				CliOutput out(out_path.empty() ? nullptr : out_path.c_str());
				out.line(L"usage: -status and -config:* cannot be combined.");
			}
			else
				ret = StatusCommand::run(out_path.empty() ? nullptr : out_path.c_str());
			_log->close();
			return ret;
		}

		// Never elevated, never interactive; handled on its own.
		if(config)
		{
			auto ret = ConfigCommand::run(config_command, config_root, force, titles, titles_where, out_path.empty() ? nullptr : out_path.c_str());
			_log->close();
			return ret;
		}

		if(reg.REGISTER || reg.UNREGISTER || reg.RESTART || reg.FOLDEREXTENSIONS)
		{
			// Process exit code: 0 on success, 1 on failure. Returning the bool
			// directly made success exit with 1.
			return Register(reg) ? 0 : 1;
		}
        else
        {
            if(!cmd.empty())
            {
                int run = 1;
                if(auto o = cmdline[L"runas"])
                    run = (int)string::ToInt(o);

				int sw = SW_SHOWNORMAL;
				if(auto o = cmdline[L"show"])
					sw = (int)string::ToInt(o);

                string verb = L"open";
                if(run > 1)
                    verb = L"runas";

                auto hinst = ::ShellExecuteW(nullptr,
                                verb,
                                cmd,
                                cmdline[L"args"],
                                cmdline[L"wd"],
								sw);

				// ShellExecute reports success with a value greater than 32.
				if(reinterpret_cast<INT_PTR>(hinst) <= 32)
				{
					_log->close();
					return 1;
				}
            }
        }
    }

	_log->close();
    return 0;
}

HFONT FontSize(HFONT hFont, int size, int weight = 0, byte quality= ANTIALIASED_QUALITY)
{
    LOGFONT lf{};
	if(::GetObject(hFont, sizeof(LOGFONT), &lf))
	{
		lf.lfHeight = -size;
		lf.lfWidth = 0;
		if(weight > 0)
			lf.lfWeight = weight;
		lf.lfQuality = quality;
		return ::CreateFontIndirect(&lf);
	}
    return nullptr;
}

bool hover = false;
using namespace Nilesoft::Drawing;

void DrawPng(HDC hdc, HBITMAP hbitmap, int x, int y, int size = 16, int sx = 0, int sy = 0, int alpha = 255)
{
	if(hdc && hbitmap)
	{
		auto memDC = ::CreateCompatibleDC(hdc);
		if(memDC)
		{
			auto prev_bitmap = ::SelectObject(memDC, hbitmap);
			BLENDFUNCTION bf{};
			bf.BlendOp = AC_SRC_OVER;
			bf.BlendFlags = 0;
			bf.SourceConstantAlpha = (byte)alpha;
			bf.AlphaFormat = AC_SRC_ALPHA;
			::GdiAlphaBlend(hdc, x, y, size, size, memDC, sx, sy, size, size, bf);
			// Clean up
			::SelectObject(memDC, prev_bitmap);
			::DeleteDC(memDC);
		}
	}
}

void DrawString(HDC hdc, HFONT hFont, RECT *rc, COLORREF color, const wchar_t *text, int length, DWORD format, uint8_t opacity)
{
	if(!_hTheme || !hdc || !hFont) return;

	BLENDFUNCTION bf = { AC_SRC_OVER, 0, opacity, AC_SRC_ALPHA };
	BP_PAINTPARAMS params = { sizeof(BP_PAINTPARAMS) };
	params.dwFlags = BPPF_ERASE | BPPF_NOCLIP;
	params.pBlendFunction = &bf;
	HDC hdcPaint = nullptr;
	auto hBufferedPaint = ::BeginBufferedPaint(hdc, rc, BPBF_TOPDOWNDIB, &params, &hdcPaint);
	if(hBufferedPaint)
	{
		if(hdcPaint)
		{
			::SetTextColor(hdcPaint, color);
			::SetBkMode(hdcPaint, TRANSPARENT);

			auto hFontOld = ::SelectObject(hdcPaint, hFont);
			DTTOPTS dttOpts = { sizeof(DTTOPTS),  DTT_COMPOSITED | DTT_TEXTCOLOR, color };
			::BufferedPaintClear(hBufferedPaint, rc);
			::DrawThemeTextEx(_hTheme, hdcPaint, 0, 0, text, length, format, rc, &dttOpts);
			::SelectObject(hdcPaint, hFontOld);
		}
		::EndBufferedPaint(hBufferedPaint, TRUE);
	}
}

int __stdcall btn_on_paint(UI::Control* s, int msg, [[maybe_unused]] WPARAM wp, [[maybe_unused]] LPARAM lp)
{
	if(msg == WM_MOUSEMOVE)
		return 1;

	RECT rect{};
	::GetClientRect(s->Handle, &rect);

	DC dc(s->Handle);

	auto color_txt = msg == WM_MOUSEHOVER ? s->Color.Select.Text : s->Color.Text;
	auto color_bg = msg == WM_MOUSEHOVER ? s->Color.Select.Background : s->Color.Background;

	if(msg == WM_MOUSEHOVER || msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP)
	{
		//if(wp & MK_LBUTTON)
		{
			color_bg = s->Color.Select.Background;
		}
	}

	// if(color_bg == -1)
   //      color_bg = c->Parent->Color.Select.Background;

	if(color_bg != -1)
	{
		auto br = ::CreateSolidBrush(color_bg);
		::FillRect(dc, &rect, br);
		::DeleteObject(br);
	}

	if(!s->Text.empty())
	{
		auto ofont = s->Font.Handle;
		LOGFONT lf{};
		GetObject(ofont, sizeof(LOGFONT), &lf);

		lf.lfQuality = CLEARTYPE_QUALITY;
		//lf.lfWeight = FW_SEMIBOLD;
		lf.lfHeight = -dpi(14);
		auto cfont = ::CreateFontIndirect(&lf);
		auto tf = DT_VCENTER | DT_SINGLELINE | DT_NOCLIP;

		auto pos = s->Text.index_of('\t');
		if(pos != string::npos)
		{
			string left = s->Text.substr(0, pos);
			string right = s->Text.substr(pos++);

			auto rc = rect;
			rc.left = dpi(20);
			DrawString(dc, cfont, &rc, color_txt, left, left.length<int>(), DT_LEFT | tf, 255);

			rc.right -= dpi(20);
			DrawString(dc, cfont, &rc, m_theme.text.dis, right, right.length<int>(), DT_RIGHT | tf, 128);
		}
		else
		{
			DrawString(dc, cfont, &rect, color_txt, s->Text, -1, s->Font.Format.Align | tf, 255);
		}
		::DeleteObject(cfont);
	}
    return 0;
};

void Open(HWND hWnd, const wchar_t* cmd)
{
	::ShellExecuteW(hWnd, L"open", cmd, nullptr, nullptr, SW_NORMAL);
}

LRESULT CALLBACK WindowProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch(message)
    {
		case WM_CREATE:
		{
			::SendMessageW(hWnd, WM_SETFONT, (WPARAM)::GetStockObject(DEFAULT_GUI_FONT), TRUE);
			break;
		}
        case WM_ACTIVATE:// Extend the frame into the client area.
        {
           // MARGINS margins{ 0,0,0,1 };
            //::DwmExtendFrameIntoClientArea(hWnd, &margins);
            break;
        }
        case WM_PAINT:
        {
            PAINTSTRUCT ps{};
            DC dc = BeginPaint(hWnd, &ps);
			{
				RECT r;
				::GetWindowRect(hWnd, &r);
				::OffsetRect(&r, -r.left,- r.top);
				dc.draw_fillrect(r, m_theme.frame.color, m_theme.frame.border);

				BITMAP bmp{};
				GetObject(hbitmap_logo, sizeof(BITMAP), &bmp);
				//draw logo
				DrawPng(dc, hbitmap_logo, dpi(35), dpi(35), bmp.bmHeight, 0, 0, is_elevated ? 240 : 128);

				dc.set_back_mode();
				string text = APP_NAME L" ";
#ifdef APP_CANARY
				text += APP_CANARY L" ";
#endif
				text += APP_VERSION;

				dc.set_text(0x808080);

				auto ofont = (HFONT)::GetStockObject(DEFAULT_GUI_FONT);
				auto cfont = FontSize(ofont, dpi(14), CLEARTYPE_NATURAL_QUALITY);
				ofont = dc.set_font(cfont);

				RECT rect = { 0, dpi(150 + 10), dpi(150), dpi(25 + 128 + 32) };

				DrawString(dc, cfont, &rect, 0x808080, text, text.length<int>(), DT_CENTER, 255);

				text.format(L"\xA9 %d %s", VERSION_YEAR, APP_COMPANY);
				rect = { 0, dpi(150 + 30), dpi(150), dpi(25 + 128 + 32 + 15) };
				DrawString(dc, cfont, &rect, 0x808080, text, text.length<int>(), DT_CENTER, 255);
				::DeleteObject(::SelectObject(dc, ofont));
			}
            ::EndPaint(hWnd, &ps);
            return TRUE;
        }
        //case WM_ERASEBKGND:
        //    return 0;
        case WM_NCCALCSIZE:
            return FALSE;
        case WM_NCHITTEST:
            return HTCAPTION;
		case WM_ERASEBKGND:
			return TRUE;
        case WM_COMMAND:
        {
            switch(wParam)
            {
                case ID_CLOSE:
                    ::DestroyWindow(hWnd);
                    break;
                case ID_MINIMIZE:
                    SendMessage(hWnd, WM_SYSCOMMAND, SC_MINIMIZE, lParam);
                    break;
                case ID_WEB:
					Open(hWnd, APP_WEBSITE);
                    break;
                case ID_DONATE:
					Open(hWnd, L"https://nilesoft.org/donate");
                    break;
                case ID_EMAIL:
					Open(hWnd, L"mailto:support@nilesoft.org");
                    break;
                case ID_GITHUB:
					Open(hWnd, L"https://github.com/moudey/shell");
                    break;
				case ID_RESTART:
					Windows::Explorer::Restart();
					break;
                case ID_REG:
				case ID_UNREG:
				{
					REGOP reg{};
					reg.RESTART = true;
					reg.TREAT = true;
					if(wParam == ID_REG)
						reg.REGISTER = true;
					else if(wParam == ID_UNREG)
						reg.UNREGISTER = true;
					return Register(reg, hWnd);
				}
            }
            break;
        }
        case WM_SYSKEYUP:
        case WM_KEYUP:
      //      return ::DefWindowProc(hWnd, message, wParam, lParam);
     //   case WM_SYSKEYDOWN:
     //   case WM_KEYDOWN:
        {
            if(::GetAsyncKeyState(VK_CONTROL) < 0)
            {
                //  int count = 0;
                //  for(int i = 0; i < 256; i++)
                //      count += IsAsyncKeyDown(i);

                //  if(count == 2)
                {
                    auto vkCode = LOWORD(wParam); // virtual-key code
                    switch(vkCode)
                    {
                        case 'R':
                            main_window->SendCommand(ID_REG);
                            break;
                        case 'U':
                            main_window->SendCommand(ID_UNREG);
                            break;
                        case 'X':
                            main_window->SendCommand(ID_RESTART);
                            break;
                        case 'W':
                            main_window->SendCommand(ID_WEB);
                            break;
                        case 'E':
                            main_window->SendCommand(ID_EMAIL);
                            break;
                        case 'G':
                            main_window->SendCommand(ID_GITHUB);
                            break;
                        case 'D':
                            main_window->SendCommand(ID_DONATE);
                            break;
                    }
                }
            }
            break;
        }
        case WM_CLOSE:
            ::DestroyWindow(hWnd);
            break;
        case WM_DESTROY:
        case WM_ENDSESSION:
            ::PostQuitMessage(0);
            break;
        default:
            return ::DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

BOOL CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, [[maybe_unused]] LPARAM lParam)
{
    switch(message)
    {
		case WM_INITDIALOG:
        {
			//WCHAR szTitle[100];
			// LoadString(g_hInstance, IDS_APP_TITLE, szTitle, ARRAYSIZE(szTitle));
            auto usage = L"Command-Line Help\r\n\r\n"
                L"Without arguments, shell.exe opens the app,\r\n"
                L"Nilesoft Shell Pro (NilesoftShellPro.exe next to\r\n"
                L"shell.exe), or this program's own register and\r\n"
                L"unregister window when the app isn't there.\r\n\r\n"
                //L"shell.exe [-[r][u]] [-i] [-s] [-re]\n\n"
                L"-register\t\tRegistering.\r\n"
                L"-unregister\tUnregistering.\r\n"
                L"-user\t\tWith -register/-unregister: current user\r\n"
                L"\t\tonly (HKCU), no administrator rights needed.\r\n"
                L"-treat\t\tDisable Windows 11 context menu.\r\n"
                L"-silent\t\tPrevents displaying messages.\r\n"
                L"-restart\t\tRestart Windows Explorer (this session;\r\n"
                L"\t\tno administrator rights needed).\r\n\r\n"
                L"Per-user configuration (no administrator rights):\r\n"
                L"-config:init\tCopy shell.nss and imports\\ to\r\n"
                L"\t\t%APPDATA%\\Nilesoft\\Shell. Existing files are\r\n"
                L"\t\tkept unless -force is given.\r\n"
                L"-config:reset\tBack the per-user folder up to\r\n"
                L"\t\tbackup-<timestamp>, then copy the defaults again.\r\n"
                L"-config:path\tPrint the config file in use.\r\n"
                L"-config:check <file>\r\n"
                L"\t\tParse <file> and its imports; print the\r\n"
                L"\t\terrors and warnings as JSON (exit 1 on error).\r\n"
                L"\t\tWith -titles[:<where>], also list the menu titles\r\n"
                L"\t\t(where: file, folder, back, desktop, taskbar).\r\n"
                L"-status\t\tPrint registration, Windows 11 menu and\r\n"
                L"\t\tconfig state as JSON (read-only).\r\n"
                L"-out:<file>\tWrite the output to <file> (UTF-8)\r\n"
                L"\t\tinstead of stdout / the console.\r\n\r\n"
                //L"-runas:N\t\tLaunch with elevated privileges.\r\n"
                //L"\t\tN=[admin | system | trustedinsaller]\r\n\r\n"
                L"-?\t\tDispay this help message.\r\n\r\n"
                L"Examples:\r\nshell.exe -register -treat\r\n"
                L"shell.exe -register -user -treat -restart\r\n"
              //  L"shell.exe -runas:admin -cmd:'cmd.exe' -args:\"/K echo Hello world!\"\r\n"
                ;
            ::SetDlgItemTextW(hwnd, IDC_CMDLINE_TEXT, usage);
           // SendDlgItemMessage(hwnd, IDC_CMDLINE_TEXT, EM_SETSEL, -1, -1);
            break;
        }
        case WM_CLOSE:
        {
            ::PostQuitMessage(0);
            break;
        }
        case WM_COMMAND:
        {   
            if(LOWORD(wParam) == IDOK)
                ::PostQuitMessage(0);
            break;
        }
       
    }
    return FALSE;
}
