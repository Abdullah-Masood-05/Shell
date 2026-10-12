#pragma once

namespace Nilesoft 
{
	namespace Security
	{
		class Permission
		{
		public:
			// SetRegistry(HKEY) and SetRegistry(HKEY, subkey) used to live here.
			// They granted BUILTIN\Users KEY_ALL_ACCESS, container+object inherit,
			// on any key they were given, and nothing called them; removed rather
			// than left for the next caller who needs a write to succeed.

			// Grants BUILTIN\Users read/write (no WRITE_DAC, WRITE_OWNER or
			// DELETE, no inheritance) on one existing file. Must never be used on
			// a directory or on anything Explorer loads code from.
			static bool SetFile(const wchar_t* path)
			{
				auto_handle hFile = ::CreateFileW(path, READ_CONTROL | WRITE_DAC,
												  0, nullptr, OPEN_EXISTING,
												  FILE_ATTRIBUTE_NORMAL, nullptr);
				if(hFile)
				{
					return SetFile(hFile);
				}
				return false;
			}

			static bool SetFile(HANDLE hFile)
			{

				if(hFile == INVALID_HANDLE_VALUE)
					return false;

				struct dest
				{
					PSID sid = nullptr;
					PACL dacl = nullptr;
					PSECURITY_DESCRIPTOR sd = nullptr;

					~dest()
					{
						if(sid) ::FreeSid(sid);
						if(dacl) ::LocalFree(dacl);
						if(sd) ::LocalFree(sd);
					}
				}d;

				DWORD dwRes;
				PACL pOldDACL = nullptr;
				EXPLICIT_ACCESSW ea = { 0 };
				SID_IDENTIFIER_AUTHORITY SIDAuthNT = SECURITY_NT_AUTHORITY;

				dwRes = ::GetSecurityInfo(hFile, SE_FILE_OBJECT,
										  DACL_SECURITY_INFORMATION, nullptr, nullptr, &pOldDACL, nullptr, &d.sd);
				if(dwRes != ERROR_SUCCESS)
					return false;

				// BUILTIN\Users.
				if(!::AllocateAndInitializeSid(&SIDAuthNT, 2,
											   SECURITY_BUILTIN_DOMAIN_RID,
											   DOMAIN_ALIAS_RID_USERS, 0, 0, 0, 0, 0, 0, &d.sid))
				{
					return false;
				}

				// Only what a non-elevated Explorer needs to append to the log
				// file this is called on (Log.cpp, right after creating it). This
				// used to be GENERIC_ALL - which includes WRITE_DAC, WRITE_OWNER and
				// DELETE - with container+object inheritance, so on a directory it
				// would hand every user control of everything created inside it.
				ea.grfAccessPermissions = FILE_GENERIC_READ | FILE_GENERIC_WRITE;
				ea.grfAccessMode = GRANT_ACCESS;
				ea.grfInheritance = NO_INHERITANCE;
				ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
				ea.Trustee.TrusteeType = TRUSTEE_IS_GROUP;
				ea.Trustee.ptstrName = (wchar_t*)d.sid;

				// Create a new ACL that contains the new ACEs.
				if(ERROR_SUCCESS != ::SetEntriesInAclW(1, &ea, pOldDACL, &d.dacl))
					return false;

				if(d.dacl)
				{
					return ERROR_SUCCESS == ::SetSecurityInfo(
						hFile, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, d.dacl, nullptr);
				}

				return false;
			}
		};
	}
}