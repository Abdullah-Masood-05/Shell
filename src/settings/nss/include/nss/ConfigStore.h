#pragma once

// ConfigStore: the save transaction for a config tree.
//
// save():
//   1. Optional validator on a temporary copy of the tree with the new text
//      applied (the app runs `shell.exe -silent -config:check` there). On
//      failure nothing is written.
//   2. Snapshot of the current tree into <backups>/YYYYMMDD-HHMMSS/ with a
//      manifest.json (reason, file hashes), then retention.
//   3. A single-level <file>.bak of every file about to change.
//   4. Atomic writes (temp file in the same folder, FlushFileBuffers,
//      ReplaceFileW, falling back to MoveFileExW REPLACE_EXISTING |
//      WRITE_THROUGH), imports first and the root last, keeping each file's
//      encoding, BOM and line endings.
//   5. Optional verifier on the real tree; on failure (or a failed write)
//      every file is put back as it was.
//
// The tree is every *.nss file under the root's folder, excluding the
// backups folder.

#include "nss/Text.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace nss
{
	struct FileWrite
	{
		std::filesystem::path path;      // absolute, or relative to the root's folder
		std::string text;                // UTF-8
		std::optional<TextFormat> format; // default: the existing file's, else defaultFormat()
	};

	struct CheckResult
	{
		bool ok = true;
		std::string message;
	};

	// Gets the root file to check (inside a temp copy for the validator).
	using Validator = std::function<CheckResult(const std::filesystem::path &root)>;

	struct SaveOptions
	{
		std::string reason = "save";
		Validator validator;   // before writing, on a temp copy
		Validator verify;      // after writing, on the real tree; failure rolls back
		bool snapshot = true;
		bool backup = true;    // <file>.bak
	};

	struct SaveResult
	{
		bool ok = false;
		std::string error;
		std::string snapshotId;
		std::vector<std::filesystem::path> written;
		bool rolledBack = false;
		CheckResult check;     // what the validator or verifier said
	};

	struct SnapshotFile
	{
		std::string path;      // relative to the root's folder, '/' separated
		std::string sha256;
		uint64_t size = 0;
	};

	struct SnapshotInfo
	{
		std::string id;        // folder name, YYYYMMDD-HHMMSS[-n]
		std::filesystem::path dir;
		std::string reason;
		std::string created;   // ISO 8601 local time
		std::string root;      // root file name
		bool pinned = false;
		std::vector<SnapshotFile> files;
	};

	struct RetentionPolicy
	{
		size_t maxCount = 50;   // newest unpinned snapshots kept
		int maxAgeDays = 60;    // unpinned snapshots older than this go
		std::optional<std::chrono::system_clock::time_point> now;  // for tests
	};

	class ConfigStore
	{
	public:
		explicit ConfigStore(std::filesystem::path rootFile, std::filesystem::path backupsDir = {});

		const std::filesystem::path &rootFile() const { return _root; }
		std::filesystem::path rootDir() const { return _root.parent_path(); }
		const std::filesystem::path &backupsDir() const { return _backups; }

		SaveResult save(std::vector<FileWrite> writes, const SaveOptions &options = {});

		std::vector<std::filesystem::path> treeFiles() const;
		std::optional<SnapshotInfo> snapshot(const std::string &reason, std::string *error = nullptr);
		std::vector<SnapshotInfo> snapshots() const;  // newest first
		std::optional<SnapshotInfo> findSnapshot(const std::string &id) const;
		bool pin(const std::string &id, bool pinned);
		// Puts the snapshot's files back (after taking a "before restore"
		// snapshot). Files added since are left alone unless removeExtra.
		bool restore(const std::string &id, std::string *error = nullptr, bool removeExtra = false);
		// Deletes snapshots outside the policy; returns how many.
		size_t applyRetention(const RetentionPolicy &policy = {});
		void setRetention(const RetentionPolicy &policy) { _retention = policy; }

		static bool atomicWrite(const std::filesystem::path &path, std::string_view bytes, std::string *error = nullptr);
		static std::string sha256(std::string_view bytes);

	private:
		std::filesystem::path absolute(const std::filesystem::path &p) const;
		std::string relative(const std::filesystem::path &p) const;

		std::filesystem::path _root;
		std::filesystem::path _backups;
		RetentionPolicy _retention;
	};

	// A Validator that runs `shell.exe -silent -config:check <root> -out:<tmp>`
	// without a window and passes when the JSON says "ok": true.
	Validator makeShellCheckValidator(std::filesystem::path shellExe, unsigned timeoutMs = 30000);
}
