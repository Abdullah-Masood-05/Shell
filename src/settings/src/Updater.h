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

// Removes updates\<version> folders for versions at or below `current`.
void cleanupDownloads(const Version &current);

} // namespace updater
