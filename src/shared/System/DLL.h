#pragma once

namespace Nilesoft
{
	class DLL
	{
		HMODULE m_handle = nullptr;
		const wchar_t* m_dllPath = nullptr;
		bool loadLibrary = false;

	public:
		DLL() noexcept {}
		DLL(HMODULE handel) :m_handle(handel) {}
		DLL(const wchar_t* dllPath, bool load_as_data = false) 
			: m_dllPath(dllPath), m_handle(nullptr) 
		{
			load(load_as_data);
		}
		~DLL()
		{
			if(loadLibrary && m_handle)
				::FreeLibrary(m_handle);
		}

		HMODULE handel() { return m_handle; }
		operator HMODULE() { return m_handle; }

		explicit operator bool() const { return m_handle != nullptr; }

		/*
			Loads a module without giving the search path (and in particular the
			current directory) a chance to supply it.

			Every bare name this codebase loads is a Windows module: kernel32,
			user32, ntdll, shell32, shlwapi, uxtheme, OleAut32, Shcore, Comdlg32,
			winbrand, ... A plain LoadLibrary of a bare name uses the standard
			search order, which is what a DLL preloading attack needs - and this
			code runs inside explorer.exe and every process that shows a shell
			menu or file dialog.

			Microsoft's mitigations are to use the LOAD_LIBRARY_SEARCH flags with
			LoadLibraryEx, or to specify a fully qualified path. This does the
			first and falls back to the second (not to an unqualified load):
			LOAD_LIBRARY_SEARCH_SYSTEM32 needs KB2533623 on Windows 7 and fails
			there with ERROR_INVALID_PARAMETER.

			  https://learn.microsoft.com/windows/win32/dlls/dynamic-link-library-security

			A name that contains a path is a file the caller chose (e.g. an icon
			source) and is loaded exactly as given.
		*/
		static HMODULE LoadSafe(const wchar_t *name, DWORD flags = 0)
		{
			if(!name || !*name)
				return nullptr;

			if(::wcspbrk(name, L"\\/:"))
				return ::LoadLibraryExW(name, nullptr, flags);

			if(auto module = ::LoadLibraryExW(name, nullptr, flags | LOAD_LIBRARY_SEARCH_SYSTEM32))
				return module;

			if(::GetLastError() != ERROR_INVALID_PARAMETER)
				return nullptr;

			wchar_t path[MAX_PATH]{};
			auto length = ::GetSystemDirectoryW(path, ARRAYSIZE(path));
			return QualifiedLoad(path, length, name, flags);
		}

		/*
			The same guarantee for Windows binaries that are not in System32,
			e.g. regedit.exe and explorer.exe (Windows directory) or
			System32\WindowsPowerShell\v1.0\powershell.exe. `relative` is resolved
			against the Windows directory, so LoadLibraryEx still receives a fully
			qualified path that was never searched for.
		*/
		static HMODULE LoadSafeWindows(const wchar_t *relative, DWORD flags = 0)
		{
			if(!relative || !*relative)
				return nullptr;

			wchar_t path[MAX_PATH]{};
			auto length = ::GetWindowsDirectoryW(path, ARRAYSIZE(path));
			return QualifiedLoad(path, length, relative, flags);
		}

	private:
		// `dir` holds `length` characters from GetSystemDirectory/GetWindowsDirectory.
		static HMODULE QualifiedLoad(wchar_t (&dir)[MAX_PATH], UINT length, const wchar_t *name, DWORD flags)
		{
			if(length == 0 || length >= MAX_PATH)
				return nullptr;

			// Documented to omit the trailing separator except at a drive root.
			if(dir[length - 1] != L'\\')
			{
				if(length + 1 >= MAX_PATH)
					return nullptr;
				dir[length++] = L'\\';
			}

			for(auto p = name; *p; ++p)
			{
				if(length + 1 >= MAX_PATH)
					return nullptr;
				dir[length++] = *p;
			}
			dir[length] = 0;

			return ::LoadLibraryExW(dir, nullptr, flags);
		}

	public:
		bool load(bool load_as_data = false)
		{
			if(m_handle)
				return true;

			if(m_dllPath)
			{
				m_handle = ::GetModuleHandleW(m_dllPath);
				if(!m_handle)
				{
					m_handle = LoadSafe(m_dllPath, load_as_data ? LOAD_LIBRARY_AS_DATAFILE : 0);

					loadLibrary = true;
				}
				return m_handle != nullptr;
			}
			return false;
		}

		void unload()
		{
			if(m_handle && loadLibrary)
			{
				loadLibrary = false;
				::FreeLibrary(m_handle);
			}
			m_handle = nullptr;
		}

		bool is_func(const char* lpProcName)
		{
			if(m_handle)
				return ::GetProcAddress(m_handle, lpProcName) != nullptr;
			return false;
		}

		template<typename RET = int, typename... Args>
		RET invoke(const char *lpProcName, Args... arguments)
		{
			RET ret{};
			if(m_handle && lpProcName)
			{
				if(auto lpFunc = ::GetProcAddress(m_handle, lpProcName); lpFunc)
				{
					using func_call = RET(__stdcall *)(Args... args);
					ret = ((func_call)lpFunc)(arguments...);
				}
			}
			return ret;
		}

		static bool Load(const wchar_t* dllPath, HMODULE &hModule)
		{
			hModule = LoadSafe(dllPath);
			return (hModule != nullptr);
		}

		static bool Unload(HMODULE hModule)
		{
			if(hModule)
				return !!::FreeLibrary(hModule);
			return false;
		}

		template<typename T>
		T Get(const char* lpProcName)
		{
			if(lpProcName && m_handle)
				return (T)::GetProcAddress(m_handle, lpProcName);
			return nullptr;
		}

		template<typename T>
		T Get(int ordinal)
		{
			if(m_handle)
				return (T)::GetProcAddress(m_handle, MAKEINTRESOURCEA(ordinal));
			return nullptr;
		}

		template<typename T>
		bool Get(const char* lpProcName, T* lpFunc)
		{
			if(lpFunc)
			{
				*lpFunc = Get<T>(lpProcName);
				return *lpFunc != nullptr;
			}
			return false;
		}

		template<typename T>
		static T Get(const wchar_t* dllPath, const char* lpProcName)
		{
			T lpFunc = nullptr;
			if(dllPath && lpProcName)
			{
				auto hModule = ::GetModuleHandleW(dllPath);
				if(hModule)
					lpFunc = (T)::GetProcAddress(hModule, lpProcName);
				else
				{
					hModule = LoadSafe(dllPath);
					if(hModule)
					{
						lpFunc = (T)::GetProcAddress(hModule, lpProcName);
						::FreeLibrary(hModule);
					}
				}
			}
			return lpFunc;
		}

		template<typename T>
		static bool Get(const wchar_t* dllPath, const char* lpProcName, T* lpFunc)
		{
			if(lpFunc)
			{
				*lpFunc = Get<T>(dllPath, lpProcName);
				return *lpFunc != nullptr;
			}
			return false;
		}

		template<typename T>
		static T Get(HMODULE hModule, const char* lpProcName)
		{
			if(hModule && lpProcName)
				return reinterpret_cast<T>(::GetProcAddress(hModule, lpProcName));
			return nullptr;
		}

		template<typename T>
		static bool Get(HMODULE hModule, const char* lpProcName, T* lpFunc)
		{
			if(hModule && lpProcName && lpFunc)
			{
				*lpFunc = (T)::GetProcAddress(hModule, lpProcName);
				return *lpFunc != nullptr;
			}
			return false;
		}

		static bool IsFunc(HMODULE hModule, const char *lpProcName)
		{
			if(hModule)
				return ::GetProcAddress(hModule, lpProcName) != nullptr;
			return false;
		}

		static bool IsFunc(HMODULE hModule, uint32_t ordinal)
		{
			if(hModule)
				return ::GetProcAddress(hModule, MAKEINTRESOURCEA(ordinal)) != nullptr;
			return false;
		}

		template<typename RET=int, typename... Args>
		static RET Invoke(HMODULE hModule, const char *lpProcName, Args... arguments)
		{
			RET ret{};
			if(hModule && lpProcName)
			{
				if(auto lpFunc = ::GetProcAddress(hModule, lpProcName); lpFunc)
				{
					using func_call = RET(__stdcall *)(Args... args);
					ret = ((func_call)lpFunc)(arguments...);
				}
			}
			return ret;
		}

		template<typename RET = int, typename... Args>
		static RET Invoke(HMODULE hModule, uint32_t ordinal, Args... arguments)
		{
			RET ret{};
			try {
				ret = Invoke<RET>(hModule, MAKEINTRESOURCEA(ordinal), arguments...);
			}
			catch(...) {
			}
			return ret;
		}

		/*template<typename... Args>
		static void Invoke(HMODULE hModule, const char *lpProcName, Args... arguments)
		{
			if(hModule && lpProcName)
			{
				if(auto lpFunc = ::GetProcAddress(hModule, lpProcName))
				{
					using func_call = RET(__stdcall *)(Args... args);
					((func_call)lpFunc)(arguments...);
				}
			}
		}*/

		template<typename... Args>
		static void Invoke(const wchar_t*name, const char* lpProcName, Args... arguments)
		{
			if(name && lpProcName)
			{
				bool freeLibrary = false;
				auto hModule = ::GetModuleHandleW(name);
				if(!hModule)
				{
					hModule = LoadSafe(name);
					freeLibrary = true;
				}

				if(hModule)
				{
					if(auto lpFunc = ::GetProcAddress(hModule, lpProcName); lpFunc)
					{
						using func_call = void(__stdcall*)(Args... args);
						(func_call(lpFunc))(arguments...);
					}
					if(freeLibrary)
						::FreeLibrary(hModule);
				}
			}
		}

		template<typename Result, typename... Args>
		static Result Invoke(const wchar_t* name, const char* lpProcName, Args... arguments)
		{
			Result res{};
			if(name && lpProcName)
			{
				bool freeLibrary = false;
				auto hModule = ::GetModuleHandleW(name);
				if(!hModule)
				{
					hModule = LoadSafe(name);
					freeLibrary = true;
				}

				if(hModule)
				{
					using func_call = Result(__stdcall*)(Args... args);
					if(auto lpFunc = (func_call)::GetProcAddress(hModule, lpProcName); lpFunc)
					{
						res = lpFunc(arguments...);
					}
					if(freeLibrary)
						::FreeLibrary(hModule);
				}
			}
			return res;
		}

		
		static constexpr auto kernel32 = L"kernel32.dll";
		static constexpr auto user32 = L"user32.dll";

		template<typename Result, typename... Args>
		static Result Kernel32(const char *lpProcName, Args... arguments)
		{
			Result res {};
			if(lpProcName)
			{
				bool freeLibrary = false;
				auto hModule = ::GetModuleHandleW(kernel32);
				if(!hModule)
				{
					hModule = LoadSafe(kernel32);
					freeLibrary = true;
				}

				if(hModule)
				{
					using func_call = Result(__stdcall *)(Args... args);
					if(auto lpFunc = (func_call)::GetProcAddress(hModule, lpProcName); lpFunc)
					{
						res = lpFunc(arguments...);
					}

					if(freeLibrary)
						::FreeLibrary(hModule);
				}
			}
			return res;
		}

		template<typename Result, typename... Args>
		static Result User32(const char *lpProcName, Args... arguments)
		{
			Result res {};
			if(lpProcName)
			{
				bool freeLibrary = false;
				auto hModule = ::GetModuleHandleW(user32);
				if(!hModule)
				{
					hModule = LoadSafe(user32);
					freeLibrary = true;
				}

				if(hModule)
				{
					using func_call = Result(__stdcall *)(Args... args);
					if(auto lpFunc = (func_call)::GetProcAddress(hModule, lpProcName); lpFunc)
					{
						res = lpFunc(arguments...);
					}

					if(freeLibrary)
						::FreeLibrary(hModule);
				}
			}
			return res;
		}
	};
}