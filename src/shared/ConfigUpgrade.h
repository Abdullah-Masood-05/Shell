#pragma once

/*
	Brings a per-user config folder (%APPDATA%\Nilesoft\Shell, or the folder of the
	HKCU "config" file) up to date with the files shipped in the install folder.

	Updates (MSI, portable zip, the app's updater) only replace the install folder, so
	the copies that `shell -config:init` made earlier would stay as they were forever.
	For every shipped file (shell.nss and imports\**\*.nss):

	  - the user's copy is missing: copied when the user's config refers to it
	    (its file name appears in one of the user's .nss files; for imports\lang\*.nss,
	    when they mention imports\lang), else left out;
	  - the copy is the same as the shipped file: nothing to do ("unchanged");
	  - the copy is one of the versions shipped before (ShippedConfigHashes.inc, every
	    version in the git history; see scripts/update-shipped-hashes.ps1): the user never
	    edited it, so it is replaced ("updated");
	  - anything else was edited by the user and is left alone ("modified_kept"), unless
	    it is named in Options::take (the app's "Use the new version").

	Files are compared after normalize(): BOM, line endings and trailing blanks don't
	count, nor the app's own `import 'imports/settings-app*.nss'` lines or the
	localization block -config:init adds to an old shell.nss. A replaced shell.nss keeps
	the app's import lines (at its end, as the app writes them). Files the app owns
	(imports\settings-app*.nss, and the snippet copies in imports\snippets\) and files the
	install folder doesn't have are never touched.

	Before anything is written, the whole config tree is snapshotted into
	<config folder>\backups\<YYYYMMDD-HHMMSS>\ with a manifest.json, the format of the
	app's Backups page (src/settings/nss: ConfigStore), so the old files can be compared
	and restored there.

	Used by shell.exe (-config:upgrade, -config:init) and by the app's tests. Only the
	C++ standard library and Win32 here: no Nilesoft string types.
*/

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace config_upgrade
{
	struct Known
	{
		const char *path;  // relative to the config folder, '/' separated, lower case
		uint64_t hash;     // fingerprint()
	};

	inline constexpr Known known_hashes[] = {
#include "ShippedConfigHashes.inc"
	};

	// --- text ---------------------------------------------------------------------------

	inline char ascii_lower(char c)
	{
		return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
	}

	inline std::string to_lower(std::string_view s)
	{
		std::string out(s);
		for(auto &c : out)
			c = ascii_lower(c);
		return out;
	}

	inline bool starts_with(std::string_view s, std::string_view prefix)
	{
		return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
	}

	// `import 'imports/settings-app.nss'` (or "...", either slash, any case), alone on its
	// line: the line the app adds to the root shell.nss for each file it owns.
	inline bool is_app_import_line(std::string_view line)
	{
		auto l = to_lower(line);
		std::string_view s(l);
		while(!s.empty() && (s.front() == ' ' || s.front() == '\t'))
			s.remove_prefix(1);
		while(!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
			s.remove_suffix(1);
		if(!starts_with(s, "import"))
			return false;
		s.remove_prefix(6);
		if(s.empty() || (s.front() != ' ' && s.front() != '\t'))
			return false;
		while(!s.empty() && (s.front() == ' ' || s.front() == '\t'))
			s.remove_prefix(1);
		if(s.size() < 2 || (s.front() != '\'' && s.front() != '"') || s.back() != s.front())
			return false;
		auto path = s.substr(1, s.size() - 2);
		if(path.find_first_of("'\"") != path.npos)
			return false;
		if(!starts_with(path, "imports/settings-app") && !starts_with(path, "imports\\settings-app"))
			return false;
		return path.size() >= 4 && path.substr(path.size() - 4) == ".nss";
	}

	// The comparison form of a config file (scripts/update-shipped-hashes.ps1 does the
	// same, byte for byte):
	//   - a UTF-8 BOM at the start is dropped; CRLF and lone CR become LF;
	//   - spaces and tabs at the end of each line are dropped;
	//   - the block -config:init adds ("// Added by shell -config:init" through
	//     "// or import lang"), with one blank line next to it, is dropped;
	//   - is_app_import_line() lines are dropped;
	//   - blank lines at the end are dropped; the lines are joined with LF.
	inline std::string normalize(std::string_view bytes)
	{
		if(starts_with(bytes, "\xEF\xBB\xBF"))
			bytes.remove_prefix(3);

		std::vector<std::string> lines(1);
		for(size_t i = 0; i < bytes.size(); i++)
		{
			char c = bytes[i];
			if(c == '\r' || c == '\n')
			{
				if(c == '\r' && i + 1 < bytes.size() && bytes[i + 1] == '\n')
					i++;
				lines.emplace_back();
			}
			else
				lines.back() += c;
		}
		for(auto &l : lines)
		{
			while(!l.empty() && (l.back() == ' ' || l.back() == '\t'))
				l.pop_back();
		}

		for(size_t start = 0; start < lines.size(); start++)
		{
			if(!starts_with(lines[start], "// Added by shell -config:init"))
				continue;
			for(size_t end = start + 1; end < lines.size(); end++)
			{
				if(!starts_with(lines[end], "// or import lang"))
					continue;
				size_t from = start, count = end - start + 1;
				if(from > 0 && lines[from - 1].empty())
				{
					from--;
					count++;
				}
				else if(end + 1 < lines.size() && lines[end + 1].empty())
					count++;
				lines.erase(lines.begin() + static_cast<ptrdiff_t>(from), lines.begin() + static_cast<ptrdiff_t>(from + count));
				break;
			}
			break;
		}

		std::vector<std::string> kept;
		for(auto &l : lines)
		{
			if(!is_app_import_line(l))
				kept.push_back(std::move(l));
		}
		while(!kept.empty() && kept.back().empty())
			kept.pop_back();

		std::string out;
		for(size_t i = 0; i < kept.size(); i++)
		{
			if(i)
				out += '\n';
			out += kept[i];
		}
		return out;
	}

	// SHA-256 (BCrypt). False if the provider failed.
	inline bool sha256(std::string_view bytes, unsigned char (&hash)[32])
	{
		BCRYPT_ALG_HANDLE alg = nullptr;
		if(!BCRYPT_SUCCESS(::BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
			return false;
		auto status = ::BCryptHash(alg, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char *>(bytes.data())),
								   static_cast<ULONG>(bytes.size()), hash, sizeof(hash));
		::BCryptCloseAlgorithmProvider(alg, 0);
		return BCRYPT_SUCCESS(status);
	}

	inline std::string sha256_hex(std::string_view bytes)
	{
		unsigned char hash[32]{};
		sha256(bytes, hash);
		static constexpr char hex[] = "0123456789abcdef";
		std::string out;
		for(auto b : hash)
		{
			out += hex[b >> 4];
			out += hex[b & 0xF];
		}
		return out;
	}

	// The first 8 bytes (big-endian) of the SHA-256 of normalize(bytes).
	inline uint64_t fingerprint(std::string_view bytes)
	{
		unsigned char hash[32]{};
		sha256(normalize(bytes), hash);
		uint64_t value = 0;
		for(int i = 0; i < 8; i++)
			value = (value << 8) | hash[i];
		return value;
	}

	// `path` ('/' separated, any case) had this fingerprint in a shipped version.
	inline bool is_known(std::string_view path, uint64_t hash)
	{
		auto key = to_lower(path);
		for(auto &k : known_hashes)
		{
			if(k.hash == hash && key == k.path)
				return true;
		}
		return false;
	}

	// Two relative paths name the same file (any case, either slash).
	inline bool same_rel(std::string_view a, std::string_view b)
	{
		if(a.size() != b.size())
			return false;
		for(size_t i = 0; i < a.size(); i++)
		{
			char x = ascii_lower(a[i] == '\\' ? '/' : a[i]);
			char y = ascii_lower(b[i] == '\\' ? '/' : b[i]);
			if(x != y)
				return false;
		}
		return true;
	}

	// imports/settings-app*.nss and the snippet copies in imports/snippets/ (the app's
	// Snippets page): the app's own files.
	inline bool is_app_owned(std::string_view path)
	{
		auto key = to_lower(path);
		for(auto &c : key)
			if(c == '\\') c = '/';
		return starts_with(key, "imports/settings-app") || starts_with(key, "imports/snippets/");
	}

	// Whether the user's config refers to `path` (see the top of the file). `texts` are
	// the user's .nss files, lower case.
	inline bool is_referenced(std::string_view path, const std::vector<std::string> &texts)
	{
		auto key = to_lower(path);
		for(auto &c : key)
			if(c == '\\') c = '/';
		if(key == "shell.nss")
			return false;  // the root: never copied in by this

		if(starts_with(key, "imports/lang/"))
		{
			for(auto &t : texts)
			{
				if(t.find("imports/lang") != t.npos || t.find("imports\\lang") != t.npos ||
				   t.find("imports\\\\lang") != t.npos)
					return true;
			}
			return false;
		}

		auto name = key.substr(key.find_last_of('/') + 1);
		for(auto &t : texts)
		{
			for(size_t pos = t.find(name); pos != t.npos; pos = t.find(name, pos + 1))
			{
				char before = pos ? t[pos - 1] : ' ';
				if(before == '/' || before == '\\' || before == '\'' || before == '"' || before == ' ' || before == '\t')
					return true;
			}
		}
		return false;
	}

	// --- the decision -------------------------------------------------------------------

	enum class Action
	{
		unchanged,      // the user's copy is the shipped file
		update,         // replace the user's copy with the shipped file
		add,            // copy the shipped file in (missing, and referred to)
		keep_modified,  // the user edited it: left alone
		skip,           // missing and not referred to: left out
	};

	struct FileState
	{
		bool user_exists = false;
		uint64_t user_hash = 0;     // fingerprint() of the user's copy
		uint64_t shipped_hash = 0;  // fingerprint() of the shipped file
		bool known = false;         // is_known(path, user_hash)
		bool referenced = false;    // is_referenced(path, ...)
		bool take = false;          // replace even if edited (Options::take)
	};

	inline Action decide(const FileState &f)
	{
		if(!f.user_exists)
			return f.referenced || f.take ? Action::add : Action::skip;
		if(f.user_hash == f.shipped_hash)
			return Action::unchanged;
		if(f.known || f.take)
			return Action::update;
		return Action::keep_modified;
	}

	// The is_app_import_line() lines of `user` (without their line breaks), in order.
	inline std::vector<std::string> app_import_lines(std::string_view user)
	{
		std::vector<std::string> out;
		size_t pos = 0;
		while(pos < user.size())
		{
			auto end = user.find('\n', pos);
			auto line = user.substr(pos, end == user.npos ? user.npos : end - pos);
			pos = end == user.npos ? user.size() : end + 1;
			while(!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
				line.remove_suffix(1);
			if(is_app_import_line(line))
				out.emplace_back(line);
		}
		return out;
	}

	// `shipped` with `imports` appended, one per line, in its own line-break style.
	inline std::string with_app_imports(std::string_view shipped, const std::vector<std::string> &imports)
	{
		std::string out(shipped);
		if(imports.empty())
			return out;
		const char *eol = out.find("\r\n") != out.npos ? "\r\n" : "\n";
		if(!out.empty() && out.back() != '\n')
			out += eol;
		for(auto &line : imports)
		{
			out += line;
			out += eol;
		}
		return out;
	}

	// --- files --------------------------------------------------------------------------

	inline std::string narrow(std::wstring_view w)
	{
		if(w.empty())
			return {};
		int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
		std::string out(static_cast<size_t>(n > 0 ? n : 0), '\0');
		if(n > 0)
			::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
		return out;
	}

	inline std::wstring widen(std::string_view s)
	{
		if(s.empty())
			return {};
		int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
		std::wstring out(static_cast<size_t>(n > 0 ? n : 0), L'\0');
		if(n > 0)
			::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
		return out;
	}

	inline std::wstring join(std::wstring_view dir, std::wstring_view name)
	{
		std::wstring out(dir);
		if(!out.empty() && out.back() != L'\\' && out.back() != L'/')
			out += L'\\';
		out += name;
		return out;
	}

	// 'imports/lang/en.nss' -> L"imports\\lang\\en.nss"
	inline std::wstring native(std::string_view rel)
	{
		auto w = widen(rel);
		for(auto &c : w)
			if(c == L'/') c = L'\\';
		return w;
	}

	inline std::wstring full_path(const std::wstring &path)
	{
		wchar_t buffer[MAX_PATH * 4]{};
		auto n = ::GetFullPathNameW(path.c_str(), static_cast<DWORD>(std::size(buffer)), buffer, nullptr);
		std::wstring out = n && n < std::size(buffer) ? std::wstring(buffer, n) : path;
		while(out.size() > 3 && (out.back() == L'\\' || out.back() == L'/'))
			out.pop_back();
		return out;
	}

	inline bool same_path(const std::wstring &a, const std::wstring &b)
	{
		return ::CompareStringOrdinal(full_path(a).c_str(), -1, full_path(b).c_str(), -1, TRUE) == CSTR_EQUAL;
	}

	inline bool file_exists(const std::wstring &path)
	{
		auto attr = ::GetFileAttributesW(path.c_str());
		return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
	}

	inline bool read_file(const std::wstring &path, std::string &bytes)
	{
		bytes.clear();
		auto h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
		if(h == INVALID_HANDLE_VALUE)
			return false;
		bool ok = false;
		LARGE_INTEGER size{};
		if(::GetFileSizeEx(h, &size) && size.QuadPart >= 0 && size.QuadPart < 64 * 1024 * 1024)
		{
			bytes.resize(static_cast<size_t>(size.QuadPart));
			DWORD read = 0;
			ok = bytes.empty() || (::ReadFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) && read == bytes.size());
		}
		::CloseHandle(h);
		return ok;
	}

	inline bool make_dirs(const std::wstring &dir)
	{
		if(dir.empty())
			return false;
		auto attr = ::GetFileAttributesW(dir.c_str());
		if(attr != INVALID_FILE_ATTRIBUTES)
			return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
		auto slash = dir.find_last_of(L"\\/");
		if(slash != dir.npos && slash > 0 && dir[slash - 1] != L':')
			make_dirs(dir.substr(0, slash));
		return ::CreateDirectoryW(dir.c_str(), nullptr) || ::GetLastError() == ERROR_ALREADY_EXISTS;
	}

	inline std::wstring parent(const std::wstring &path)
	{
		auto slash = path.find_last_of(L"\\/");
		return slash == path.npos ? std::wstring() : path.substr(0, slash);
	}

	// Written next to the target, then moved over it: never a half-written file.
	inline bool write_file(const std::wstring &path, std::string_view bytes)
	{
		if(!make_dirs(parent(path)))
			return false;
		auto temp = path + L"." + std::to_wstring(::GetCurrentProcessId()) + L".tmp";
		auto h = ::CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if(h == INVALID_HANDLE_VALUE)
			return false;
		DWORD written = 0;
		bool ok = (bytes.empty() || ::WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr)) &&
			written == bytes.size() && ::FlushFileBuffers(h);
		::CloseHandle(h);
		if(ok)
		{
			auto attr = ::GetFileAttributesW(path.c_str());
			if(attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_READONLY))
				::SetFileAttributesW(path.c_str(), attr & ~FILE_ATTRIBUTE_READONLY);
			ok = ::MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
		}
		if(!ok)
			::DeleteFileW(temp.c_str());
		return ok;
	}

	// Every *.nss under `dir` as '/' separated paths relative to it. Reparse points are
	// not followed; folders whose lower-case name is in `skip` (top level only) are left out.
	inline void list_nss(const std::wstring &dir, const std::string &prefix, std::vector<std::string> &out,
						 const std::vector<std::wstring> &skip = {}, bool skip_backup_folders = false)
	{
		WIN32_FIND_DATAW fd{};
		auto find = ::FindFirstFileExW(join(dir, L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
		if(find == INVALID_HANDLE_VALUE)
			return;
		do
		{
			std::wstring name = fd.cFileName;
			if(name == L"." || name == L".." || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
				continue;
			if(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			{
				if(prefix.empty())
				{
					bool skipped = false;
					for(auto &s : skip)
						skipped |= ::CompareStringOrdinal(name.c_str(), -1, s.c_str(), -1, TRUE) == CSTR_EQUAL;
					if(skipped || (skip_backup_folders && ::_wcsnicmp(name.c_str(), L"backup-", 7) == 0))
						continue;
				}
				list_nss(join(dir, name), prefix + narrow(name) + "/", out, skip, skip_backup_folders);
			}
			else if(name.size() > 4 && ::_wcsicmp(name.c_str() + name.size() - 4, L".nss") == 0)
				out.push_back(prefix + narrow(name));
		} while(::FindNextFileW(find, &fd));
		::FindClose(find);
	}

	// --- snapshot (the app's Backups format) -------------------------------------------

	inline std::string json_quote(std::string_view s)
	{
		std::string out = "\"";
		for(unsigned char c : s)
		{
			switch(c)
			{
				case '"': out += "\\\""; break;
				case '\\': out += "\\\\"; break;
				case '\n': out += "\\n"; break;
				case '\r': out += "\\r"; break;
				case '\t': out += "\\t"; break;
				default:
					if(c < 0x20)
					{
						char buf[8];
						std::snprintf(buf, sizeof(buf), "\\u%04x", c);
						out += buf;
					}
					else
						out += static_cast<char>(c);
			}
		}
		out += '"';
		return out;
	}

	struct Snapshot
	{
		std::string id;    // YYYYMMDD-HHMMSS[-n]
		std::wstring dir;  // <config>\backups\<id>
		size_t files = 0;
	};

	// Copies every *.nss of `config_dir` (the backups folder excepted, as the app's
	// ConfigStore does) to <config_dir>\backups\<id>\ and writes its manifest.json.
	inline bool snapshot(const std::wstring &config_dir, const std::wstring &root_name, const std::string &reason,
						 Snapshot &out, std::string &error)
	{
		auto backups = join(config_dir, L"backups");
		if(!make_dirs(backups))
		{
			error = "cannot create " + narrow(backups);
			return false;
		}

		auto now = std::time(nullptr);
		std::tm tm{};
		::localtime_s(&tm, &now);
		char base[32], created[32];
		std::strftime(base, sizeof(base), "%Y%m%d-%H%M%S", &tm);
		std::strftime(created, sizeof(created), "%Y-%m-%dT%H:%M:%S", &tm);

		out.id = base;
		for(int n = 2; ::GetFileAttributesW(join(backups, widen(out.id)).c_str()) != INVALID_FILE_ATTRIBUTES; n++)
			out.id = std::string(base) + "-" + std::to_string(n);
		out.dir = join(backups, widen(out.id));
		if(!::CreateDirectoryW(out.dir.c_str(), nullptr))
		{
			error = "cannot create " + narrow(out.dir);
			return false;
		}

		std::vector<std::string> files;
		list_nss(config_dir, {}, files, { L"backups" });
		std::sort(files.begin(), files.end());

		std::string list;
		for(auto &rel : files)
		{
			std::string bytes;
			if(!read_file(join(config_dir, native(rel)), bytes))
				continue;
			if(!write_file(join(out.dir, native(rel)), bytes))
			{
				error = "cannot write the snapshot file " + rel;
				return false;
			}
			list += list.empty() ? "\r\n" : ",\r\n";
			list += "    { \"path\": " + json_quote(rel) + ", \"sha256\": " + json_quote(sha256_hex(bytes)) +
				", \"size\": " + std::to_string(bytes.size()) + " }";
			out.files++;
		}

		std::string manifest = "{\r\n";
		manifest += "  \"version\": 1,\r\n";
		manifest += "  \"id\": " + json_quote(out.id) + ",\r\n";
		manifest += "  \"created\": " + json_quote(created) + ",\r\n";
		manifest += "  \"reason\": " + json_quote(reason) + ",\r\n";
		manifest += "  \"root\": " + json_quote(narrow(root_name)) + ",\r\n";
		manifest += "  \"pinned\": false,\r\n";
		manifest += "  \"files\": [" + list + (list.empty() ? "]\r\n" : "\r\n  ]\r\n");
		manifest += "}\r\n";
		if(!write_file(join(out.dir, L"manifest.json"), manifest))
		{
			error = "cannot write " + narrow(join(out.dir, L"manifest.json"));
			return false;
		}
		return true;
	}

	// --- the upgrade --------------------------------------------------------------------

	// Snapshot reasons, in English: the app shows them translated (nsp::tr).
	inline constexpr const char *reason_upgrade = "Before updating the menu files";
	inline constexpr const char *reason_take = "Before using the new version of a menu file";

	struct Options
	{
		std::wstring root_name = L"shell.nss";  // the root file, for the manifest
		std::vector<std::string> take;          // replace these even if edited (relative paths)
		bool snapshot = true;
		bool dry_run = false;                   // decide only, write nothing
	};

	struct Result
	{
		bool ok = true;
		std::string error;
		std::vector<std::string> updated, added, modified_kept, unchanged, failed;
		std::string backup_id;    // empty: nothing was written, so no snapshot
		std::wstring backup_dir;
	};

	inline Result run(const std::wstring &install_dir, const std::wstring &config_dir, const Options &options = {})
	{
		Result result;
		if(same_path(install_dir, config_dir))
		{
			result.ok = false;
			result.error = "the config folder is the install folder";
			return result;
		}

		// The shipped files.
		std::vector<std::string> shipped;
		if(file_exists(join(install_dir, L"shell.nss")))
			shipped.push_back("shell.nss");
		list_nss(join(install_dir, L"imports"), "imports/", shipped);
		std::sort(shipped.begin(), shipped.end());

		auto taken = [&](const std::string &rel)
		{
			for(auto &t : options.take)
			{
				if(same_rel(rel, t))
					return true;
			}
			return false;
		};

		struct Write
		{
			std::string rel;
			std::string bytes;
			bool added = false;
		};
		std::vector<Write> writes;

		struct Missing
		{
			std::string rel;  // emptied once added
			std::string bytes;
			FileState state;
		};
		std::vector<Missing> missing;

		// The text of the config once this is done (lower case), for is_referenced():
		// the shipped text of each file that gets replaced, the user's text of the others.
		std::vector<std::string> texts;

		for(auto &rel : shipped)
		{
			if(is_app_owned(rel))
				continue;
			std::string bytes;
			if(!read_file(join(install_dir, native(rel)), bytes))
			{
				result.failed.push_back(rel);
				continue;
			}

			FileState f;
			f.shipped_hash = fingerprint(bytes);
			f.take = taken(rel);
			auto user_path = join(config_dir, native(rel));
			f.user_exists = file_exists(user_path);
			if(!f.user_exists)
			{
				missing.push_back({ rel, std::move(bytes), f });
				continue;
			}

			std::string user;
			if(!read_file(user_path, user))
			{
				result.failed.push_back(rel);
				continue;
			}
			f.user_hash = fingerprint(user);
			f.known = is_known(rel, f.user_hash);

			switch(decide(f))
			{
				case Action::unchanged:
					result.unchanged.push_back(rel);
					break;
				case Action::keep_modified:
					result.modified_kept.push_back(rel);
					break;
				case Action::update:
					texts.push_back(to_lower(bytes));
					// The root keeps the app's imports of its own files.
					writes.push_back({ rel, with_app_imports(bytes, app_import_lines(user)), false });
					break;
				default:
					break;
			}
		}

		std::vector<std::string> user_files;
		list_nss(config_dir, {}, user_files, { L"backups" }, true);
		for(auto &rel : user_files)
		{
			bool replaced = false;
			for(auto &w : writes)
				replaced |= same_rel(w.rel, rel);
			std::string bytes;
			if(!replaced && read_file(join(config_dir, native(rel)), bytes))
				texts.push_back(to_lower(bytes));
		}

		// Missing files: added when referred to, also by a file added in an earlier round.
		for(bool more = true; more;)
		{
			more = false;
			for(auto &m : missing)
			{
				if(m.rel.empty())
					continue;
				m.state.referenced = is_referenced(m.rel, texts);
				if(decide(m.state) != Action::add)
					continue;
				texts.push_back(to_lower(m.bytes));
				writes.push_back({ m.rel, std::move(m.bytes), true });
				m.rel.clear();
				more = true;
			}
		}

		// A file to take that isn't shipped (or is the app's own) is reported as failed.
		for(auto &t : options.take)
		{
			bool listed = false;
			for(auto &rel : shipped)
				listed |= !is_app_owned(rel) && same_rel(rel, t);
			if(!listed)
			{
				result.failed.push_back(t);
				result.error = "not a shipped file: " + t;
			}
		}

		if(writes.empty() || options.dry_run)
		{
			for(auto &w : writes)
				(w.added ? result.added : result.updated).push_back(w.rel);
			result.ok = result.failed.empty();
			return result;
		}

		if(options.snapshot)
		{
			Snapshot snap;
			if(!snapshot(config_dir, options.root_name, options.take.empty() ? reason_upgrade : reason_take, snap, result.error))
			{
				// Nothing written without a copy of what was there.
				result.ok = false;
				result.error = "no backup, nothing changed: " + result.error;
				return result;
			}
			result.backup_id = snap.id;
			result.backup_dir = snap.dir;
		}

		// Imports first, the root last (shell.dll reloads on a change of the root).
		std::stable_partition(writes.begin(), writes.end(), [](const Write &w) { return w.rel != "shell.nss"; });
		for(auto &w : writes)
		{
			if(write_file(join(config_dir, native(w.rel)), w.bytes))
				(w.added ? result.added : result.updated).push_back(w.rel);
			else
				result.failed.push_back(w.rel);
		}
		std::sort(result.updated.begin(), result.updated.end());
		std::sort(result.added.begin(), result.added.end());
		result.ok = result.failed.empty();
		if(!result.ok && result.error.empty())
			result.error = "some files could not be read or written";
		return result;
	}

	// {"ok":..,"configDir":..,"installDir":..,"backup":..,"backupDir":..,"updated":[..],
	//  "added":[..],"modified_kept":[..],"unchanged":[..],"failed":[..],"error":..}
	// with CRLF line breaks, UTF-8. An empty config_dir is written as null (no per-user config).
	inline std::string to_json(const Result &r, const std::wstring &install_dir, const std::wstring &config_dir)
	{
		auto str_or_null = [](const std::string &s) { return s.empty() ? std::string("null") : json_quote(s); };
		auto list = [](const std::vector<std::string> &items)
		{
			std::string out = "[";
			for(size_t i = 0; i < items.size(); i++)
				out += (i ? ", " : "") + json_quote(items[i]);
			return out + "]";
		};
		std::string j = "{\r\n";
		j += std::string("  \"ok\": ") + (r.ok ? "true" : "false") + ",\r\n";
		j += "  \"configDir\": " + str_or_null(narrow(config_dir)) + ",\r\n";
		j += "  \"installDir\": " + str_or_null(narrow(install_dir)) + ",\r\n";
		j += "  \"backup\": " + str_or_null(r.backup_id) + ",\r\n";
		j += "  \"backupDir\": " + str_or_null(narrow(r.backup_dir)) + ",\r\n";
		j += "  \"updated\": " + list(r.updated) + ",\r\n";
		j += "  \"added\": " + list(r.added) + ",\r\n";
		j += "  \"modified_kept\": " + list(r.modified_kept) + ",\r\n";
		j += "  \"unchanged\": " + list(r.unchanged) + ",\r\n";
		j += "  \"failed\": " + list(r.failed) + ",\r\n";
		j += "  \"error\": " + str_or_null(r.error) + "\r\n";
		j += "}";
		return j;
	}
}
