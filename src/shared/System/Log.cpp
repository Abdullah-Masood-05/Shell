
//#include <time.h>
#include <system.h>
//#include <Globals.h>
#include <Resource.h>
//#include <mutex>

namespace Nilesoft
{
	using namespace Text;

	// A mutex to protect the display resource.
	// Initialization of the display protection mutex.
	std::mutex _mutex; // mutex for critical section

	string Logger::GetPath()
	{
		return ResolvePath(IO::Path::Module(CurrentModule)).move();
	}

	// Whether this process can append to `log`, or create it. Probes with real
	// opens instead of reading the folder's ACL, so the answer cannot disagree
	// with what open() will get. The probe file is deleted when its handle
	// closes. kernel32 only.
	static bool CanWriteLog(const string &log)
	{
		auto h = ::CreateFileW(log.c_str(), FILE_APPEND_DATA,
							   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
							   nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if(h != INVALID_HANDLE_VALUE)
		{
			::CloseHandle(h);
			return true;
		}

		auto error = ::GetLastError();
		// Open elsewhere without FILE_SHARE_WRITE: still the file to use.
		if(error == ERROR_SHARING_VIOLATION)
			return true;
		if(error != ERROR_FILE_NOT_FOUND)
			return false;

		string probe = log;
		probe.append_format(L".%lu.probe", ::GetCurrentProcessId());
		h = ::CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
						  FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_DELETE_ON_CLOSE,
						  nullptr);
		if(h == INVALID_HANDLE_VALUE)
			return false;
		::CloseHandle(h);
		return true;
	}

	string Logger::ResolvePath(const string &module_path, bool create_dir)
	{
		string path = module_path;
		auto len = path.length();
		if(len > 4)
			path = (path.substr(0, len - 3) + L"log").move();

		if(path.empty() || CanWriteLog(path))
			return path.move();

		wchar_t local[MAX_PATH]{};
		auto n = ::GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
		if(n == 0 || n >= MAX_PATH)
			return path.move();

		// %LOCALAPPDATA%\Nilesoft\Shell\<name>.log
		string dir = IO::Path::Combine(local, APP_COMPANY).move();
		if(create_dir)
			::CreateDirectoryW(dir.c_str(), nullptr);
		dir = IO::Path::Combine(dir, APP_NAME).move();
		if(create_dir)
			::CreateDirectoryW(dir.c_str(), nullptr);
		return IO::Path::Combine(dir, IO::Path::Name(path)).move();
	}

	const string &Logger::path() const
	{
		static std::mutex resolving;
		std::lock_guard<std::mutex> lock(resolving);
		if(_path.empty())
			_path = GetPath().move();
		return _path;
	}

	string Logger::Now()
	{
		string date_time(128, '\0');
		auto now = time(nullptr);
		tm ts{};
		if(0 == ::localtime_s(&ts, &now))
		{
			auto l = (size_t)::wcsftime(&date_time[0], date_time.capacity(), L"%Y-%m-%d %X", &ts);
			date_time.release(l);
		}
		return date_time.move();
	}

	void Logger::close()
	{
		if(hFile != INVALID_HANDLE_VALUE)
			::CloseHandle(hFile);
		hFile = INVALID_HANDLE_VALUE;
	}

	// Serialises open, write and close-after-write across threads. open() used
	// to run before this was taken, so two threads could both CreateFileW and
	// one handle leaked (upstream #715).
	std::mutex mutex;

	bool Logger::open(const wchar_t *path)
	{
		std::lock_guard<std::mutex> lock(mutex);
		_path = path;
		return open_locked(CREATE_NEW);
	}

	bool Logger::reset()
	{
		std::lock_guard<std::mutex> lock(mutex);
		close();
		return open_locked(CREATE_NEW);
	}

	bool Logger::open(uint32_t creation)
	{
		std::lock_guard<std::mutex> lock(mutex);
		return open_locked(creation);
	}

	bool Logger::open_locked(uint32_t creation)
	{
		if(hFile != INVALID_HANDLE_VALUE)
			return true;

		// Every process Shell is loaded into appends to this one file.
		// FILE_APPEND_DATA without FILE_WRITE_DATA makes each write go to the
		// end atomically; seek-then-write from separate handles could overwrite
		// another process's line. Shared for reading (and delete), so the log
		// can be read while a process holds it open (upstream #715).
		// https://learn.microsoft.com/en-us/windows/win32/fileio/file-access-rights-constants
		hFile = ::CreateFileW(path().c_str(), FILE_APPEND_DATA | SYNCHRONIZE,
							  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
							  nullptr, creation,
							  FILE_ATTRIBUTE_NORMAL, nullptr);

		if(hFile == INVALID_HANDLE_VALUE)
		{
			auto error = ::GetLastError();
			if(creation != OPEN_EXISTING &&
			   (error == ERROR_ALREADY_EXISTS ||
			   error == ERROR_FILE_EXISTS))
			{
				return open_locked(OPEN_EXISTING);
			}

			hFile = INVALID_HANDLE_VALUE;

			return false;
		}
		else if(creation == CREATE_NEW)
		{
			Security::Permission::SetFile(path().c_str());

			// An append handle on the new, empty file writes this at offset 0.
			DWORD written = 0;
			uint8_t smarker[2] = {0xFF, 0xFE};
			::WriteFile(hFile, smarker, 2, &written, nullptr);

			string msg;
			msg.append(APP_FULLNAME);
			msg.append_format(L"\r\nversion %d.%d.%d", VERSION_MAJOR, VERSION_MINOR, VERSION_BUILD);
			if constexpr(VERSION_REV > 0)
				msg.append_format(L".%d", VERSION_REV);
			msg.append_format(L" %s\r\n", APP_PROCESS);
			msg.append_format(L"%s\r\n", APP_EMAIL);
			msg.append_format(L"%s\r\n", APP_WEBSITELINK);
			//msg.append_format(L"Create on %s\r\n", Now().c_str());
			auto ver = &Windows::Version::Instance();
			msg.append_format(L"\r\n%s, version %s\r\n", ver->Name.c_str(), ver->BuildNumber.c_str());
			msg.append_format(L"%s\r\n", string(80, '-').c_str());
			::WriteFile(hFile, msg.c_str(), uint32_t(msg.length() * sizeof(wchar_t)), &written, nullptr);
		}
		// No seek: an append handle always writes at the end.
		_close_after_write = true;
		return true;
	}

	bool Logger::_write(LogLevel logLevel, const wchar_t *message, ...)
	{
		auto result = false;
		if(Muted || !message) return result;

		// Waiting for output access; the file is opened under the same lock.
		std::lock_guard<std::mutex> lock(mutex);
		if(!open_locked(CREATE_NEW)) return result;
		string msg;

		va_list argList;
		va_start(argList, message);
		msg.format(argList, message);
		va_end(argList);

		if(!msg.empty())
		{
			string str;
			if(logLevel == LogLevel::Write)
				str = msg.move();
			else if(logLevel == LogLevel::None)
				str.format(L"\r\n%s %s", Now().c_str(), msg.c_str());
			else if(logLevel != LogLevel::Debug)
				str.format(L"\r\n%s [%s] %s", Now().c_str(), sLogLevel[(int)logLevel], msg.c_str());
#ifdef _DEBUG
			else
				str.format(L"\r\n%s [%s] %s", Now().c_str(), sLogLevel[(int)logLevel], msg.c_str());

			if(logLevel == LogLevel::Error)// || logLevel == LogLevel::Debug)
				::MessageBeep(MB_ICONERROR);
			else if(logLevel == LogLevel::Warning)
				::MessageBeep(MB_ICONWARNING);
#endif
			DWORD written = 0;
			result =::WriteFile(hFile, str.c_str(),
							   uint32_t(str.length() * sizeof(wchar_t)), &written, nullptr);
		}

		if(_close_after_write)
			this->close();

		return result;
	}

	/*	bool Logger::_write(LogLevel logLevel, const wchar_t *msg)
		{
			// Waiting for output access.
			//std::lock_guard<std::mutex> lock(mutex);
			if(msg)
			{
				if(!open()) return false;

				std::wstring str;
				if(logLevel == LogLevel::Write)
					str = msg;
				else if(logLevel == LogLevel::None)
					string::Format(str, L"\n%s %s", Now().c_str(), msg);
				else if(logLevel != LogLevel::Debug)
					string::Format(str, L"\n%s [%s] %s", Now().c_str(), sLogLevel[(int)logLevel], msg);
	#ifdef _DEBUG
				else
					string::Format(str, L"\n%s [%s] %s", Now().c_str(), sLogLevel[(int)logLevel], msg);
	#endif

	#ifdef _DEBUG
				if(logLevel == LogLevel::Error)// || logLevel == LogLevel::Debug)
					::MessageBeep(MB_ICONERROR);
				else if(logLevel == LogLevel::Warning)
					::MessageBeep(MB_ICONWARNING);
	#endif
				return ::WriteFile(hFile, str.c_str(),
								   uint32_t(str.length() * sizeof(wchar_t)), nullptr, nullptr);
			}
			return false;
		}*/

		/*
		template<class... Types>
		struct count_args
		{
			static const auto value = (uint32_t)sizeof...(Types);
		};
				template<typename ...Args>
				bool write(LogLevel logLevel, const wchar_t *message_format, const Args& ... args)
				{
					if(message_format == nullptr) return false;

					// Waiting for output access.
					//std::lock_guard<std::mutex> lock(mutex);

					string msg;
					//constexpr auto size = Debug<sizeof(Args)...>::value;
					//const std::size_t n = sizeof...(T); //you may use `constexpr` instead of `const`
					//auto n = sizeof...(args);
					//constexpr auto n = (sizeof(Args) + ..);//c++17
					auto n = sizeof...(Args);
					if(n > 0)
					{
						const auto z = 260;
						msg.capacity(z);

						auto size = string::StringPrintF(msg.data(), z, message_format, args ...);

						if(size > z)
						{
							msg.capacity(size);
							size = string::StringPrintF(msg.data(), z + 1, message_format, args ...);
						}

						if(size == 0) return false;
						msg.release(size);
					}
					else
					{
						msg = message_format;
					}

					if(msg.empty())
					{
						if(!open()) return false;

						string str;

						if(logLevel == LogLevel::Write)
							str = msg;
						else if(logLevel == LogLevel::None)
							string::Format(str, L"\n%s %s", Now().c_str(), msg.c_str());
						else if(logLevel != LogLevel::Debug)
							string::Format(str, L"\n%s [%s] %s", Now().c_str(), sLogLevel[(int)logLevel], msg.c_str());
		#ifdef _DEBUG
						else
							string::Format(str, L"\n%s [%s] %s", Now().c_str(), sLogLevel[(int)logLevel], msg.c_str());
		#endif

		#ifdef _DEBUG
						if(logLevel == LogLevel::Error)// || logLevel == LogLevel::Debug)
							::MessageBeep(MB_ICONERROR);
						else if(logLevel == LogLevel::Warning)
							::MessageBeep(MB_ICONWARNING);
		#endif
						return ::WriteFile(hFile, str.c_str(),
										   uint32_t(str.length() * sizeof(wchar_t)), nullptr, nullptr);
						return true;
					}
					return false;
				}
		*/

		/*
		bool write(LogLevel logLevel, const wchar_t *message_format, ...)
				{
					if(message_format == nullptr) return false;

					// Waiting for output access.
					//std::lock_guard<std::mutex> lock(mutex);
					string msg;

					va_list argList;
					va_start(argList, message_format);
					msg.format(argList, message_format);
					va_end(argList);

					if(msg.empty())
					{
						if(!open()) return false;

						string str;

						if(logLevel == LogLevel::Write)
							str = msg;
						else if(logLevel == LogLevel::None)
							str.format(L"\n%s %s", Now().c_str(), msg.c_str());
						else if(logLevel != LogLevel::Debug)
							str.format(L"\n%s [%s] %s", Now().c_str(), sLogLevel[(int)logLevel], msg.c_str());
		#ifdef _DEBUG
						else
							str.format(L"\n%s [%s] %s", Now().c_str(), sLogLevel[(int)logLevel], msg.c_str());
		#endif

		#ifdef _DEBUG
						if(logLevel == LogLevel::Error)// || logLevel == LogLevel::Debug)
							::MessageBeep(MB_ICONERROR);
						else if(logLevel == LogLevel::Warning)
							::MessageBeep(MB_ICONWARNING);
		#endif
						return ::WriteFile(hFile, str.c_str(),
										   uint32_t(str.length() * sizeof(wchar_t)), nullptr, nullptr);
						return true;
					}
					return false;
				}
		*/
}