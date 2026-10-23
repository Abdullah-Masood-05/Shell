#pragma once

// In-app updates: GitHub releases, versions, the asset to install, SHA-256 checks,
// downloads (WinHTTP) and the portable apply step. No Slint here (tests/test_updater.cpp);
// UpdateController.cpp connects it to the UI and main.cpp runs the apply steps.
//
// Test hook: NSP_UPDATE_FEED=<https URL or local JSON file> replaces the releases URL.
// With a local file, assets may also be local file paths (for live tests without GitHub).

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace updater {

inline constexpr const char *kReleasesUrl = "https://api.github.com/repos/Abdullah-Masood-05/Shell/releases?per_page=20";
inline constexpr const char *kSumsName = "SHA256SUMS.txt";

// A semantic version: 2.0.1, 2.0.1-beta.1 (a leading "v" and "+build" are ignored).
struct Version
{
    int major = 0, minor = 0, patch = 0;
    std::vector<std::string> pre; // the dot-separated pre-release identifiers

    static std::optional<Version> parse(std::string_view s);
    std::string str() const;
    bool prerelease() const { return !pre.empty(); }
};

// <0, 0, >0 with semver precedence: 2.0.1-beta.2 < 2.0.1-beta.10 < 2.0.1-rc.1 < 2.0.1.
int compare(const Version &a, const Version &b);
inline bool operator<(const Version &a, const Version &b) { return compare(a, b) < 0; }
inline bool operator==(const Version &a, const Version &b) { return compare(a, b) == 0; }

struct Asset
{
    std::string name;
    std::string url; // browser_download_url
    uint64_t size = 0;
};

struct Release
{
    std::string tag;
    Version version;
    std::string name;
    std::string htmlUrl;
    std::string publishedAt; // ISO 8601, as GitHub sends it
    bool draft = false;
    bool prerelease = false; // GitHub's flag or a suffix in the tag
    std::vector<Asset> assets;

    const Asset *find(std::string_view assetName) const;
};

// The GitHub "list releases" JSON. Entries whose tag isn't a version are left out.
std::vector<Release> parseReleases(const std::string &json, std::string &error);

enum class Channel { Stable, Beta };

// Stable, unless the running build is itself a pre-release.
Channel defaultChannel(const Version &running);

// The newest release above `current` for the channel: no drafts, pre-releases only on
// Beta, and not the version the user skipped ("" for none).
std::optional<Release> selectUpdate(const std::vector<Release> &releases, const Version &current, Channel channel,
                                    const std::string &skipped);

enum class InstallType { Msi, Portable };
enum class Arch { X64, Arm64, X86 };

// The machine's own architecture (an x64 app on ARM64 says Arm64).
Arch nativeArch();

// MSI when the app is under Program Files and the all-users registration points to the
// same folder; otherwise portable. Paths compare case-insensitively.
InstallType detectInstallType(const std::filesystem::path &exePath, const std::filesystem::path &programFiles,
                              const std::string &machineDllUtf8);

// setup-<arch>.msi, or the x64 portable zip (the versioned name first).
const Asset *chooseAsset(const Release &release, InstallType type, Arch arch);

// SHA256SUMS.txt (sha256sum format, text or binary mode): file name -> lowercase hex.
std::map<std::string, std::string> parseSha256Sums(const std::string &text);

// Lowercase hex, or "" on error.
std::string sha256File(const std::filesystem::path &file);
std::string sha256Bytes(std::string_view bytes);

// --- update.json: what a release changes ----------------------------------------------
//
// Published next to the other assets since 2.2.0 (release.yml) and listed in
// SHA256SUMS.txt:
//   { "schema": 1, "version": "2.2.0",
//     "app":    { "package": "NilesoftShellPro-2.2.0-x64-app.zip", "sha256": "<zip>",
//                 "minCore": "2.1.1", "files": { "NilesoftShellPro.exe": "<sha256>", ... } },
//     "core":   { "since": "2.1.1", "changed": false },
//     "config": { "since": "2.2.0", "changed": true } }
// core.since: the release whose shell.dll and shell.exe this release still ships (the core
// sources haven't changed after it). app.minCore: the oldest installed release whose core
// this app works with (core.since). config: the same for shell.nss and imports\.

inline constexpr const char *kManifestName = "update.json";

struct Manifest
{
    int schema = 0;
    Version version;
    bool hasApp = false;
    std::string appPackage;                    // a file name in the same release
    std::string appSha256;
    Version minCore;
    std::map<std::string, std::string> appFiles; // '/'-separated relative path -> sha256
    Version coreSince;
    bool coreChanged = true;
    Version configSince;
    bool configChanged = true;
};

// Nullopt (and `error`) for anything that isn't a schema 1 manifest. A file path with
// "..", a drive or a leading slash makes the whole manifest unusable.
std::optional<Manifest> parseManifest(const std::string &json, std::string &error);

// How an update gets installed.
//   Portable   the portable zip, copied over the portable folder (no administrator rights)
//   AppOnly    the app package, installed for this user only (no administrator rights)
//   Installer  setup-<arch>.msi for all users: needs administrator approval
enum class UpdatePath { Portable, AppOnly, Installer };

// Why an MSI install needs the installer (and so administrator approval).
enum class ElevationReason { None, CoreChanged, NoAppPackage, UnknownCore };

struct UpdatePlan
{
    UpdatePath path = UpdatePath::Installer;
    bool needsElevation = true;
    ElevationReason reason = ElevationReason::None;
    const Asset *asset = nullptr; // what to download; nullptr: nothing usable in the release
};

// The way to install `release`. `installedRelease` is the release of the installed core
// (the MSI's own NilesoftShellPro.exe version), nullopt when unknown. Only the Installer
// path ever needs elevation, and it's chosen only when the app package can't be used: the
// release has no update.json or package, or its core is newer than the installed one.
UpdatePlan choosePlan(const Release &release, InstallType type, Arch arch, const std::optional<Manifest> &manifest,
                      const std::optional<Version> &installedRelease);

struct ManifestResult
{
    bool ok = false;      // fetched and verified
    bool missing = false; // the release has no update.json (older releases): not an error
    std::optional<Manifest> manifest;
    std::string error;
};

// Downloads update.json and checks it against SHA256SUMS.txt (both small, into memory).
ManifestResult fetchManifest(const Release &release, bool allowLocal = false);

// Release downloads only: github.com/Abdullah-Masood-05/Shell/releases/download/... and
// the hosts GitHub redirects them to. https only.
bool isAllowedDownloadUrl(std::string_view url);

// --- network -------------------------------------------------------------------------

struct HttpResult
{
    bool ok = false;
    int status = 0;
    std::string body;
    std::string error;
    bool offline = false;     // no connection, DNS failure, timeout
    bool rateLimited = false; // GitHub's API limit
};

// Called with bytes so far and the total (0 if unknown); return false to cancel.
using Progress = std::function<bool(uint64_t done, uint64_t total)>;

// GET into memory. Redirects are followed only to URLs `allowRedirect` accepts.
HttpResult httpGet(const std::string &url, const std::function<bool(std::string_view)> &allowRedirect = {});

// GET into a file (written to `file`.part, then renamed). Every hop must pass isAllowedDownloadUrl.
HttpResult httpDownload(const std::string &url, const std::filesystem::path &file, const Progress &progress);

// NSP_UPDATE_FEED, or empty.
std::wstring feedOverride();

struct FeedResult
{
    bool ok = false;
    std::vector<Release> releases;
    std::string error;
    bool offline = false;
    bool rateLimited = false;
    bool local = false; // read from a local file (NSP_UPDATE_FEED)
};

FeedResult fetchReleases(const std::wstring &feed = feedOverride());

// %LOCALAPPDATA%\Nilesoft\Shell\updates
std::filesystem::path updatesRoot();

struct DownloadResult
{
    bool ok = false;
    bool cancelled = false;
    std::filesystem::path file;
    std::string error;
};

// Downloads `asset` and the release's SHA256SUMS.txt into `dir` and checks the hash; a
// mismatch deletes the file. A verified file already in `dir` is reused. `allowLocal`:
// asset URLs may be local paths (a local NSP_UPDATE_FEED).
DownloadResult downloadAndVerify(const Release &release, const Asset &asset, const std::filesystem::path &dir,
                                 const Progress &progress, bool allowLocal = false);

// --- applying ---------------------------------------------------------------------------

// Extracts a zip with Windows' own tar.exe into an empty `dest`.
bool extractZip(const std::filesystem::path &zip, const std::filesystem::path &dest, std::string &error);

struct ApplyResult
{
    bool ok = false;
    std::string error;
    std::vector<std::wstring> keptFiles; // .nss files the user edited; the new one is next to it as .new
};

// Copies the files of an extracted portable zip over `installDir`. Each file that is
// replaced is first renamed to <name>.<n>.update.old (a loaded shell.dll can be renamed,
// not overwritten); on failure everything is put back. .nss files the user changed are
// kept. Nothing outside installDir is touched.
ApplyResult applyPortable(const std::filesystem::path &staging, const std::filesystem::path &installDir);

// Deletes the *.update.old files a previous update left in installDir (a dll Explorer
// still has loaded stays until the next time).
void cleanupOldFiles(const std::filesystem::path &installDir);

// Installs an extracted app package for this user: every file must be in the manifest
// with a matching SHA-256 (and nothing else may be there), NilesoftShellPro.exe among
// them. The files go to <appRoot>\<version>\ with an install.json (AppChannel.h) naming
// `baseDir`, the install folder this copy belongs to. Written as <version>.partial first
// and renamed when complete, so a half-written copy is never used. No administrator rights.
// On success `installedDir` is the new folder.
ApplyResult installAppPackage(const std::filesystem::path &staging, const Manifest &manifest,
                              const std::filesystem::path &appRoot, const std::filesystem::path &baseDir,
                              std::filesystem::path *installedDir = nullptr);

// Removes updates\<version> folders for versions at or below `current`.
void cleanupDownloads(const Version &current);

} // namespace updater
