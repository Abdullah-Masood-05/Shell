#pragma once

#include "doctest.h"
#include "nss/nss.h"

#include <windows.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace test
{
	namespace fs = std::filesystem;

	inline fs::path binDir()
	{
		return fs::path(NSS_SOURCE_BIN).lexically_normal();
	}

	inline std::vector<fs::path> corpus()
	{
		std::vector<fs::path> files;
		for(auto &e : fs::recursive_directory_iterator(binDir()))
			if(e.is_regular_file() && e.path().extension() == ".nss")
				files.push_back(e.path());
		std::sort(files.begin(), files.end());
		return files;
	}

	inline std::string readBytes(const fs::path &p)
	{
		std::string s;
		nss::readFile(p, s);
		return s;
	}

	inline void writeBytes(const fs::path &p, std::string_view bytes)
	{
		fs::create_directories(p.parent_path());
		std::ofstream f(p, std::ios::binary | std::ios::trunc);
		f.write(bytes.data(), std::streamsize(bytes.size()));
	}

	// A fresh folder under %TEMP%, removed when the test ends.
	struct TempDir
	{
		fs::path path;

		TempDir()
		{
			static std::atomic<int> n{ 0 };
			path = fs::temp_directory_path() /
				(L"nsscore-test-" + std::to_wstring(::GetCurrentProcessId()) + L"-" + std::to_wstring(++n));
			fs::remove_all(path);
			fs::create_directories(path);
		}
		~TempDir()
		{
			std::error_code ec;
			fs::remove_all(path, ec);
		}
		fs::path operator/(const fs::path &p) const { return path / p; }
	};

	// Copies the shipped config tree (shell.nss and imports/) into `dir`.
	inline void copyCorpus(const fs::path &dir)
	{
		fs::copy(binDir() / "shell.nss", dir / "shell.nss");
		fs::copy(binDir() / "imports", dir / "imports", fs::copy_options::recursive);
	}
}
