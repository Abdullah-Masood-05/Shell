#define VC_EXTRALEAN
//#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlwapi.h>
#include <msi.h>
#include <msiquery.h>
#include <string>
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "msi.lib")

template<typename ... Args>
static void log(const char *format, Args ... args)
{
	/*
	using ___snprintfFT = size_t(__stdcall *)(char *, size_t, const char *, Args... args);
	if(auto hm = ::GetModuleHandleW(L"msvcrt.dll"); hm)
	{
		auto ___snprintf = (___snprintfFT)::GetProcAddress(hm, "_snprintf");
		if(___snprintf)
		{
			auto size = ___snprintf(nullptr, 0, format, args ...);
			if(size > 0)
			{
				std::string buf(size, '\0');
				___snprintf(buf.data(), size, format, args ...);
				::OutputDebugStringA(buf.c_str());
			}
		}
	}
	*/
	auto size = ::_snprintf(nullptr, 0, format, args ...);
	if(size > 0)
	{
		std::string buf(size, '\0');
		::_snprintf(buf.data(), size, format, args ...);
		::OutputDebugStringA(buf.c_str());
	}
}

struct MSI
{
	MSIHANDLE hInstall = 0;

	MSI(MSIHANDLE hInstall = 0)
		: hInstall{ hInstall }
	{
	}

	bool get(const wchar_t *name, std::wstring &value) const
	{
		if(hInstall && name != nullptr)
		{
			DWORD length = 0;
			auto hr = ::MsiGetPropertyW(hInstall, name, nullptr, &length);
			if(length > 0)
			{
				value.resize(length++);
				hr = ::MsiGetPropertyW(hInstall, name, value.data(), &length);
				return true;
			}
		}
		return false;
	}


	std::wstring InstallFolder()
	{
		std::wstring result;
		get(L"INSTALLFOLDER", result);
		return std::move(result);
	}

	std::wstring ProductCode()
	{
		std::wstring result;
		get(L"ProductCode", result);
		return std::move(result);
	}

	bool set(const wchar_t *name, const wchar_t *value) const
	{
		if(hInstall && name != nullptr)
		{
			return ERROR_SUCCESS == ::MsiSetPropertyW(hInstall, name, value);
		}
		return false;
	}
};

bool streq(const std::wstring &s1, const wchar_t *s2)
{
	return ::memicmp(s1.c_str(), s2, s1.size() * sizeof(wchar_t)) == 0;
}

#undef ShellExecute

constexpr auto FILEEXE = L"shell.exe";
constexpr auto FILEDLL = L"shell.dll";
constexpr auto FILEOLD = L"shell.old";
constexpr auto FILECONFIG = L"shell.nss";
constexpr auto FILECONFIGBACKUP = L"shell.nss.upgrade";
constexpr auto FILECONFIGSTOCK = L"shell.nss.stock-new";

// Set by the immediate Update action when it saved shell.nss in this session;
// the restore actions in setup.wxs are conditioned on it, so a backup left
// behind by an earlier upgrade is never resurrected over a newer config.
constexpr auto PROPCONFIGSAVED = L"SHELLCONFIGSAVED";

constexpr auto upgrading = L"UPGRADINGPRODUCTCODE";

BOOL ShellExec(const wchar_t *file, const wchar_t *parameters, const wchar_t *directory, bool run_as_admin = false, int nshow = SW_NORMAL, bool wait = false)
{
	SHELLEXECUTEINFOW sei = { };
	sei.cbSize = sizeof(SHELLEXECUTEINFO);
	sei.fMask = SEE_MASK_FLAG_NO_UI | (wait ? SEE_MASK_NOCLOSEPROCESS : 0);
	sei.lpFile = file;
	sei.lpVerb = run_as_admin ? L"runas" : nullptr;
	sei.lpParameters = parameters;
	sei.lpDirectory = directory;
	sei.nShow = nshow;

	auto res = ::ShellExecuteExW(&sei) || (wait && !sei.hProcess);
	if(!res)
		return res;

	if(!wait)
		return res;

	// Wait until child process exits.
	MSG msg;
	DWORD dw;
	while(wait)
	{
		dw = ::MsgWaitForMultipleObjects(1, &sei.hProcess, FALSE, INFINITE, QS_ALLINPUT);
		if(dw != WAIT_OBJECT_0 + 1)
		{
			// unexpected failure
			wait = false;
			break;
		}
		else while(::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))  // we have a message - peek and dispatch it
		{
			if(msg.message == WM_QUIT)
			{
				wait = false;
				break;
			}
			::DispatchMessageW(&msg);
		}
	}
	::CloseHandle(sei.hProcess);
	return res;
}

/*
static BOOL ProcessStart(const wchar_t *application, const wchar_t *parameters, const wchar_t *directory = nullptr, bool wait = false, int showCmd = SW_SHOWNORMAL)
{
	PROCESS_INFORMATION processInfo = { };
	STARTUPINFOW startupInfo = { };
	startupInfo.cb = sizeof(STARTUPINFOW);
	startupInfo.wShowWindow = (WORD)showCmd;
	startupInfo.dwFlags = STARTF_USESHOWWINDOW;
	std::wstring commandLine= parameters;

	auto result = ::CreateProcessW(application,
							commandLine.data(), nullptr, nullptr, FALSE,
							NORMAL_PRIORITY_CLASS,
							nullptr, directory,
							&startupInfo, &processInfo);
	if(!result)
		return FALSE;

	// Wait until child process exits.
	if(wait && processInfo.hProcess)
		::WaitForSingleObject(processInfo.hProcess, INFINITE);

	if(processInfo.hThread)
		::CloseHandle(processInfo.hThread);
	if(processInfo.hProcess)
		::CloseHandle(processInfo.hProcess);

	return result;
}

static bool IsDirectoryExists(const std::wstring_view &path)
{
	auto attr = ::GetFileAttributesW(path.data());
	return (attr != UINT32_MAX && (attr & FILE_ATTRIBUTE_DIRECTORY));
}

// Checks if a file exists (accessible)
static bool IsFileExists(const std::wstring_view &path)
{
	auto attr = ::GetFileAttributesW(path.data());
	//return (attr != INVALID_FILE_ATTRIBUTES && (!(attr & FILE_ATTRIBUTE_DIRECTORY)));
	if(attr == INVALID_FILE_ATTRIBUTES)
	{
		auto errval = ::GetLastError();
		return (errval != ERROR_FILE_NOT_FOUND)
			&& (errval != ERROR_PATH_NOT_FOUND)
			&& (errval != ERROR_INVALID_NAME)
			&& (errval != ERROR_INVALID_DRIVE)
			&& (errval != ERROR_NOT_READY)
			&& (errval != ERROR_INVALID_PARAMETER)
			&& (errval != ERROR_BAD_PATHNAME)
			&& (errval != ERROR_BAD_NETPATH);
	}
	return true;
}
*/

static std::wstring JoinPath(const std::wstring &path1, const std::wstring &path2)
{
	std::wstring path = path1;
	if(path.size() > 0 && path.back() != L'\\')
	{
		if(path2.size() > 0 && path2.front() != L'\\')
			path += L'\\';
	}
	// path, not path1: the separator is appended to the local copy.
	return path + path2;
}

static bool InstallFolder(MSIHANDLE hInstall, std::wstring& install_folder, bool find_by_reg)
{
	MSI msi(hInstall);
	DWORD length = MAX_PATH;
	install_folder = msi.InstallFolder();

	// A deferred custom action runs in the installer's own elevated process and
	// cannot read INSTALLFOLDER - the only property it can see is the
	// CustomActionData its immediate counterpart set for it.
	if(install_folder.size() == 0)
		msi.get(L"CustomActionData", install_folder);

	if(install_folder.size() == 0)
	{
		if(find_by_reg)
		{
			install_folder.resize(MAX_PATH);
			auto rc = ::RegGetValueW(HKEY_CLASSES_ROOT,
									 L"CLSID\\{BAE3934B-8A6A-4BFB-81BD-3FC599A1BAF1}\\InprocServer32",
									 nullptr, RRF_RT_REG_SZ | KEY_WOW64_64KEY, nullptr, static_cast<void *>(install_folder.data()), &length);
			if(rc == ERROR_SUCCESS)
			{
				install_folder.resize(length / sizeof(wchar_t));
				auto p = install_folder.find_last_of(L"\\");
				if(p != install_folder.npos)
				{
					install_folder = std::move(install_folder.substr(0, p + 1));
				}
			}
		}
	}
	return install_folder.size() > 0;
}
/*
bool start(MSIHANDLE hInstall, const wchar_t *parameters, bool run_as_admin, bool wait, bool find_dir_by_reg)
{
	auto result = false;
	MSI msi(hInstall);
	std::wstring install_folder;
	if(InstallFolder(hInstall, install_folder, find_dir_by_reg))
	{
		std::wstring sh = std::move(JoinPath(install_folder, FILEEXE));
		result = ShellExec(JoinPath(install_folder, FILEEXE).c_str(), 
							parameters, install_folder.c_str(), true, SW_HIDE, true);
		std::wstring old = std::move(JoinPath(install_folder, FILEOLD));
		if(::PathFileExistsW(old.c_str()))
			::DeleteFileW(old.c_str());
	}
	return result;
}
*/
//MsiGetProperty (hMSI, "CustomActionData", strOurVersion, numSize);
//::MsiSetPropertyW(hInstall, L"MSIRESTARTMANAGERCONTROL", L"Disable");

//constexpr auto UninstallKey = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\";

/*
	shell.nss is the user's file, and a major upgrade destroyed it.

	CONFIG used to be NeverOverwrite. Whether a component installs at all is
	resolved during costing, while the old product's shell.nss is still on disk,
	so the new product resolved CONFIG to "install nothing" - and the removal of
	the old product (RemoveExistingProducts) then took the file with it, since
	nothing else held the component any more. Upgrade log of that authoring:

		Component: CONFIG; Installed: Absent; Request: Local;  Action: Null
		Component: CONFIG; Installed: Local;  Request: Absent; Action: Absent
		Executing op: FileRemove(,FileName=shell.nss,,)

	NeverOverwrite is dropped in setup.wxs, so the new product owns CONFIG and
	the removal no longer deletes it. Without NeverOverwrite the stock file may
	replace the user's one (Windows Installer only keeps an unversioned file
	whose Modified date is later than its Create date, which a config restored
	by a backup tool does not satisfy), so the file is also copied out by the
	immediate Update action and copied back after InstallFiles by the deferred
	RestoreConfig action.

	The backup lives under %ProgramData%, whose default ACL grants
	BUILTIN\Users (CI)(WD,AD), because the immediate action runs as the
	invoking user. The version's own config is written beside the restored one
	as shell.nss.stock-new when the two differ, rather than being lost.

	The copy is not deleted once restored: if the transaction rolls back after
	the restore, file-level rollback takes the restored config with it and this
	copy is the only one left. It is overwritten by the next upgrade that finds
	a config, and removed on a real uninstall.

		https://learn.microsoft.com/en-us/windows/win32/msi/component-table
		https://learn.microsoft.com/en-us/windows/win32/msi/file-versioning-rules
		https://learn.microsoft.com/en-us/windows/win32/msi/deferred-execution-custom-actions
*/
static std::wstring ProgramDataFolder()
{
	wchar_t root[MAX_PATH]{};
	auto length = ::ExpandEnvironmentStringsW(L"%ProgramData%", root, ARRAYSIZE(root));

	// The return is a character count including the terminator, and zero on
	// failure; anything larger than the buffer means it was truncated.
	if(length == 0 || length > ARRAYSIZE(root))
		return {};

	return root;
}

static std::wstring ConfigBackupPath()
{
	auto root = ProgramDataFolder();
	if(root.empty())
		return {};

	return JoinPath(JoinPath(JoinPath(root, L"Nilesoft"), L"Shell"), FILECONFIGBACKUP);
}

static bool BackupUserConfig(const std::wstring &install_folder)
{
	auto config = JoinPath(install_folder, FILECONFIG);

	// Nothing to save.
	if(!::PathFileExistsW(config.c_str()))
		return false;

	auto root = ProgramDataFolder();
	if(root.empty())
		return false;

	// Two levels, and CreateDirectory on one that already exists is harmless.
	auto vendor = JoinPath(root, L"Nilesoft");
	auto folder = JoinPath(vendor, L"Shell");
	::CreateDirectoryW(vendor.c_str(), nullptr);
	::CreateDirectoryW(folder.c_str(), nullptr);

	return ::CopyFileW(config.c_str(), JoinPath(folder, FILECONFIGBACKUP).c_str(), FALSE) != FALSE;
}

// Byte-for-byte, so a config that only moved is not reported as changed.
static bool SameContent(const std::wstring &a, const std::wstring &b)
{
	WIN32_FILE_ATTRIBUTE_DATA fa{}, fb{};
	if(!::GetFileAttributesExW(a.c_str(), GetFileExInfoStandard, &fa)
	   || !::GetFileAttributesExW(b.c_str(), GetFileExInfoStandard, &fb))
		return false;

	if(fa.nFileSizeHigh != fb.nFileSizeHigh || fa.nFileSizeLow != fb.nFileSizeLow)
		return false;

	auto open = [](const std::wstring &path)
	{
		return ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
							 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	};

	auto ha = open(a);
	auto hb = open(b);

	auto same = (ha != INVALID_HANDLE_VALUE && hb != INVALID_HANDLE_VALUE);
	while(same)
	{
		char ba[4096], bb[4096];
		DWORD ra = 0, rb = 0;
		if(!::ReadFile(ha, ba, sizeof(ba), &ra, nullptr)
		   || !::ReadFile(hb, bb, sizeof(bb), &rb, nullptr))
		{
			same = false;
			break;
		}
		if(ra != rb || ::memcmp(ba, bb, ra) != 0)
		{
			same = false;
			break;
		}
		if(ra == 0)
			break;
	}

	if(ha != INVALID_HANDLE_VALUE) ::CloseHandle(ha);
	if(hb != INVALID_HANDLE_VALUE) ::CloseHandle(hb);
	return same;
}

static void RestoreUserConfig(const std::wstring &install_folder)
{
	auto backup = ConfigBackupPath();
	if(backup.empty() || !::PathFileExistsW(backup.c_str()))
		return;

	auto config = JoinPath(install_folder, FILECONFIG);

	// The user's file is about to win, so anything new in the version's own
	// config would be lost silently. Leave that copy beside theirs instead.
	if(::PathFileExistsW(config.c_str()) && !SameContent(config, backup))
		::CopyFileW(config.c_str(), JoinPath(install_folder, FILECONFIGSTOCK).c_str(), FALSE);

	// Failing here leaves the stock config in place, which is a working menu -
	// the state this whole path exists to avoid is no config file at all.
	::CopyFileW(backup.c_str(), config.c_str(), FALSE);
}

static void DiscardUserConfigBackup()
{
	auto backup = ConfigBackupPath();
	if(backup.empty())
		return;

	::DeleteFileW(backup.c_str());

	// Both refuse when the directory is not empty, which is the wanted answer.
	auto root = ProgramDataFolder();
	if(!root.empty())
	{
		auto vendor = JoinPath(root, L"Nilesoft");
		::RemoveDirectoryW(JoinPath(vendor, L"Shell").c_str());
		::RemoveDirectoryW(vendor.c_str());
	}
}

/*
	Frees the canonical shell.dll path so Windows Installer can write the new
	binary instead of demanding a reboot.

	Windows will not let a mapped image be overwritten or deleted, but it will
	let it be renamed within its volume - and shell.dll is mapped into every
	process that has raised a shell context menu.

	This used to rotate to a fixed "shell.old", which worked exactly once. After
	that the name was taken by a rotation that was itself still mapped, so
	MoveFileW refused to overwrite it and the DeleteFileW that followed could not
	remove a mapped file either - and neither result was checked.
*/
static bool RotateOutOfTheWay(const std::wstring &path)
{
	if(!::PathFileExistsW(path.c_str()))
		return true;

	SYSTEMTIME st{};
	::GetLocalTime(&st);

	// A name that cannot already be taken, however many times this has run.
	// No MOVEFILE_REPLACE_EXISTING: a name that is already there belongs to an
	// earlier rotation that something may still have mapped.
	for(int attempt = 0; attempt < 64; attempt++)
	{
		wchar_t suffix[96]{};
		::swprintf(suffix, ARRAYSIZE(suffix), L".old.%04u%02u%02u%02u%02u%02u_%lu_%d",
				   st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
				   ::GetCurrentProcessId(), attempt);

		std::wstring rotated = path + suffix;
		if(::PathFileExistsW(rotated.c_str()))
			continue;

		if(::MoveFileExW(path.c_str(), rotated.c_str(), 0))
			return true;
	}

	return false;
}

// Rotations left by earlier installs, once nothing maps them any more. Deleting
// one that is still mapped fails harmlessly and it is tried again next time.
static void PruneRotations(const std::wstring &install_folder)
{
	const wchar_t *patterns[] = { L"shell.dll.old*", L"shell.exe.old*", FILEOLD };

	for(auto pattern : patterns)
	{
		WIN32_FIND_DATAW fd{};
		auto find = ::FindFirstFileW(JoinPath(install_folder, pattern).c_str(), &fd);
		if(find == INVALID_HANDLE_VALUE)
			continue;

		do
		{
			if(!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
				::DeleteFileW(JoinPath(install_folder, fd.cFileName).c_str());
		}
		while(::FindNextFileW(find, &fd));

		::FindClose(find);
	}
}

UINT __stdcall Install(MSIHANDLE hInstall)
{
	std::wstring install_folder;
	if(InstallFolder(hInstall, install_folder, false))
	{
		ShellExec(JoinPath(install_folder, FILEEXE).c_str(),
				  L"-r -s -t -restart", install_folder.c_str(), true, SW_HIDE, true);

		// Explorer has just been restarted, so the rotation this install made
		// may already be unmapped.
		PruneRotations(install_folder);
	}
	return ERROR_SUCCESS;
	//return ERROR_INSTALL_FAILURE;
}

UINT __stdcall Uninstall(MSIHANDLE hInstall)
{
	std::wstring install_folder;
	if(InstallFolder(hInstall, install_folder, true))
	{
		ShellExec(JoinPath(install_folder, FILEEXE).c_str(),
				  L"-u -s -t -restart", install_folder.c_str(), true, SW_HIDE, true);
	}

	// A real uninstall, not the removal half of an upgrade - the condition on
	// this action in InstallExecuteSequence excludes UPGRADINGPRODUCTCODE. So
	// nothing is going to ask for the saved config again.
	DiscardUserConfigBackup();

	return ERROR_SUCCESS;
	//return res ? ERROR_SUCCESS : ERROR_INSTALL_FAILURE;
}

UINT __stdcall Update(MSIHANDLE hInstall)
{
	std::wstring install_folder;
	if(InstallFolder(hInstall, install_folder, true))
	{
		// This entry point is scheduled twice - immediate and deferred, see
		// setup.wxs. Only the immediate one saves the config:
		// MSIRUNMODE_SCHEDULED is true for "a custom action called from install
		// script execution", i.e. the deferred half, which runs as LocalSystem;
		// letting it create the backup directory would leave Users read-only on
		// it, and every later upgrade's immediate half could not write there.
		//
		//   https://learn.microsoft.com/en-us/windows/win32/api/msiquery/nf-msiquery-msigetmode
		if(!::MsiGetMode(hInstall, MSIRUNMODE_SCHEDULED))
		{
			if(BackupUserConfig(install_folder))
				MSI(hInstall).set(PROPCONFIGSAVED, L"1");
		}

		RotateOutOfTheWay(JoinPath(install_folder, FILEDLL));
		PruneRotations(install_folder);
	}
	return ERROR_SUCCESS;
}

// Deferred, after InstallFiles: the package's own files are on disk by now, so
// this is the point at which the saved config can be put back over them.
UINT __stdcall RestoreConfig(MSIHANDLE hInstall)
{
	std::wstring install_folder;
	if(InstallFolder(hInstall, install_folder, true))
		RestoreUserConfig(install_folder);

	return ERROR_SUCCESS;
}

UINT __stdcall ValidatePath(MSIHANDLE hInstall)
{
	HRESULT hr = S_OK;

	BOOL fInstallPathIsRemote = TRUE;
	BOOL fInstallPathIsRemoveable = TRUE;

	MSI msi(hInstall);
	std::wstring pwszWixUIDir, pwszInstallPath;
	if(!msi.get(L"WIXUI_INSTALLDIR", pwszWixUIDir))
	{
		log("failed to get WixUI Installation Directory");
		return ERROR_INSTALL_FAILURE;
	}

	if(!msi.get(pwszWixUIDir.c_str(), pwszInstallPath))
	{
		log("failed to get Installation Directory");
		return ERROR_INSTALL_FAILURE;
	}

	std::wstring pStrippedTargetFolder = pwszInstallPath;

	// Terminate the path at the root
	if(!::PathStripToRootW(pStrippedTargetFolder.data()))
	{
		hr = HRESULT_FROM_WIN32(ERROR_INVALID_DRIVE);
		if(FAILED(hr))
		{
			log("failed to parse target folder");
			return hr;
		}
	}

	auto drive_type = ::GetDriveTypeW(pStrippedTargetFolder.c_str());

	fInstallPathIsRemote = (DRIVE_REMOTE == drive_type);
	fInstallPathIsRemoveable = ((DRIVE_CDROM == drive_type) || (DRIVE_REMOVABLE == drive_type) || (DRIVE_RAMDISK == drive_type) || (DRIVE_UNKNOWN == drive_type));

	// If the path does not point to a network drive, mapped drive, or removable drive,
	// then set WIXUI_INSTALLDIR_VALID to "1" otherwise set it to 0
	BOOL fInstallPathIsUnc = ::PathIsUNCW(pwszInstallPath.c_str());
	if(!fInstallPathIsUnc && !fInstallPathIsRemote && !fInstallPathIsRemoveable)
	{
		// path is valid
		if(!msi.set(L"WIXUI_INSTALLDIR_VALID", L"1"))
		{
			log("failed to set WIXUI_INSTALLDIR_VALID");
			return ERROR_INSTALL_FAILURE;
		}
	}
	else
	{
		// path is invalid; we can't log it because we're being called from a DoAction control event
		// but we can at least call WcaLog to get it to write to the debugger from a debug build
		log("Installation path %ls is invalid: it is %s UNC path, %s remote path, or %s path on a removable drive, and must be none of these.",
			pwszInstallPath.c_str(), fInstallPathIsUnc ? "a" : "not a", fInstallPathIsRemote ? "a" : "not a", fInstallPathIsRemoveable ? "a" : "not a");

		if(!msi.set(L"WIXUI_INSTALLDIR_VALID", L"0"))
		{
			log("failed to set WIXUI_INSTALLDIR_VALID");
			return ERROR_INSTALL_FAILURE;
		}
	}
	return ERROR_SUCCESS;
}

extern "C" BOOL WINAPI DllMain(IN HINSTANCE hInst, IN ULONG ulReason, IN LPVOID)
{
	switch(ulReason)
	{
		case DLL_PROCESS_ATTACH:
			break;

		case DLL_PROCESS_DETACH:
			break;
	}

	return TRUE;
}


