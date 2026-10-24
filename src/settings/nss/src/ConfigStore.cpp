#include "nss/ConfigStore.h"
#include "Json.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <ctime>
#include <fstream>

namespace fs = std::filesystem;

namespace nss
{
	namespace
	{
		std::atomic<unsigned> g_counter{ 0 };

		std::string lastErrorText(const char *what, const fs::path &path)
		{
			return std::string(what) + " '" + narrow(path.wstring()) + "' failed (error " + std::to_string(::GetLastError()) + ")";
		}

		bool isUnder(const fs::path &child, const fs::path &parent)
		{
			auto c = pathKey(child), p = pathKey(parent);
			if(!p.empty() && p.back() != '\\') p += '\\';
			return c.rfind(p, 0) == 0;
		}

		std::tm localNow(std::chrono::system_clock::time_point tp)
		{
			auto t = std::chrono::system_clock::to_time_t(tp);
			std::tm tm{};
			localtime_s(&tm, &t);
			return tm;
		}

		std::string formatTime(const std::tm &tm, const char *fmt)
		{
			char buf[64];
			std::strftime(buf, sizeof(buf), fmt, &tm);
			return buf;
		}

		// "YYYYMMDD-HHMMSS[-n]" -> time point (local time).
		std::optional<std::chrono::system_clock::time_point> parseId(const std::string &id)
		{
			if(id.size() < 15 || id[8] != '-')
				return std::nullopt;
			for(size_t k = 0; k < 15; k++)
				if(k != 8 && !std::isdigit(static_cast<unsigned char>(id[k])))
					return std::nullopt;
			std::tm tm{};
			tm.tm_year = std::stoi(id.substr(0, 4)) - 1900;
			tm.tm_mon = std::stoi(id.substr(4, 2)) - 1;
			tm.tm_mday = std::stoi(id.substr(6, 2));
			tm.tm_hour = std::stoi(id.substr(9, 2));
			tm.tm_min = std::stoi(id.substr(11, 2));
			tm.tm_sec = std::stoi(id.substr(13, 2));
			tm.tm_isdst = -1;
			auto t = std::mktime(&tm);
			if(t == -1)
				return std::nullopt;
			return std::chrono::system_clock::from_time_t(t);
		}

		bool writePlain(const fs::path &path, std::string_view bytes)
		{
			std::error_code ec;
			fs::create_directories(path.parent_path(), ec);
			std::ofstream f(path, std::ios::binary | std::ios::trunc);
			f.write(bytes.data(), std::streamsize(bytes.size()));
			return bool(f);
		}

		std::string manifestJson(const SnapshotInfo &s)
		{
			std::string j = "{\r\n";
			j += "  \"version\": 1,\r\n";
			j += "  \"id\": " + json::quote(s.id) + ",\r\n";
			j += "  \"created\": " + json::quote(s.created) + ",\r\n";
			j += "  \"reason\": " + json::quote(s.reason) + ",\r\n";
			j += "  \"root\": " + json::quote(s.root) + ",\r\n";
			j += std::string("  \"pinned\": ") + (s.pinned ? "true" : "false") + ",\r\n";
			j += "  \"files\": [";
			for(size_t k = 0; k < s.files.size(); k++)
			{
				auto &f = s.files[k];
				j += k ? ",\r\n" : "\r\n";
				j += "    { \"path\": " + json::quote(f.path) + ", \"sha256\": " + json::quote(f.sha256) +
					", \"size\": " + std::to_string(f.size) + " }";
			}
			j += s.files.empty() ? "]\r\n" : "\r\n  ]\r\n";
			j += "}\r\n";
			return j;
		}

		std::optional<SnapshotInfo> readManifest(const fs::path &dir)
		{
			std::string bytes;
			if(!readFile(dir / L"manifest.json", bytes))
				return std::nullopt;
			auto v = json::parse(bytes);
			if(!v || v->type != json::Value::Type::Object)
				return std::nullopt;
			SnapshotInfo s;
			s.dir = dir;
			s.id = narrow(dir.filename().wstring());
			s.created = v->str("created");
			s.reason = v->str("reason");
			s.root = v->str("root");
			s.pinned = v->flag("pinned");
			if(auto files = v->get("files"); files && files->type == json::Value::Type::Array)
			{
				for(auto &f : files->array)
				{
					SnapshotFile sf;
					sf.path = f.str("path");
					sf.sha256 = f.str("sha256");
					if(auto size = f.get("size")) sf.size = uint64_t(size->number);
					s.files.push_back(std::move(sf));
				}
			}
			return s;
		}
	}

	ConfigStore::ConfigStore(fs::path rootFile, fs::path backupsDir)
	{
		std::error_code ec;
		_root = fs::absolute(rootFile, ec).lexically_normal();
		_backups = backupsDir.empty() ? rootDir() / L"backups" : fs::absolute(backupsDir, ec).lexically_normal();
	}

	fs::path ConfigStore::absolute(const fs::path &p) const
	{
		return (p.is_absolute() ? p : rootDir() / p).lexically_normal();
	}

	std::string ConfigStore::relative(const fs::path &p) const
	{
		return narrow(p.lexically_relative(rootDir()).generic_wstring());
	}

	std::string ConfigStore::sha256(std::string_view bytes)
	{
		unsigned char hash[32]{};
		BCRYPT_ALG_HANDLE alg = nullptr;
		if(BCRYPT_SUCCESS(::BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
		{
			::BCryptHash(alg, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char *>(bytes.data())), ULONG(bytes.size()),
						 hash, sizeof(hash));
			::BCryptCloseAlgorithmProvider(alg, 0);
		}
		static const char *hex = "0123456789abcdef";
		std::string out;
		for(auto b : hash)
		{
			out += hex[b >> 4];
			out += hex[b & 0xF];
		}
		return out;
	}

	bool ConfigStore::atomicWrite(const fs::path &path, std::string_view bytes, std::string *error)
	{
		auto fail = [&](std::string message)
		{
			if(error) *error = std::move(message);
			return false;
		};

		auto dir = path.parent_path();
		auto tmp = dir / (L"." + path.filename().wstring() + L"." + std::to_wstring(::GetCurrentProcessId()) + L"." +
						  std::to_wstring(++g_counter) + L".tmp");

		HANDLE h = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		if(h == INVALID_HANDLE_VALUE)
			return fail(lastErrorText("creating", tmp));

		size_t done = 0;
		bool ok = true;
		while(done < bytes.size())
		{
			DWORD chunk = DWORD(std::min<size_t>(bytes.size() - done, 1u << 20)), written = 0;
			if(!::WriteFile(h, bytes.data() + done, chunk, &written, nullptr) || written == 0)
			{
				ok = false;
				break;
			}
			done += written;
		}
		std::string msg;
		if(!ok)
			msg = lastErrorText("writing", tmp);
		else if(!::FlushFileBuffers(h))
		{
			ok = false;
			msg = lastErrorText("flushing", tmp);
		}
		::CloseHandle(h);
		if(!ok)
		{
			::DeleteFileW(tmp.c_str());
			return fail(msg);
		}

		DWORD attrs = ::GetFileAttributesW(path.c_str());
		if(attrs != INVALID_FILE_ATTRIBUTES)
		{
			// ReplaceFileW keeps the target's attributes, ACL and creation time.
			if(::ReplaceFileW(path.c_str(), tmp.c_str(), nullptr,
							  REPLACEFILE_IGNORE_MERGE_ERRORS | REPLACEFILE_IGNORE_ACL_ERRORS, nullptr, nullptr))
				return true;
		}
		if(::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			return true;
		auto m = lastErrorText("replacing", path);
		::DeleteFileW(tmp.c_str());
		return fail(m);
	}

	std::vector<fs::path> ConfigStore::treeFiles() const
	{
		std::vector<fs::path> out;
		std::error_code ec;
		auto opts = fs::directory_options::skip_permission_denied;
		for(fs::recursive_directory_iterator it(rootDir(), opts, ec), end; !ec && it != end; it.increment(ec))
		{
			auto &p = it->path();
			if(it->is_directory(ec))
			{
				// Also the backup-<time> folders `shell.exe -config:reset` leaves next to
				// shell.nss: copies, not part of the config.
				if(isUnder(p, _backups) || pathKey(p) == pathKey(_backups) ||
				   (it.depth() == 0 && narrow(p.filename().wstring()).starts_with("backup-")))
					it.disable_recursion_pending();
				continue;
			}
			if(it->is_regular_file(ec) && iequals(narrow(p.extension().wstring()), ".nss") && !isUnder(p, _backups))
				out.push_back(p.lexically_normal());
		}
		std::sort(out.begin(), out.end());
		return out;
	}

	std::optional<SnapshotInfo> ConfigStore::snapshot(const std::string &reason, std::string *error)
	{
		auto now = std::chrono::system_clock::now();
		auto tm = localNow(now);
		std::string base = formatTime(tm, "%Y%m%d-%H%M%S");
		std::error_code ec;
		fs::create_directories(_backups, ec);

		SnapshotInfo s;
		s.id = base;
		for(int n = 2; fs::exists(_backups / widen(s.id), ec); n++)
			s.id = base + "-" + std::to_string(n);
		s.dir = _backups / widen(s.id);
		s.created = formatTime(tm, "%Y-%m-%dT%H:%M:%S");
		s.reason = reason;
		s.root = narrow(_root.filename().wstring());
		if(!fs::create_directories(s.dir, ec))
		{
			if(error) *error = "cannot create " + narrow(s.dir.wstring());
			return std::nullopt;
		}

		for(auto &file : treeFiles())
		{
			std::string bytes;
			if(!readFile(file, bytes, error))
				continue;
			SnapshotFile sf;
			sf.path = relative(file);
			sf.sha256 = sha256(bytes);
			sf.size = bytes.size();
			if(!writePlain(s.dir / fs::path(widen(sf.path)), bytes))
			{
				if(error) *error = "cannot write snapshot file " + sf.path;
				fs::remove_all(s.dir, ec);
				return std::nullopt;
			}
			s.files.push_back(std::move(sf));
		}
		if(!atomicWrite(s.dir / L"manifest.json", manifestJson(s), error))
		{
			fs::remove_all(s.dir, ec);
			return std::nullopt;
		}
		return s;
	}

	std::vector<SnapshotInfo> ConfigStore::snapshots() const
	{
		std::vector<SnapshotInfo> out;
		std::error_code ec;
		for(fs::directory_iterator it(_backups, ec), end; !ec && it != end; it.increment(ec))
		{
			if(!it->is_directory(ec))
				continue;
			if(auto s = readManifest(it->path()))
				out.push_back(std::move(*s));
		}
		std::sort(out.begin(), out.end(), [](auto &a, auto &b)
		{
			auto ta = parseId(a.id), tb = parseId(b.id);
			if(ta && tb && *ta != *tb) return *ta > *tb;
			if(a.id.size() != b.id.size()) return a.id.size() > b.id.size();
			return a.id > b.id;
		});
		return out;
	}

	std::optional<SnapshotInfo> ConfigStore::findSnapshot(const std::string &id) const
	{
		std::error_code ec;
		auto dir = _backups / widen(id);
		if(id.empty() || id.find_first_of("\\/:") != std::string::npos || !fs::is_directory(dir, ec))
			return std::nullopt;
		return readManifest(dir);
	}

	bool ConfigStore::pin(const std::string &id, bool pinned)
	{
		auto s = findSnapshot(id);
		if(!s)
			return false;
		s->pinned = pinned;
		return atomicWrite(s->dir / L"manifest.json", manifestJson(*s));
	}

	bool ConfigStore::restore(const std::string &id, std::string *error, bool removeExtra)
	{
		auto s = findSnapshot(id);
		if(!s)
		{
			if(error) *error = "snapshot '" + id + "' not found";
			return false;
		}

		// Read and check everything before touching the tree.
		std::vector<std::pair<fs::path, std::string>> files;
		for(auto &f : s->files)
		{
			std::string bytes;
			if(!readFile(s->dir / fs::path(widen(f.path)), bytes, error))
				return false;
			if(!f.sha256.empty() && sha256(bytes) != f.sha256)
			{
				if(error) *error = "snapshot file '" + f.path + "' is corrupted";
				return false;
			}
			files.emplace_back(absolute(fs::path(widen(f.path))), std::move(bytes));
		}

		if(!snapshot("before restore " + id, error))
			return false;

		// Imports first, the root last.
		std::stable_partition(files.begin(), files.end(), [&](auto &f) { return pathKey(f.first) != pathKey(_root); });
		std::error_code ec;
		for(auto &[path, bytes] : files)
		{
			std::string current;
			if(readFile(path, current) && current == bytes)
				continue;
			fs::create_directories(path.parent_path(), ec);
			if(!atomicWrite(path, bytes, error))
				return false;
		}
		if(removeExtra)
		{
			for(auto &p : treeFiles())
			{
				bool known = std::any_of(files.begin(), files.end(), [&](auto &f) { return pathKey(f.first) == pathKey(p); });
				if(!known)
					fs::remove(p, ec);
			}
		}
		return true;
	}

	size_t ConfigStore::applyRetention(const RetentionPolicy &policy)
	{
		auto now = policy.now.value_or(std::chrono::system_clock::now());
		auto maxAge = std::chrono::hours(24) * policy.maxAgeDays;
		size_t kept = 0, removed = 0;
		std::error_code ec;
		for(auto &s : snapshots())
		{
			if(s.pinned)
				continue;
			auto t = parseId(s.id);
			bool tooOld = t && policy.maxAgeDays >= 0 && now - *t > maxAge;
			if(kept >= policy.maxCount || tooOld)
			{
				if(fs::remove_all(s.dir, ec) > 0)
					removed++;
				continue;
			}
			kept++;
		}
		return removed;
	}

	SaveResult ConfigStore::save(std::vector<FileWrite> writes, const SaveOptions &options)
	{
		SaveResult result;
		std::error_code ec;

		struct Prepared
		{
			fs::path path;
			std::string bytes;
			bool existed = false;
			std::string old;
		};
		std::vector<Prepared> prepared;
		for(auto &w : writes)
		{
			Prepared p;
			p.path = absolute(w.path);
			p.existed = readFile(p.path, p.old);
			TextFormat format = defaultFormat();
			if(w.format)
				format = *w.format;
			else if(p.existed)
			{
				auto dec = decode(p.old);
				if(dec.ok)
					format = dec.format;
			}
			p.bytes = encode(w.text, format);
			if(p.existed && p.bytes == p.old)
				continue;  // unchanged: leave the file (and its time stamp) alone
			prepared.push_back(std::move(p));
		}
		// Imports first, the root last, so the DLL never reloads a root that
		// points at an import that is not written yet.
		std::stable_partition(prepared.begin(), prepared.end(), [&](auto &p) { return pathKey(p.path) != pathKey(_root); });

		if(prepared.empty())
		{
			result.ok = true;
			return result;
		}

		if(options.validator)
		{
			auto temp = fs::temp_directory_path(ec) /
				(L"nss-check-" + std::to_wstring(::GetCurrentProcessId()) + L"-" + std::to_wstring(++g_counter));
			fs::create_directories(temp, ec);
			for(auto &file : treeFiles())
			{
				auto dest = temp / file.lexically_relative(rootDir());
				fs::create_directories(dest.parent_path(), ec);
				fs::copy_file(file, dest, fs::copy_options::overwrite_existing, ec);
			}
			for(auto &p : prepared)
			{
				if(isUnder(p.path, rootDir()))
					writePlain(temp / p.path.lexically_relative(rootDir()), p.bytes);
			}
			result.check = options.validator(temp / _root.filename());
			fs::remove_all(temp, ec);
			if(!result.check.ok)
			{
				result.error = "the configuration did not pass the check";
				if(!result.check.message.empty())
					result.error += ": " + result.check.message;
				return result;
			}
		}

		if(options.snapshot)
		{
			std::string err;
			auto snap = snapshot(options.reason, &err);
			if(!snap)
			{
				result.error = "snapshot failed: " + err;
				return result;
			}
			result.snapshotId = snap->id;
			applyRetention(_retention);
		}

		if(options.backup)
		{
			for(auto &p : prepared)
				if(p.existed)
					atomicWrite(fs::path(p.path.wstring() + L".bak"), p.old);
		}

		auto rollback = [&](size_t count)
		{
			for(size_t k = count; k-- > 0;)
			{
				auto &p = prepared[k];
				if(p.existed)
					atomicWrite(p.path, p.old);
				else
					fs::remove(p.path, ec);
			}
			result.rolledBack = true;
			result.written.clear();
		};

		for(size_t k = 0; k < prepared.size(); k++)
		{
			auto &p = prepared[k];
			fs::create_directories(p.path.parent_path(), ec);
			std::string err;
			if(!atomicWrite(p.path, p.bytes, &err))
			{
				rollback(k);
				result.error = err;
				return result;
			}
			result.written.push_back(p.path);
		}

		if(options.verify)
		{
			result.check = options.verify(_root);
			if(!result.check.ok)
			{
				rollback(prepared.size());
				result.error = "the saved configuration failed to load; the previous files were restored";
				if(!result.check.message.empty())
					result.error += ": " + result.check.message;
				return result;
			}
		}

		result.ok = true;
		return result;
	}

	Validator makeShellCheckValidator(fs::path shellExe, unsigned timeoutMs)
	{
		return [shellExe = std::move(shellExe), timeoutMs](const fs::path &root) -> CheckResult
		{
			CheckResult r;
			r.ok = false;
			std::error_code ec;
			auto out = fs::temp_directory_path(ec) /
				(L"nss-check-" + std::to_wstring(::GetCurrentProcessId()) + L"-" + std::to_wstring(++g_counter) + L".json");

			std::wstring cmd = L"\"" + shellExe.wstring() + L"\" -silent -config:check \"" + root.wstring() +
				L"\" \"-out:" + out.wstring() + L"\"";
			STARTUPINFOW si{};
			si.cb = sizeof(si);
			si.dwFlags = STARTF_USESHOWWINDOW;
			si.wShowWindow = SW_HIDE;
			PROCESS_INFORMATION pi{};
			if(!::CreateProcessW(shellExe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
								 shellExe.parent_path().c_str(), &si, &pi))
			{
				r.message = lastErrorText("starting", shellExe);
				return r;
			}
			DWORD wait = ::WaitForSingleObject(pi.hProcess, timeoutMs);
			if(wait == WAIT_TIMEOUT)
			{
				::TerminateProcess(pi.hProcess, 1);
				r.message = "shell.exe -config:check timed out";
			}
			::CloseHandle(pi.hThread);
			::CloseHandle(pi.hProcess);
			if(wait == WAIT_TIMEOUT)
				return r;

			std::string bytes;
			if(!readFile(out, bytes))
			{
				r.message = "shell.exe -config:check wrote no result";
				return r;
			}
			fs::remove(out, ec);
			auto text = decode(bytes).text;
			r.message = text;
			if(auto v = json::parse(text); v && v->type == json::Value::Type::Object)
				r.ok = v->flag("ok");
			return r;
		};
	}
}
