
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

				REGOP regop;
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
		copy_file(src_config, ConfigLocation::Join(user_dir, ConfigLocation::FileName), overwrite, stats);

		auto src_imports = ConfigLocation::Join(install_dir, ConfigLocation::ImportsName).move();
		if(is_directory(src_imports))
			copy_tree(src_imports, ConfigLocation::Join(user_dir, ConfigLocation::ImportsName), overwrite, stats);

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

	static int run(const string &command, bool force, const wchar_t *out_path)
	{
		CliOutput out(out_path);

		int ret;
		if(command.equals(L"init"))
			ret = init(out, force);
		else if(command.equals(L"reset"))
			ret = reset(out);
		else if(command.equals(L"path"))
			ret = path(out);
		else
		{
			out.line(L"usage: shell -config:init [-force] | -config:reset | -config:path [-out:<file>]");
			return cli_usage;
		}

		// The result could not be reported: say so through the exit code.
		if(ret == cli_ok && !out.ok())
			ret = cli_failed;
		return ret;
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
        for(auto op : cmdline)
        {
            if(op->Kind == '-' || op->Kind == '/')
            {
				if(op->has_name({ L"g", L"glyphs" }))
				{
					//runglyphs = true;
					//break;
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
				}
				else if(op->has_name(L"out"))
				{
					out_path = op->Value;
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

		// Never elevated, never interactive; handled on its own.
		if(config)
		{
			auto ret = ConfigCommand::run(config_command, force, out_path.empty() ? nullptr : out_path.c_str());
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
