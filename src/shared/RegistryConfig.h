#pragma once

namespace Nilesoft
{
	struct REGOP
	{
		bool REGISTER;
		bool UNREGISTER;
		bool TREAT;
		bool CONTEXTMENU;
		bool FOLDEREXTENSIONS;
		bool ICONOVERLAY;
		bool RESTART;
		bool SILENT;
		// Per-user registration (HKCU\Software\Classes only, no elevation).
		bool USER;
	};

	// Which registry store a registration is written to.
	//   Machine: HKLM\SOFTWARE\Classes (administrators only, every user).
	//   User:    HKCU\Software\Classes (the current user only, no elevation).
	enum class RegScope { Machine, User };

	class RegistryConfig
	{
#define	APP_SIG						L" @nilesoft.shell"
#define	APP_COMP_NAME				APP_COMPANY L"." APP_NAME
#define APP_KEY						L"SOFTWARE\\" APP_COMPANY L"\\" APP_NAME

//L"SOFTWARE\\Classes\\Drive\\shellex\\FolderExtensions\\"
#define HKLM_DRIVE_FolderExtensions	L"SOFTWARE\\Classes\\Drive\\shellex\\FolderExtensions\\" CLS_FolderExtensions
#define HKCR_CONTEXTMENUHANDLERS	L"\\shellex\\ContextMenuHandlers\\" APP_SIG

#define HKLM_CURRENTVERSION			L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion"
// The same path exists under HKCU; it is used there for per-user registration.
#define HKLM_APPROVED				HKLM_CURRENTVERSION L"\\Shell Extensions\\Approved"
#define HKLM_ICONOVERLAY			HKLM_CURRENTVERSION L"\\Explorer\\ShellIconOverlayIdentifiers\\" APP_SIG

		//Computer\HKEY_LOCAL_MACHINE\SOFTWARE\Classes\CLSID\{86ca1aa0-34aa-4e8b-a509-50c905bae2a2}
		//reg.exe add "HKCU\Software\Classes\CLSID\{86ca1aa0-34aa-4e8b-a509-50c905bae2a2}\InprocServer32" /f /ve
		// The per-user Windows 11 redirect built on that key lives in exe/src/Main.cpp.

		// Registration is written to, and removed from, one store explicitly:
		// HKLM\SOFTWARE\Classes or HKCU\Software\Classes. Never through the
		// merged HKEY_CLASSES_ROOT view: a write or delete through HKCR goes to
		// HKCU\Software\Classes whenever the key already exists there, so with
		// a per-user registration present, a machine register/unregister used to
		// land in (or delete) that user's entries instead of the machine's.
		//   https://learn.microsoft.com/windows/win32/sysinfo/hkey-classes-root-key
		//
		// Reads that decide whether Explorer will find us (IsRegistered(), used
		// by the DLL) still go through HKCR, because that is the view Explorer
		// itself resolves CLSIDs and handlers through.
	public:
		static constexpr REGSAM view = KEY_WOW64_64KEY;

		static HKEY Root(RegScope scope)
		{
			return scope == RegScope::User ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE;
		}

		// "Software\Classes\<subkey>", relative to Root().
		static string ClassesKey(const wchar_t *subkey)
		{
			string key = L"Software\\Classes\\";
			key += subkey;
			return key.move();
		}

		static string ClsidKey(const wchar_t *clsid)
		{
			string key = L"CLSID\\";
			key += clsid;
			return ClassesKey(key.c_str()).move();
		}

		static bool KeyExists(HKEY root, const wchar_t *subkey)
		{
			HKEY key{};
			if(ERROR_SUCCESS != ::RegOpenKeyExW(root, subkey, 0, KEY_QUERY_VALUE | view, &key))
				return false;
			::RegCloseKey(key);
			return true;
		}

		// Creates the key if needed and sets one REG_SZ value. Unlike
		// RegistryKey::SetString an empty string is allowed.
		static bool SetString(HKEY root, const wchar_t *subkey, const wchar_t *name, const wchar_t *value)
		{
			HKEY key{};
			if(ERROR_SUCCESS != ::RegCreateKeyExW(root, subkey, 0, nullptr, REG_OPTION_NON_VOLATILE,
												  KEY_SET_VALUE | view, nullptr, &key, nullptr))
				return false;
			auto bytes = static_cast<DWORD>((::wcslen(value) + 1) * sizeof(wchar_t));
			auto rc = ::RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE *>(value), bytes);
			::RegCloseKey(key);
			return rc == ERROR_SUCCESS;
		}

		// Reads a REG_SZ value; false if the key or value is missing.
		static bool GetString(HKEY root, const wchar_t *subkey, const wchar_t *name, string &value)
		{
			wchar_t buffer[MAX_PATH * 2]{};
			DWORD bytes = sizeof(buffer);
			if(ERROR_SUCCESS != ::RegGetValueW(root, subkey, name, RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY,
											   nullptr, buffer, &bytes))
				return false;
			value = buffer;
			return true;
		}

		// Deletes a key and everything below it, in the same registry view the
		// keys were created in. True if the key existed and is gone.
		static bool DeleteTree(HKEY root, const wchar_t *subkey)
		{
			HKEY key{};
			if(ERROR_SUCCESS != ::RegOpenKeyExW(root, subkey, 0,
												DELETE | KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE | KEY_SET_VALUE | view,
												&key))
				return false;
			::RegDeleteTreeW(key, nullptr);
			::RegCloseKey(key);
			return ERROR_SUCCESS == ::RegDeleteKeyExW(root, subkey, view, 0);
		}

		static bool DeleteValue(HKEY root, const wchar_t *subkey, const wchar_t *name)
		{
			HKEY key{};
			if(ERROR_SUCCESS != ::RegOpenKeyExW(root, subkey, 0, KEY_SET_VALUE | view, &key))
				return false;
			auto rc = ::RegDeleteValueW(key, name);
			::RegCloseKey(key);
			return rc == ERROR_SUCCESS;
		}

		// Deletes a key only if it has no subkeys and no values left.
		static bool DeleteKeyIfEmpty(HKEY root, const wchar_t *subkey)
		{
			HKEY key{};
			if(ERROR_SUCCESS != ::RegOpenKeyExW(root, subkey, 0, KEY_QUERY_VALUE | view, &key))
				return false;
			DWORD subkeys = 1, values = 1;
			auto rc = ::RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr,
										 &values, nullptr, nullptr, nullptr, nullptr);
			::RegCloseKey(key);
			if(rc != ERROR_SUCCESS || subkeys || values)
				return false;
			return ERROR_SUCCESS == ::RegDeleteKeyExW(root, subkey, view, 0);
		}

		public:
			static const wchar_t *GetKeyPath()
			{/*
#ifndef _WIN64
				//if(Is64BitWindows())
				return L"Software\\Classes\\Wow6432Node";
#endif
*/
				return L"Software\\Classes";
			}

			// Whether Explorer, running as the current user, will find the
			// handler: the merged HKCR view, i.e. either scope.
			static bool IsRegistered()
			{
				return IsContextMenu() || IsIconOverlay() || IsFolderExtensions();
			}

			// Whether a registration exists in one particular store.
			static bool IsRegistered(RegScope scope)
			{
				auto root = Root(scope);
				if(KeyExists(root, ClsidKey(CLS_ContextMenu)))
					return true;
				// Icon overlay and folder extensions are only ever registered
				// machine-wide.
				return scope == RegScope::Machine &&
					(KeyExists(root, ClsidKey(CLS_IconOverlay)) || KeyExists(root, ClsidKey(CLS_FolderExtensions)));
			}

			// "none", "user", "machine" or "both".
			static const wchar_t *RegistrationMode()
			{
				auto machine = IsRegistered(RegScope::Machine);
				auto user = IsRegistered(RegScope::User);
				if(machine && user) return L"both";
				if(machine) return L"machine";
				if(user) return L"user";
				return L"none";
			}

			// The DLL path a scope's context-menu CLSID points at (empty if none).
			static string InprocServerPath(RegScope scope)
			{
				string path;
				GetString(Root(scope), (ClsidKey(CLS_ContextMenu) + L"\\InprocServer32").c_str(), nullptr, path);
				return path.move();
			}

			static bool IsHKCR(const string &key)
			{
				return Registry::Exists(HKCR, L"CLSID\\" + key);
			}

			static bool IsContextMenu()
			{
				return IsHKCR(CLS_ContextMenu);
			}

			static bool IsIconOverlay()
			{
				return IsHKCR(CLS_IconOverlay);
			}

			static bool IsFolderExtensions()
			{
				return IsHKCR(CLS_FolderExtensions);
			}

			// register COM-object: add Software\Classes\CLSID\{<CLSID>} in the scope's store.
			static bool RegisterInprocServer(RegScope scope, const wchar_t *module, const wchar_t *clsid, const wchar_t *value)
			{
				auto root = Root(scope);
				auto key = ClsidKey(clsid).move();
				auto inproc = (key + L"\\InprocServer32").move();

				if(SetString(root, key, nullptr, value) &&
				   SetString(root, inproc, nullptr, module) &&
				   SetString(root, inproc, L"ThreadingModel", L"Apartment"))
					return true;

				DeleteTree(root, key);
				return false;
			}

			// Require admininstrator's rights!
			static bool Register(const wchar_t *dllPath, REGOP reg)
			{
				if(!dllPath || !dllPath[0])
					return false;

				if(!::IsWindows7OrGreater())
					return false;

				constexpr auto scope = RegScope::Machine;
				int ret = 0;

				if(reg.FOLDEREXTENSIONS)
				{
					if(RegisterInprocServer(scope, dllPath, CLS_FolderExtensions, APP_COMP_NAME))
					{
						ret++;
						auto key = Registry::LocalMachine.CreateSubKey(HKLM_DRIVE_FolderExtensions, false);
						if(key)
						{
							ret += key.SetString(nullptr, APP_COMP_NAME);
							ret += key.SetInt(L"DriveMask", 0xff);
							key.Close();
						}
						else
						{
							DeleteTree(HKEY_LOCAL_MACHINE, ClsidKey(CLS_FolderExtensions));
							ret = 0;
						}
					}
				}

				if(reg.ICONOVERLAY)
				{
					// register COM-object for overlay icon handler
					if(RegisterInprocServer(scope, dllPath, CLS_IconOverlay, APP_COMP_NAME))
					{
						// register overlay icon handler
						ret += Registry::LocalMachine.SetString(HKLM_ICONOVERLAY, nullptr, CLS_IconOverlay, false);
					}
				}

				if(reg.CONTEXTMENU)
				{
					// register COM-object for shortcut menu handler
					if(RegisterInprocServer(scope, dllPath, CLS_ContextMenu, APP_COMP_NAME))
					{
						if(Registry::LocalMachine.SetString(HKLM_APPROVED, CLS_ContextMenu, APP_SIG, false))
						{
							//register shortcut menu handler
							ret += RegisterContextMenuHandler(scope, true);
						}
					}
				}

				if(ret > 0)
				{
					RegisterNss(scope);
					return true;
				}
				return false;
			}

			// Per-user registration. Writes only below HKEY_CURRENT_USER, so it
			// needs no elevation. Covers the context-menu handler for the same
			// classes as machine mode and the .nss association; the icon overlay
			// and folder extensions are machine-only (HKLM) and are skipped.
			static bool RegisterUser(const wchar_t *dllPath)
			{
				if(!dllPath || !dllPath[0])
					return false;

				if(!::IsWindows7OrGreater())
					return false;

				constexpr auto scope = RegScope::User;

				if(!RegisterInprocServer(scope, dllPath, CLS_ContextMenu, APP_COMP_NAME))
					return false;

				if(!RegisterContextMenuHandler(scope, true))
				{
					UnregisterUser();
					return false;
				}

				// Only consulted when the EnforceShellExtensionSecurity policy is
				// set; harmless otherwise. Best effort.
				SetString(HKEY_CURRENT_USER, HKLM_APPROVED, CLS_ContextMenu, APP_SIG);

				RegisterNss(scope);
				return true;
			}

			// Require administrator's rights!
			static bool Unregister()
			{
				constexpr auto scope = RegScope::Machine;
				auto root = Root(scope);
				int ret = 0;

				ret += DeleteTree(root, ClsidKey(CLS_FolderExtensions));
				//ret += Registry::DeleteSubKey(HKLM, HKLM_DRIVE_FolderExtensions);

				ret += Registry::DeleteSubKey(HKLM, HKLM_ICONOVERLAY);
				// unregister COM-object Delete the CLSID\{<CLSID>} key.
				ret += DeleteTree(root, ClsidKey(CLS_IconOverlay));

				ret += Registry::DeleteKeyValue(HKLM, HKLM_APPROVED, CLS_ContextMenu);
				// unregister COM-object Delete the CLSID\{<CLSID>} key.
				ret += DeleteTree(root, ClsidKey(CLS_ContextMenu));
				ret += RegisterContextMenuHandler(scope, false);

				/*if(Windows::Version::Instance().IsWindows11OrGreater())
				{
					string key_treatas;
					key_treatas.format(L"CLSID\\%s\\TreatAs", string::ToString(IID_FileExplorerContextMenu, 2).c_str());
					Registry::DeleteSubKey(HKCR, key_treatas);
				}
				*/
				UnregisterNss(scope);

				return ret > 0;// == 5;
			}

			// Removes everything RegisterUser() wrote. Never touches HKLM.
			static bool UnregisterUser()
			{
				constexpr auto scope = RegScope::User;
				int ret = 0;
				ret += DeleteValue(HKEY_CURRENT_USER, HKLM_APPROVED, CLS_ContextMenu);
				ret += DeleteTree(Root(scope), ClsidKey(CLS_ContextMenu));
				ret += RegisterContextMenuHandler(scope, false);
				UnregisterNss(scope);
				return ret > 0;
			}

			// NOTE: The function add or removes the {{<CLSID>}} key under
			// Software\Classes\<type>\shellex\ContextMenuHandlers in the scope's store.
			static bool RegisterContextMenuHandler(RegScope scope, const wchar_t *fileType, bool bRegister)
			{
				if(!fileType || !fileType[0])
					return false;

				wchar_t default_value[MAX_PATH] { 0 };
				if(*fileType == L'.')
				{
					// Resolved through the merged view: the ProgID Explorer will
					// actually use for the extension.
					auto key = Registry::ClassesRoot.OpenSubKey(fileType);
					// If the key exists and its default value is not empty,
					// use the ProgID as the file type.
					if(key)
					{
						if(key.GetString(nullptr, default_value, MAX_PATH))
							fileType = default_value;
						key.Close();
					}
				}

				string subKey = fileType;
				subKey += HKCR_CONTEXTMENUHANDLERS;
				subKey = ClassesKey(subKey.c_str()).move();
				// key Software\Classes\<Types>\shellex\ContextMenuHandlers\{{<CLSID>}}
				// Create
				if(bRegister)
					return SetString(Root(scope), subKey, nullptr, CLS_ContextMenu);
				// Remove
				return DeleteTree(Root(scope), subKey);
			}

			// Update Registry
			// true	 = Register the context menu handler.
			// false = Unregister the context menu handler.
			static bool RegisterContextMenuHandler(RegScope scope, bool _register)
			{
				int res = 0;
				// The context menu handler is associated with the classes.
				res += RegisterContextMenuHandler(scope, L"*", _register);
				res += RegisterContextMenuHandler(scope, L"Directory", _register);
				//res += RegisterContextMenuHandler(scope, L"AllFileSystemObjects", _register);
				res += RegisterContextMenuHandler(scope, L"Drive", _register);
				res += RegisterContextMenuHandler(scope, L"Folder", _register);
				res += RegisterContextMenuHandler(scope, L"Directory\\Background", _register);
				res += RegisterContextMenuHandler(scope, L"DesktopBackground", _register);
				res += RegisterContextMenuHandler(scope, L"LibraryFolder", _register);
				res += RegisterContextMenuHandler(scope, L"LibraryFolder\\Background", _register);
				return res > 0;
			}

			static constexpr auto NssOpenCommand = L"notepad \"%1\"";

			// .nss: plain text, opened with Notepad.
			static void RegisterNss(RegScope scope)
			{
				auto root = Root(scope);
				auto nss = ClassesKey(L".nss").move();
				SetString(root, nss, L"Content Type", L"text/plain");
				SetString(root, (nss + L"\\shell\\open\\command").c_str(), nullptr, NssOpenCommand);
			}

			static void UnregisterNss(RegScope scope)
			{
				auto root = Root(scope);
				auto nss = ClassesKey(L".nss").move();

				if(scope == RegScope::Machine)
				{
					DeleteTree(root, nss);
					return;
				}

				// HKCU\Software\Classes\.nss may also hold the user's own choices
				// (OpenWithProgids, an editor's ProgID), so only what RegisterNss
				// wrote is removed, and keys only once nothing else is left.
				auto open = (nss + L"\\shell\\open").move();
				auto command = (open + L"\\command").move();
				string value;
				if(GetString(root, command, nullptr, value) && value.equals(NssOpenCommand))
				{
					DeleteValue(root, command, nullptr);
					DeleteKeyIfEmpty(root, command);
				}
				DeleteKeyIfEmpty(root, open);
				DeleteKeyIfEmpty(root, (nss + L"\\shell").c_str());

				if(GetString(root, nss, L"Content Type", value) && value.equals(L"text/plain"))
					DeleteValue(root, nss, L"Content Type");
				DeleteKeyIfEmpty(root, nss);
			}

			static bool get(const wchar_t* subkey, const wchar_t *name, string &value)
			{
				string _subkey = APP_KEY;
				if(subkey)
				{
					if(subkey[0] != L'\\')
						_subkey += L"\\";
					_subkey += subkey;
				}

				if(auto key = Registry::CurrentUser.OpenSubKey(_subkey, true, false); key)
					return key.ReadString(name, value);
				return false;
			}

			static bool get(const wchar_t *subkey, const wchar_t *name, int &value)
			{
				string _subkey = APP_KEY;
				if(subkey)
				{
					if(subkey[0] != L'\\')
						_subkey += L"\\";
					_subkey += subkey;
				}

				if(auto key = Registry::CurrentUser.OpenSubKey(_subkey, true, false); key)
				{
					value = key.GetInt(name, value);
					return true;
				}
				return false;
			}
		};
	}
