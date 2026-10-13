// Menu titles as shell.dll evaluates them (shell.exe -config:check -titles):
// no title may come out blank, whether the root shell.nss imports a language
// file (the current default) or not (an older one, which the installer keeps
// when the user changed it, next to newer imports whose titles are loc.<name>).

#include "TestUtil.h"
#include "../src/Json.h"

#include <optional>

using namespace nss;
namespace fs = std::filesystem;

namespace
{
	// The default shell.nss before the imports were localized (Nilesoft Shell
	// 1.9.x), as kept on an upgraded machine: no "import lang", literal titles.
	constexpr std::string_view oldRoot =
		"\xEF\xBB\xBF" "settings\r\n"
		"{\r\n"
		"\tpriority=1\r\n"
		"\texclude.where = !process.is_explorer\r\n"
		"\tshowdelay = 200\r\n"
		"\t// Options to allow modification of system items\r\n"
		"\tmodify.remove.duplicate=1\r\n"
		"\ttip.enabled=true\r\n"
		"}\r\n"
		"\r\n"
		"import 'imports/theme.nss'\r\n"
		"import 'imports/images.nss'\r\n"
		"\r\n"
		"import 'imports/modify.nss'\r\n"
		"\r\n"
		"menu(mode=\"multiple\" title=\"Pin/Unpin\" image=icon.pin)\r\n"
		"{\r\n"
		"}\r\n"
		"\r\n"
		"menu(mode=\"multiple\" title=title.more_options image=icon.more_options)\r\n"
		"{\r\n"
		"}\r\n"
		"\r\n"
		"import 'imports/terminal.nss'\r\n"
		"import 'imports/file-manage.nss'\r\n"
		"import 'imports/develop.nss'\r\n"
		"import 'imports/goto.nss'\r\n"
		"import 'imports/taskbar.nss'\r\n"
		"import 'imports/my-tools.nss'\r\n";

	bool shellBuilt()
	{
		return fs::exists(test::binDir() / "shell.exe") && fs::exists(test::binDir() / "shell.dll");
	}

	// Runs `exe` with `args` (hidden, waits up to 60 s); its exit code, or -1.
	int run(const fs::path &exe, const std::wstring &args)
	{
		std::wstring cmd = L"\"" + exe.wstring() + L"\" " + args;
		STARTUPINFOW si{};
		si.cb = sizeof(si);
		si.dwFlags = STARTF_USESHOWWINDOW;
		si.wShowWindow = SW_HIDE;
		PROCESS_INFORMATION pi{};
		if(!::CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
							 exe.parent_path().c_str(), &si, &pi))
			return -1;
		DWORD code = DWORD(-1);
		if(::WaitForSingleObject(pi.hProcess, 60000) == WAIT_OBJECT_0)
			::GetExitCodeProcess(pi.hProcess, &code);
		else
			::TerminateProcess(pi.hProcess, 1);
		::CloseHandle(pi.hThread);
		::CloseHandle(pi.hProcess);
		return int(code);
	}

	struct Titles
	{
		json::Value result;
		std::string text;  // the raw JSON, for failure messages

		const std::vector<json::Value> &list(std::string_view key) const
		{
			static const std::vector<json::Value> none;
			auto v = result.get(key);
			return v && v->type == json::Value::Type::Array ? v->array : none;
		}
		size_t count(std::string_view state) const
		{
			size_t n = 0;
			for(auto &t : list("titles"))
				n += t.str("state") == state;
			return n;
		}
		bool has(std::string_view title) const
		{
			for(auto &t : list("titles"))
				if(t.str("state") == "text" && t.str("title") == title)
					return true;
			return false;
		}
		bool warns(std::string_view fragment) const
		{
			for(auto &w : list("warnings"))
				if(w.str("message").find(fragment) != std::string::npos)
					return true;
			return false;
		}
		std::string blanks() const
		{
			std::string s;
			for(auto &t : list("titles"))
				if(t.str("state") == "empty")
				{
					auto line = t.get("line");
					s += t.str("file") + ":" + std::to_string(line ? int(line->number) : 0) + "\n";
				}
			return s;
		}
	};

	// shell.exe -silent -config:check <root> -titles[:<where>], from src\bin.
	std::optional<Titles> checkTitles(const fs::path &root, const fs::path &scratch, std::wstring_view where = {})
	{
		auto out = scratch / "titles.json";
		fs::remove(out);
		std::wstring titles = L"-titles";
		if(!where.empty())
			titles += L":" + std::wstring(where);
		run(test::binDir() / "shell.exe",
			L"-silent -config:check \"" + root.wstring() + L"\" " + titles + L" \"-out:" + out.wstring() + L"\"");
		std::string bytes;
		if(!readFile(out, bytes))
			return std::nullopt;
		Titles t;
		t.text = decode(bytes).text;
		auto v = json::parse(t.text);
		if(!v || v->type != json::Value::Type::Object)
			return std::nullopt;
		t.result = std::move(*v);
		return t;
	}

	// The shipped shell.nss's localization block: "// localization" through
	// "// or import lang ...".
	std::string shippedLocalizationBlock()
	{
		auto text = decode(test::readBytes(test::binDir() / "shell.nss")).text;
		auto begin = text.find("// localization");
		auto last = text.find("// or import lang");
		if(begin == std::string::npos || last == std::string::npos)
			return {};
		auto end = text.find('\n', last);
		return text.substr(begin, end == std::string::npos ? std::string::npos : end + 1 - begin);
	}

	// A copy of the install folder (shell.exe, shell.dll, imports\) with
	// `root` as its shell.nss, and an %APPDATA% for -config:init to fill.
	struct FakeInstall
	{
		test::TempDir dir;
		fs::path install = dir / "install";
		fs::path appdata = dir / "appdata";
		fs::path userRoot = appdata / "Nilesoft" / "Shell" / "shell.nss";

		explicit FakeInstall(std::string_view root)
		{
			fs::create_directories(install);
			fs::create_directories(appdata);
			fs::copy(test::binDir() / "shell.exe", install / "shell.exe");
			fs::copy(test::binDir() / "shell.dll", install / "shell.dll");
			fs::copy(test::binDir() / "imports", install / "imports", fs::copy_options::recursive);
			test::writeBytes(install / "shell.nss", root);
		}

		// shell.exe -silent -config:init [-force] with APPDATA pointing here.
		int init(bool force = false)
		{
			wchar_t saved[32768]{};
			auto had = ::GetEnvironmentVariableW(L"APPDATA", saved, ARRAYSIZE(saved)) > 0;
			::SetEnvironmentVariableW(L"APPDATA", appdata.c_str());
			auto code = run(install / "shell.exe",
							std::wstring(L"-silent -config:init") + (force ? L" -force" : L"") +
							L" \"-out:" + (dir / "init.txt").wstring() + L"\"");
			::SetEnvironmentVariableW(L"APPDATA", had ? saved : nullptr);
			return code;
		}
	};
}

TEST_CASE("integration: no title is blank with the shipped shell.nss")
{
	if(!shellBuilt())
	{
		MESSAGE("skipped: shell.exe is not built");
		return;
	}
	test::TempDir dir;
	auto t = checkTitles(test::binDir() / "shell.nss", dir.path);
	REQUIRE(t);
	CAPTURE(t->text);
	CHECK(t->result.flag("ok"));
	CHECK_MESSAGE(t->count("empty") == 0, t->blanks());
	CHECK(t->count("text") > 100);
	CHECK(t->list("warnings").empty());
	// (title.<name> strings come from Windows in its display language, so only
	// loc.* and literal titles are named here.)
	for(auto title : { "File manage", "&Develop", "Apps", "Windows", "Paint", "Task Manager", "Taskbar Settings",
					   "Copy", "New file", "System" })
		CHECK_MESSAGE(t->has(title), title);
}

TEST_CASE("integration: -titles:<where> lists what type= shows there")
{
	if(!shellBuilt())
	{
		MESSAGE("skipped: shell.exe is not built");
		return;
	}
	test::TempDir dir;
	test::copyCorpus(dir.path);
	test::writeBytes(dir / "shell.nss", oldRoot);

	// The taskbar menu of the bug report: every entry has a title.
	auto t = checkTitles(dir / "shell.nss", dir.path, L"taskbar");
	REQUIRE(t);
	CAPTURE(t->text);
	CHECK(t->result.str("where") == "taskbar");
	CHECK_MESSAGE(t->count("empty") == 0, t->blanks());
	for(auto title : { "Apps", "Windows", "Paint", "Calculator", "Task Manager", "Taskbar Settings" })
		CHECK_MESSAGE(t->has(title), title);
	CHECK_FALSE(t->has("File manage"));	// type='file|dir|drive|namespace|back'

	t = checkTitles(dir / "shell.nss", dir.path, L"file");
	REQUIRE(t);
	CHECK_MESSAGE(t->count("empty") == 0, t->blanks());
	CHECK(t->has("File manage"));
	CHECK_FALSE(t->has("Task Manager"));

	for(auto where : { L"folder", L"back", L"desktop" })
	{
		t = checkTitles(dir / "shell.nss", dir.path, where);
		REQUIRE(t);
		CHECK(t->result.flag("ok"));
		CHECK_MESSAGE(t->count("empty") == 0, t->blanks());
		CHECK(t->count("text") > 20);
	}

	// Anything else is a usage error, and no output file is written.
	CHECK_FALSE(checkTitles(dir / "shell.nss", dir.path, L"nowhere"));
}

TEST_CASE("integration: loc.* falls back to the install folder's en.nss")
{
	if(!shellBuilt())
	{
		MESSAGE("skipped: shell.exe is not built");
		return;
	}
	// Imports next to the root, but no imports\lang there: src\bin's is used.
	test::TempDir dir;
	test::copyCorpus(dir.path);
	fs::remove_all(dir / "imports" / "lang");
	test::writeBytes(dir / "shell.nss", oldRoot);

	auto t = checkTitles(dir / "shell.nss", dir.path);
	REQUIRE(t);
	CAPTURE(t->text);
	CHECK_MESSAGE(t->count("empty") == 0, t->blanks());
	CHECK(t->has("File manage"));
	CHECK(t->has("Taskbar Settings"));
}

TEST_CASE("integration: no title is blank with an older shell.nss that imports no language file")
{
	if(!shellBuilt())
	{
		MESSAGE("skipped: shell.exe is not built");
		return;
	}
	test::TempDir dir;
	test::copyCorpus(dir.path);
	test::writeBytes(dir / "shell.nss", oldRoot);

	auto t = checkTitles(dir / "shell.nss", dir.path);
	REQUIRE(t);
	CAPTURE(t->text);
	CHECK(t->result.flag("ok"));
	CHECK_MESSAGE(t->count("empty") == 0, t->blanks());
	// The loc.* titles fall back to imports\lang\en.nss, and the check says so.
	for(auto title : { "File manage", "&Develop", "Apps", "Windows", "Paint", "Calculator", "Task Manager",
					   "Taskbar Settings", "Folder", "Copy", "New file" })
		CHECK_MESSAGE(t->has(title), title);
	CHECK(t->warns("no imported language file defines"));
}

TEST_CASE("integration: a loc name no language file defines shows as readable text")
{
	if(!shellBuilt())
	{
		MESSAGE("skipped: shell.exe is not built");
		return;
	}
	test::TempDir dir;
	test::writeBytes(dir / "shell.nss",
					 "item(title=loc.my_custom_name cmd='notepad.exe')\r\n"
					 "item(title=loc.file_manage cmd='notepad.exe')\r\n"
					 "item(title='' cmd='notepad.exe')\r\n");

	auto t = checkTitles(dir / "shell.nss", dir.path);
	REQUIRE(t);
	CAPTURE(t->text);
	CHECK(t->result.flag("ok"));
	CHECK(t->has("My custom name"));
	CHECK(t->has("File manage"));
	CHECK(t->warns("loc.my_custom_name is not defined in any language file"));
	// A title that really is empty is still reported.
	CHECK(t->count("empty") == 1);
}

TEST_CASE("integration: -config:init gives an older shell.nss the localization block")
{
	if(!shellBuilt())
	{
		MESSAGE("skipped: shell.exe is not built");
		return;
	}
	auto block = shippedLocalizationBlock();
	REQUIRE(!block.empty());

	FakeInstall fake(oldRoot);
	REQUIRE(fake.init() == 0);
	auto seeded = decode(test::readBytes(fake.userRoot)).text;
	CAPTURE(seeded);

	// The file as copied is kept, byte for byte.
	auto old_copy = fake.userRoot;
	old_copy += ".old";
	CHECK(test::readBytes(old_copy) == std::string(oldRoot));

	// The default's block, word for word, right after settings { }, before
	// the first import...
	auto at = seeded.find(block);
	REQUIRE(at != std::string::npos);
	CHECK(at > seeded.find("tip.enabled=true"));
	CHECK(at < seeded.find("import 'imports/theme.nss'"));
	// ...and every line of the old file still there, in order.
	auto old = decode(std::string(oldRoot)).text;
	size_t from = 0;
	for(size_t pos = 0; pos < old.size();)
	{
		auto end = old.find('\n', pos);
		auto line = old.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
		pos = end == std::string::npos ? old.size() : end + 1;
		if(line.empty() || line == "\r")
			continue;
		auto found = seeded.find(line, from);
		CHECK_MESSAGE(found != std::string::npos, line);
		if(found != std::string::npos)
			from = found + line.size();
	}

	// It loads, with every title, and needs no English fallback any more.
	auto t = checkTitles(fake.userRoot, fake.dir.path);
	REQUIRE(t);
	CAPTURE(t->text);
	CHECK(t->result.flag("ok"));
	CHECK_MESSAGE(t->count("empty") == 0, t->blanks());
	CHECK_FALSE(t->warns("no imported language file defines"));

	// Running it again changes nothing.
	auto patched = test::readBytes(fake.userRoot);
	REQUIRE(fake.init() == 0);
	CHECK(test::readBytes(fake.userRoot) == patched);

	// A kept per-user shell.nss without the block (the user's own, older one)
	// gets it too; the earlier .old is not overwritten.
	test::writeBytes(fake.userRoot, oldRoot);
	REQUIRE(fake.init() == 0);
	CHECK(decode(test::readBytes(fake.userRoot)).text.find(block) != std::string::npos);
	auto old_copy2 = fake.userRoot;
	old_copy2 += ".old.2";
	CHECK(test::readBytes(old_copy2) == std::string(oldRoot));
	CHECK(test::readBytes(old_copy) == std::string(oldRoot));
}

TEST_CASE("integration: -config:init copies a current shell.nss unchanged")
{
	if(!shellBuilt())
	{
		MESSAGE("skipped: shell.exe is not built");
		return;
	}
	auto shipped = test::readBytes(test::binDir() / "shell.nss");
	FakeInstall fake(shipped);
	REQUIRE(fake.init() == 0);
	CHECK(test::readBytes(fake.userRoot) == shipped);
	auto old_copy = fake.userRoot;
	old_copy += ".old";
	CHECK_FALSE(fs::exists(old_copy));
}
