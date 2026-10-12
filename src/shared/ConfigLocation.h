#pragma once

#include "RegistryConfig.h"

namespace Nilesoft
{
	/*
		Where shell.nss is loaded from.

		Shared by shell.dll, which only ever reads the file, and shell.exe,
		which reports it (-status, -config:path) and seeds the per-user copy
		(-config:init, -config:reset), so the two always agree.

		Resolution order, first match wins:
		  1. HKCU\Software\Nilesoft\Shell, value "config", if that file exists.
		     A relative path is rooted at the install folder.
		  2. %APPDATA%\Nilesoft\Shell\shell.nss, if it exists.
		  3. <install folder>\shell.nss, the folder holding shell.dll/shell.exe.

		Imports resolve relative to the importing file, so a per-user shell.nss
		uses the imports\ folder next to it.
	*/
	struct ConfigLocation
	{
		enum class Source { registry, appdata, install, none };

		static constexpr auto FileName = L"shell.nss";
		static constexpr auto ImportsName = L"imports";

		// The file to load. With Source::none nothing exists and this is the
		// install-folder path, i.e. where a config would be looked for last.
		string path;
		Source source = Source::none;
		// The HKCU "config" value (rooted), if set, whether or not it exists.
		string configured;

		static const wchar_t *SourceName(Source source)
		{
			switch(source)
			{
				case Source::registry: return L"registry";
				case Source::appdata: return L"appdata";
				case Source::install: return L"install";
				default: return L"none";
			}
		}

		static bool IsFile(const wchar_t *path)
		{
			if(!path || !*path)
				return false;
			auto attr = ::GetFileAttributesW(path);
			return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
		}

		static string Join(const wchar_t *dir, const wchar_t *name)
		{
			string path = dir;
			if(!path.empty() && path.back() != L'\\' && path.back() != L'/')
				path += L"\\";
			path += name;
			return path.move();
		}

		// %APPDATA%\Nilesoft\Shell, or empty if it cannot be determined.
		// The APPDATA environment variable is used when set, so a process
		// started with a different APPDATA (a test run, a portable launcher)
		// resolves the same folder as the tools it starts; otherwise the
		// RoamingAppData known folder.
		static string UserDirectory()
		{
			string dir;
			if(auto len = ::GetEnvironmentVariableW(L"APPDATA", nullptr, 0); len > 1)
			{
				string buffer;
				auto written = ::GetEnvironmentVariableW(L"APPDATA", buffer.buffer(len), len);
				if(written > 0 && written < len)
					dir = buffer.release(written).move();
			}

			if(dir.empty())
			{
				PWSTR known = nullptr;
				if(SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_DONT_VERIFY, nullptr, &known)) && known)
					dir = known;
				::CoTaskMemFree(known);
			}

			if(dir.empty())
				return dir;
			return Join(dir, APP_COMPANY L"\\" APP_NAME).move();
		}

		// %APPDATA%\Nilesoft\Shell\shell.nss, or empty.
		static string UserConfig()
		{
			auto dir = UserDirectory().move();
			if(dir.empty())
				return dir;
			return Join(dir, FileName).move();
		}

		static ConfigLocation Resolve(const wchar_t *install_dir)
		{
			ConfigLocation location;

			string cfg;
			if(RegistryConfig::get(nullptr, L"config", cfg) && !cfg.empty())
			{
				// Rooted explicitly: the working directory is not ours to rely on.
				if(!IO::Path::IsAbsolute(cfg))
					cfg = IO::Path::Combine(install_dir, cfg).move();
				location.configured = cfg;
				if(IsFile(cfg))
				{
					location.path = cfg.move();
					location.source = Source::registry;
					return location;
				}
			}

			if(auto user = UserConfig().move(); IsFile(user))
			{
				location.path = user.move();
				location.source = Source::appdata;
				return location;
			}

			location.path = Join(install_dir, FileName).move();
			location.source = IsFile(location.path) ? Source::install : Source::none;
			return location;
		}
	};
}
