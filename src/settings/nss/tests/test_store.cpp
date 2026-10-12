#include "TestUtil.h"

#include <ctime>

using namespace nss;
namespace fs = std::filesystem;

namespace
{
	TextFormat utf16()
	{
		TextFormat f;
		f.encoding = Encoding::Utf16LE;
		f.bom = true;
		f.eol = Eol::CRLF;
		return f;
	}

	// A small tree: a UTF-16 root and a UTF-8 theme import.
	void makeTree(const fs::path &dir)
	{
		test::writeBytes(dir / "shell.nss", encode("settings { priority = 1 }\r\nimport 'imports/theme.nss'\r\n", utf16()));
		test::writeBytes(dir / "imports/theme.nss", "theme\n{\n\tdark = true\n}\n");
	}

	size_t countFiles(const fs::path &dir, std::wstring_view suffix)
	{
		size_t n = 0;
		for(auto &e : fs::recursive_directory_iterator(dir))
		{
			auto name = e.path().filename().wstring();
			if(name.size() >= suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
				n++;
		}
		return n;
	}

	std::string idAt(std::chrono::system_clock::time_point tp)
	{
		auto t = std::chrono::system_clock::to_time_t(tp);
		std::tm tm{};
		localtime_s(&tm, &t);
		char buf[32];
		std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
		return buf;
	}

	void fakeSnapshot(const fs::path &backups, const std::string &id, bool pinned)
	{
		test::writeBytes(backups / id / "manifest.json",
			"{ \"version\": 1, \"id\": \"" + id + "\", \"reason\": \"test\", \"pinned\": " + (pinned ? "true" : "false") +
			", \"files\": [] }");
	}
}

TEST_CASE("store: sha256 and atomic writes")
{
	CHECK(ConfigStore::sha256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	CHECK(ConfigStore::sha256("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

	test::TempDir dir;
	auto file = dir / "a.nss";
	std::string err;
	CHECK(ConfigStore::atomicWrite(file, "one", &err));
	CHECK(test::readBytes(file) == "one");
	::SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_HIDDEN);
	CHECK(ConfigStore::atomicWrite(file, "two", &err));
	CHECK(test::readBytes(file) == "two");
	CHECK((::GetFileAttributesW(file.c_str()) & FILE_ATTRIBUTE_HIDDEN) != 0);  // ReplaceFileW kept them
	CHECK(countFiles(dir.path, L".tmp") == 0);
	CHECK_FALSE(ConfigStore::atomicWrite(dir / "missing-dir/x.nss", "x", &err));
	CHECK_FALSE(err.empty());
}

TEST_CASE("store: save writes imports first, keeps encodings, makes .bak and a snapshot")
{
	test::TempDir dir;
	makeTree(dir.path);
	auto oldRoot = test::readBytes(dir / "shell.nss");
	auto oldTheme = test::readBytes(dir / "imports/theme.nss");

	ConfigStore store(dir / "shell.nss");
	CHECK(store.treeFiles().size() == 2);

	std::string seenRoot;
	SaveOptions opts;
	opts.reason = "appearance";
	opts.validator = [&](const fs::path &root)
	{
		seenRoot = decode(test::readBytes(root)).text;
		CHECK(root.parent_path() != dir.path);
		CHECK(fs::exists(root.parent_path() / "imports/settings-app.nss"));
		return CheckResult{ true, "{\"ok\": true}" };
	};

	auto res = store.save({
		{ "shell.nss", "settings { priority = 1 }\r\nimport 'imports/theme.nss'\r\nimport 'imports/settings-app.nss'\r\n", {} },
		{ "imports/settings-app.nss", OverridesWriter::generate({ { "theme.dark", "false" } }), {} },
		{ "imports/theme.nss", "theme\n{\n\tdark = true\n}\n", {} },  // unchanged: not rewritten
	}, opts);
	REQUIRE_MESSAGE(res.ok, res.error);
	CHECK(seenRoot.find("settings-app") != std::string::npos);
	REQUIRE(res.written.size() == 2);
	CHECK(res.written[0].filename() == "settings-app.nss");
	CHECK(res.written[1].filename() == "shell.nss");

	// The root keeps UTF-16 LE + BOM; the new file gets UTF-8 + BOM.
	auto rootBytes = test::readBytes(dir / "shell.nss");
	CHECK(rootBytes.substr(0, 2) == "\xFF\xFE");
	CHECK(decode(rootBytes).text.ends_with("import 'imports/settings-app.nss'\r\n"));
	CHECK(test::readBytes(dir / "imports/settings-app.nss").substr(0, 3) == "\xEF\xBB\xBF");
	CHECK(test::readBytes(dir / "imports/theme.nss") == oldTheme);

	// Single-level .bak of the files that changed.
	CHECK(test::readBytes(dir / "shell.nss.bak") == oldRoot);
	CHECK_FALSE(fs::exists(dir / "imports/theme.nss.bak"));

	// The snapshot holds the tree as it was before the save.
	auto snaps = store.snapshots();
	REQUIRE(snaps.size() == 1);
	CHECK(snaps[0].id == res.snapshotId);
	CHECK(snaps[0].reason == "appearance");
	CHECK(snaps[0].root == "shell.nss");
	CHECK_FALSE(snaps[0].pinned);
	REQUIRE(snaps[0].files.size() == 2);
	CHECK(snaps[0].files[0].path == "imports/theme.nss");
	CHECK(snaps[0].files[1].path == "shell.nss");
	CHECK(snaps[0].files[1].sha256 == ConfigStore::sha256(oldRoot));
	CHECK(snaps[0].files[1].size == oldRoot.size());
	CHECK(test::readBytes(snaps[0].dir / "shell.nss") == oldRoot);
	CHECK(countFiles(dir.path, L".tmp") == 0);

	// Backups are not part of the tree.
	CHECK(store.treeFiles().size() == 3);

	// Restore puts the old files back and snapshots the current state first.
	auto newRoot = test::readBytes(dir / "shell.nss");
	std::string err;
	REQUIRE_MESSAGE(store.restore(res.snapshotId, &err), err);
	CHECK(test::readBytes(dir / "shell.nss") == oldRoot);
	CHECK(fs::exists(dir / "imports/settings-app.nss"));  // extras stay by default
	auto after = store.snapshots();
	REQUIRE(after.size() == 2);
	CHECK(after[0].reason == "before restore " + res.snapshotId);
	CHECK(test::readBytes(after[0].dir / "shell.nss") == newRoot);

	REQUIRE(store.restore(res.snapshotId, &err, true));
	CHECK_FALSE(fs::exists(dir / "imports/settings-app.nss"));

	CHECK_FALSE(store.restore("nope", &err));
	CHECK_FALSE(store.restore("..\\x", &err));
}

TEST_CASE("store: a failed check writes nothing; a failed verify rolls back")
{
	test::TempDir dir;
	makeTree(dir.path);
	auto oldRoot = test::readBytes(dir / "shell.nss");
	ConfigStore store(dir / "shell.nss");

	SaveOptions bad;
	bad.validator = [](const fs::path &) { return CheckResult{ false, "line 1: Identifier undefined" }; };
	auto res = store.save({ { "shell.nss", "theme { bogus = 1 }\r\n", {} }, { "imports/new.nss", "x=1", {} } }, bad);
	CHECK_FALSE(res.ok);
	CHECK(res.error.find("Identifier undefined") != std::string::npos);
	CHECK(test::readBytes(dir / "shell.nss") == oldRoot);
	CHECK_FALSE(fs::exists(dir / "imports/new.nss"));
	CHECK(store.snapshots().empty());
	CHECK_FALSE(fs::exists(dir / "shell.nss.bak"));

	SaveOptions verify;
	verify.verify = [&](const fs::path &root)
	{
		CHECK(root == dir / "shell.nss");
		CHECK(decode(test::readBytes(root)).text == "theme { dark = false }\r\n");
		return CheckResult{ false, "load failed" };
	};
	res = store.save({ { "shell.nss", "theme { dark = false }\r\n", {} }, { "imports/new.nss", "x=1", {} } }, verify);
	CHECK_FALSE(res.ok);
	CHECK(res.rolledBack);
	CHECK(test::readBytes(dir / "shell.nss") == oldRoot);
	CHECK_FALSE(fs::exists(dir / "imports/new.nss"));
	CHECK(store.snapshots().size() == 1);

	// Nothing to do when nothing changed.
	SaveOptions none;
	none.validator = [](const fs::path &) { CHECK_MESSAGE(false, "not called"); return CheckResult{}; };
	res = store.save({ { "imports/theme.nss", "theme\n{\n\tdark = true\n}\n", {} } }, none);
	CHECK(res.ok);
	CHECK(res.written.empty());
}

TEST_CASE("store: retention keeps 50 or 60 days, never pinned ones")
{
	test::TempDir dir;
	makeTree(dir.path);
	ConfigStore store(dir / "shell.nss");
	auto now = std::chrono::system_clock::now();
	for(int k = 0; k < 55; k++)
		fakeSnapshot(store.backupsDir(), idAt(now - std::chrono::minutes(k)), false);
	auto old = idAt(now - std::chrono::hours(24 * 61));
	auto oldPinned = idAt(now - std::chrono::hours(24 * 400));
	auto recentPinned = idAt(now - std::chrono::hours(5));
	fakeSnapshot(store.backupsDir(), old, false);
	fakeSnapshot(store.backupsDir(), oldPinned, true);
	fakeSnapshot(store.backupsDir(), recentPinned, false);
	CHECK(store.pin(recentPinned, true));
	CHECK(store.findSnapshot(recentPinned)->pinned);

	CHECK(store.snapshots().size() == 58);
	RetentionPolicy policy;
	policy.now = now;
	CHECK(store.applyRetention(policy) == 6);  // 5 beyond 50, 1 too old

	auto left = store.snapshots();
	CHECK(left.size() == 52);
	CHECK(left.front().id == idAt(now));
	CHECK(store.findSnapshot(oldPinned));
	CHECK(store.findSnapshot(recentPinned));
	CHECK_FALSE(store.findSnapshot(old));
	CHECK_FALSE(store.findSnapshot(idAt(now - std::chrono::minutes(54))));
	CHECK(store.findSnapshot(idAt(now - std::chrono::minutes(49))));

	CHECK(store.pin(oldPinned, false));
	CHECK(store.applyRetention(policy) == 1);
	CHECK(store.applyRetention(policy) == 0);
}

TEST_CASE("integration: shell.exe -config:check accepts the generated config")
{
	auto shell = test::binDir() / "shell.exe";
	if(!fs::exists(shell) || !fs::exists(test::binDir() / "shell.dll"))
	{
		MESSAGE("skipped: " << shell.string() << " is not built");
		return;
	}

	test::TempDir dir;
	test::copyCorpus(dir.path);
	ConfigStore store(dir / "shell.nss");
	auto root = NssDocument::load(dir / "shell.nss");
	REQUIRE(root.has_value());
	CHECK(OverridesWriter::ensureImportedLast(*root));

	Overrides overrides{
		{ "theme.background.opacity", "80" },
		{ "theme.background.effect", "[3, #202020, 50]" },
		{ "theme.item.radius", "3" },
		{ "theme.font.size", "13" },
		{ "theme.view", "view.compact" },
		{ "settings.tip.enabled", "false" },
	};
	for(auto &[k, v] : overrides)
		CHECK_MESSAGE(ThemeSchema::validate(k, v).empty(), k);

	SaveOptions opts;
	opts.reason = "integration";
	opts.validator = makeShellCheckValidator(shell);
	auto res = store.save({
		{ OverridesWriter::relativePath, OverridesWriter::generate(overrides), {} },
		{ "shell.nss", root->text(), {} },
	}, opts);
	CAPTURE(res.check.message);
	REQUIRE_MESSAGE(res.ok, res.error);
	CHECK(res.check.ok);
	CHECK(res.check.message.find("\"ok\": true") != std::string::npos);

	// And the real tree, checked in place.
	auto direct = makeShellCheckValidator(shell)(dir / "shell.nss");
	CAPTURE(direct.message);
	CHECK(direct.ok);

	// A broken override is refused and nothing is written.
	auto before = test::readBytes(dir / OverridesWriter::relativePath);
	res = store.save({ { OverridesWriter::relativePath, OverridesWriter::generate({ { "theme.bogus", "1" } }), {} } }, opts);
	CHECK_FALSE(res.ok);
	CHECK_FALSE(res.check.ok);
	CHECK(res.check.message.find("\"ok\": false") != std::string::npos);
	CHECK(test::readBytes(dir / OverridesWriter::relativePath) == before);
}
