#include "Updater.h"

#include "Text.h"
#include "Tr.h"

#include <nlohmann/json.hpp>

#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <fstream>
#include <iterator>

#ifndef SETTINGS_VERSION
#define SETTINGS_VERSION "0.0.0"
#endif

namespace fs = std::filesystem;
using text::narrow;
using text::widen;

namespace updater {

namespace {

bool isDigits(std::string_view s)
{
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

std::vector<std::string_view> split(std::string_view s, char sep)
{
    std::vector<std::string_view> out;
    size_t start = 0;
    while (true) {
        size_t at = s.find(sep, start);
        out.push_back(s.substr(start, at == std::string_view::npos ? std::string_view::npos : at - start));
        if (at == std::string_view::npos)
            break;
        start = at + 1;
    }
    return out;
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

std::wstring lowerW(std::wstring s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return wchar_t(std::towlower(c)); });
    return s;
}

// A folder as a lowercase string without a trailing separator, for comparisons.
std::wstring folderKey(const fs::path &p)
{
    auto s = lowerW(p.lexically_normal().wstring());
    std::replace(s.begin(), s.end(), L'/', L'\\');
    while (s.size() > 3 && s.back() == L'\\')
        s.pop_back();
    return s;
}

bool startsWithI(std::string_view s, std::string_view prefix)
{
    return s.size() >= prefix.size() && lower(std::string(s.substr(0, prefix.size()))) == lower(std::string(prefix));
}

std::string str(const nlohmann::json &j, const char *key)
{
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

bool flag(const nlohmann::json &j, const char *key)
{
    auto it = j.find(key);
    return it != j.end() && it->is_boolean() && it->get<bool>();
}

bool readFile(const fs::path &p, std::string &out)
{
    std::ifstream in(p, std::ios::binary);
    if (!in)
        return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

// An asset name from the feed becomes a file name: nothing that could leave the folder.
bool safeFileName(const std::string &name)
{
    if (name.empty() || name == "." || name == ".." || name.find("..") != std::string::npos)
        return false;
    return name.find_first_of("/\\:*?\"<>|") == std::string::npos;
}

std::string winError(DWORD code)
{
    wchar_t *buf = nullptr;
    HMODULE winhttp = GetModuleHandleW(L"winhttp.dll");
    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_FROM_SYSTEM;
    if (code >= WINHTTP_ERROR_BASE && code <= WINHTTP_ERROR_LAST && winhttp)
        flags |= FORMAT_MESSAGE_FROM_HMODULE;
    DWORD n = FormatMessageW(flags, winhttp, code, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::wstring msg = n && buf ? std::wstring(buf, n) : widen(nsp::trf("error {}", code));
    if (buf)
        LocalFree(buf);
    while (!msg.empty() && (msg.back() == L'\n' || msg.back() == L'\r' || msg.back() == L' ' || msg.back() == L'.'))
        msg.pop_back();
    return narrow(msg);
}

bool isOfflineError(DWORD code)
{
    switch (code) {
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
    case ERROR_WINHTTP_CANNOT_CONNECT:
    case ERROR_WINHTTP_TIMEOUT:
    case ERROR_WINHTTP_CONNECTION_ERROR:
        return true;
    default:
        return false;
    }
}

struct InetHandle
{
    HINTERNET h = nullptr;
    InetHandle() = default;
    explicit InetHandle(HINTERNET handle) : h(handle) {}
    InetHandle(const InetHandle &) = delete;
    InetHandle &operator=(const InetHandle &) = delete;
    ~InetHandle()
    {
        if (h)
            WinHttpCloseHandle(h);
    }
    explicit operator bool() const { return h != nullptr; }
};

std::wstring queryHeader(HINTERNET req, DWORD info)
{
    DWORD size = 0;
    WinHttpQueryHeaders(req, info, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &size, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0)
        return {};
    std::wstring s(size / sizeof(wchar_t) + 1, L'\0');
    if (!WinHttpQueryHeaders(req, info, WINHTTP_HEADER_NAME_BY_INDEX, s.data(), &size, WINHTTP_NO_HEADER_INDEX))
        return {};
    s.resize(size / sizeof(wchar_t));
    return s;
}

std::wstring queryCustomHeader(HINTERNET req, const wchar_t *name)
{
    DWORD size = 0;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_CUSTOM, name, nullptr, &size, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0)
        return {};
    std::wstring s(size / sizeof(wchar_t) + 1, L'\0');
    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_CUSTOM, name, s.data(), &size, WINHTTP_NO_HEADER_INDEX))
        return {};
    s.resize(size / sizeof(wchar_t));
    return s;
}

using Sink = std::function<bool(const char *data, size_t size)>;

// One GET with manual redirects (each target must pass `allowRedirect`).
HttpResult perform(const std::string &startUrl, const Sink &sink, const Progress &progress,
                   const std::function<bool(std::string_view)> &allowRedirect)
{
    HttpResult res;
    auto fail = [&](DWORD code) {
        res.error = winError(code);
        res.offline = isOfflineError(code);
        return res;
    };

    static const std::wstring agent = L"NilesoftShellPro/" + widen(SETTINGS_VERSION) +
                                      L" (+https://github.com/Abdullah-Masood-05/Shell)";
    InetHandle session(WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                   WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session)
        return fail(GetLastError());
    WinHttpSetTimeouts(session.h, 15000, 15000, 30000, 30000);
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    if (!WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols)) {
        protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2; // TLS 1.3 needs Windows 11
        WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols);
    }

    std::string url = startUrl;
    for (int hop = 0; hop < 6; ++hop) {
        std::wstring wurl = widen(url);
        URL_COMPONENTS uc{ sizeof uc };
        wchar_t host[256]{}, path[4096]{};
        uc.lpszHostName = host;
        uc.dwHostNameLength = DWORD(std::size(host));
        uc.lpszUrlPath = path;
        uc.dwUrlPathLength = DWORD(std::size(path));
        uc.dwExtraInfoLength = 1; // the query string stays part of the path
        if (!WinHttpCrackUrl(wurl.c_str(), DWORD(wurl.size()), 0, &uc))
            return fail(GetLastError());
        if (uc.nScheme != INTERNET_SCHEME_HTTPS) {
            res.error = nsp::tr("Only https addresses are used for updates.");
            return res;
        }
        std::wstring object(uc.lpszUrlPath, uc.dwUrlPathLength);
        if (uc.lpszExtraInfo && uc.dwExtraInfoLength)
            object.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);

        InetHandle conn(WinHttpConnect(session.h, host, uc.nPort, 0));
        if (!conn)
            return fail(GetLastError());
        InetHandle req(WinHttpOpenRequest(conn.h, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                          WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
        if (!req)
            return fail(GetLastError());
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(req.h, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof policy);
        const bool api = lowerW(host) == L"api.github.com";
        const wchar_t *headers = api ? L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n"
                                     : L"Accept: application/octet-stream\r\n";
        if (!WinHttpSendRequest(req.h, headers, DWORD(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(req.h, nullptr))
            return fail(GetLastError());

        DWORD status = 0, size = sizeof status;
        WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &size, WINHTTP_NO_HEADER_INDEX);
        res.status = int(status);

        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            std::string location = narrow(queryHeader(req.h, WINHTTP_QUERY_LOCATION));
            if (!location.empty() && location.front() == '/')
                location = "https://" + narrow(host) + location;
            if (location.empty() || !allowRedirect || !allowRedirect(location)) {
                res.error = location.empty()
                                ? std::string(nsp::tr("The download was redirected to an unexpected address."))
                                : nsp::trf("The download was redirected to an unexpected address: {}", location.substr(0, 120));
                return res;
            }
            url = location;
            continue;
        }

        if (status != 200) {
            auto remaining = queryCustomHeader(req.h, L"X-RateLimit-Remaining");
            res.rateLimited = (status == 403 || status == 429) && (remaining == L"0" || status == 429);
            res.error = res.rateLimited
                            ? std::string(nsp::tr("GitHub's limit for update checks was reached. Try again in an hour."))
                            : nsp::trf("The server answered HTTP {}.", status);
            return res;
        }

        uint64_t total = 0;
        auto lengthText = queryHeader(req.h, WINHTTP_QUERY_CONTENT_LENGTH);
        if (!lengthText.empty())
            total = _wcstoui64(lengthText.c_str(), nullptr, 10);

        uint64_t done = 0;
        std::vector<char> buf(256 * 1024);
        while (true) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(req.h, &avail))
                return fail(GetLastError());
            if (avail == 0)
                break;
            DWORD read = 0;
            if (!WinHttpReadData(req.h, buf.data(), std::min<DWORD>(avail, DWORD(buf.size())), &read))
                return fail(GetLastError());
            if (read == 0)
                break;
            if (!sink(buf.data(), read)) {
                res.error = nsp::tr("Writing the download failed.");
                return res;
            }
            done += read;
            if (progress && !progress(done, total)) {
                res.error = "Cancelled.";
                return res;
            }
        }
        if (total && done != total) {
            res.error = nsp::tr("The download was cut off.");
            res.offline = true;
            return res;
        }
        res.ok = true;
        return res;
    }
    res.error = nsp::tr("Too many redirects.");
    return res;
}

bool isHttps(std::string_view url) { return startsWithI(url, "https://"); }

} // namespace

// --- versions ---------------------------------------------------------------------------

std::optional<Version> Version::parse(std::string_view s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.remove_suffix(1);
    if (!s.empty() && (s.front() == 'v' || s.front() == 'V'))
        s.remove_prefix(1);
    if (auto plus = s.find('+'); plus != std::string_view::npos)
        s = s.substr(0, plus);
    std::string_view core = s, pre;
    if (auto dash = s.find('-'); dash != std::string_view::npos) {
        core = s.substr(0, dash);
        pre = s.substr(dash + 1);
        if (pre.empty())
            return std::nullopt;
    }
    auto parts = split(core, '.');
    if (parts.size() != 3)
        return std::nullopt;
    Version v;
    int *fields[] = { &v.major, &v.minor, &v.patch };
    for (size_t i = 0; i < 3; ++i) {
        if (!isDigits(parts[i]) || parts[i].size() > 9)
            return std::nullopt;
        *fields[i] = std::stoi(std::string(parts[i]));
    }
    if (!pre.empty()) {
        for (auto id : split(pre, '.')) {
            if (id.empty() || !std::all_of(id.begin(), id.end(), [](char c) {
                    return std::isalnum(static_cast<unsigned char>(c)) || c == '-';
                }))
                return std::nullopt;
            v.pre.emplace_back(id);
        }
    }
    return v;
}

std::string Version::str() const
{
    std::string s = std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
    for (size_t i = 0; i < pre.size(); ++i)
        s += (i == 0 ? "-" : ".") + pre[i];
    return s;
}

int compare(const Version &a, const Version &b)
{
    if (a.major != b.major)
        return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor)
        return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch)
        return a.patch < b.patch ? -1 : 1;
    // A release ranks above its own pre-releases.
    if (a.pre.empty() != b.pre.empty())
        return a.pre.empty() ? 1 : -1;
    for (size_t i = 0; i < std::min(a.pre.size(), b.pre.size()); ++i) {
        const auto &x = a.pre[i], &y = b.pre[i];
        const bool nx = isDigits(x), ny = isDigits(y);
        if (nx && ny) {
            // Numerically, without overflow: longer (no leading zeros) is bigger.
            auto tx = x.substr(std::min(x.find_first_not_of('0'), x.size() - 1));
            auto ty = y.substr(std::min(y.find_first_not_of('0'), y.size() - 1));
            if (tx.size() != ty.size())
                return tx.size() < ty.size() ? -1 : 1;
            if (tx != ty)
                return tx < ty ? -1 : 1;
        } else if (nx != ny) {
            return nx ? -1 : 1; // numeric identifiers rank lower
        } else if (x != y) {
            return x < y ? -1 : 1;
        }
    }
    if (a.pre.size() != b.pre.size())
        return a.pre.size() < b.pre.size() ? -1 : 1;
    return 0;
}

// --- releases ---------------------------------------------------------------------------

const Asset *Release::find(std::string_view assetName) const
{
    for (const auto &a : assets)
        if (lower(a.name) == lower(std::string(assetName)))
            return &a;
    return nullptr;
}

std::vector<Release> parseReleases(const std::string &json, std::string &error)
{
    std::vector<Release> out;
    auto j = nlohmann::json::parse(json, nullptr, false);
    if (!j.is_array()) {
        error = j.is_object() && j.contains("message") ? nsp::trf("GitHub: {}", str(j, "message"))
                                                       : std::string(nsp::tr("The list of releases couldn't be read."));
        return out;
    }
    for (const auto &r : j) {
        if (!r.is_object())
            continue;
        Release rel;
        rel.tag = str(r, "tag_name");
        auto v = Version::parse(rel.tag);
        if (!v)
            continue;
        rel.version = *v;
        rel.name = str(r, "name");
        rel.htmlUrl = str(r, "html_url");
        rel.publishedAt = str(r, "published_at");
        rel.draft = flag(r, "draft");
        rel.prerelease = flag(r, "prerelease") || v->prerelease();
        if (auto it = r.find("assets"); it != r.end() && it->is_array()) {
            for (const auto &a : *it) {
                if (!a.is_object())
                    continue;
                Asset asset;
                asset.name = str(a, "name");
                asset.url = str(a, "browser_download_url");
                if (auto s = a.find("size"); s != a.end() && s->is_number_unsigned())
                    asset.size = s->get<uint64_t>();
                if (!asset.name.empty() && !asset.url.empty())
                    rel.assets.push_back(std::move(asset));
            }
        }
        out.push_back(std::move(rel));
    }
    return out;
}

Channel defaultChannel(const Version &running) { return running.prerelease() ? Channel::Beta : Channel::Stable; }

std::optional<Release> selectUpdate(const std::vector<Release> &releases, const Version &current, Channel channel,
                                    const std::string &skipped)
{
    auto skip = Version::parse(skipped);
    const Release *best = nullptr;
    for (const auto &r : releases) {
        if (r.draft)
            continue;
        if (channel == Channel::Stable && r.prerelease)
            continue;
        if (skip && r.version == *skip)
            continue;
        if (compare(r.version, current) <= 0)
            continue;
        if (!best || compare(best->version, r.version) < 0)
            best = &r;
    }
    if (!best)
        return std::nullopt;
    return *best;
}

// --- install type and assets ------------------------------------------------------------

Arch nativeArch()
{
    USHORT process = 0, native = 0;
    using Fn = BOOL(WINAPI *)(HANDLE, USHORT *, USHORT *);
    auto fn = reinterpret_cast<Fn>(
        reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2")));
    if (fn && fn(GetCurrentProcess(), &process, &native)) {
        if (native == IMAGE_FILE_MACHINE_ARM64)
            return Arch::Arm64;
        if (native == IMAGE_FILE_MACHINE_I386)
            return Arch::X86;
        return Arch::X64;
    }
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64)
        return Arch::Arm64;
    if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL)
        return Arch::X86;
    return Arch::X64;
}

InstallType detectInstallType(const fs::path &exePath, const fs::path &programFiles, const std::string &machineDllUtf8)
{
    if (programFiles.empty() || machineDllUtf8.empty())
        return InstallType::Portable;
    const auto dir = folderKey(exePath.parent_path());
    const auto pf = folderKey(programFiles);
    const bool underProgramFiles = dir.size() > pf.size() && dir.compare(0, pf.size(), pf) == 0 && dir[pf.size()] == L'\\';
    if (!underProgramFiles)
        return InstallType::Portable;
    const auto dllDir = folderKey(fs::path(widen(machineDllUtf8)).parent_path());
    return dllDir == dir ? InstallType::Msi : InstallType::Portable;
}

const Asset *chooseAsset(const Release &release, InstallType type, Arch arch)
{
    if (type == InstallType::Msi) {
        switch (arch) {
        case Arch::Arm64:
            return release.find("setup-arm64.msi");
        case Arch::X86:
            return release.find("setup-x86.msi");
        default:
            return release.find("setup-x64.msi");
        }
    }
    // Only an x64 portable build is published (it is what a portable install runs).
    if (auto *a = release.find("NilesoftShellPro-" + release.version.str() + "-x64-portable.zip"))
        return a;
    return release.find("NilesoftShellPro-x64-portable.zip");
}

// --- checksums --------------------------------------------------------------------------

std::map<std::string, std::string> parseSha256Sums(const std::string &textIn)
{
    std::map<std::string, std::string> out;
    for (auto line : split(textIn, '\n')) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.remove_suffix(1);
        if (line.size() < 66)
            continue;
        auto hash = line.substr(0, 64);
        if (!std::all_of(hash.begin(), hash.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); }))
            continue;
        if (line[64] != ' ' && line[64] != '\t')
            continue;
        auto name = line.substr(65);
        while (!name.empty() && (name.front() == ' ' || name.front() == '*' || name.front() == '\t'))
            name.remove_prefix(1);
        if (name.rfind("./", 0) == 0)
            name.remove_prefix(2);
        if (!name.empty())
            out[std::string(name)] = lower(std::string(hash));
    }
    return out;
}

std::string sha256File(const fs::path &file)
{
    HANDLE f = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return {};
    std::string result;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
        BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0))) {
        std::vector<UCHAR> buf(1 << 20);
        bool ok = true;
        while (true) {
            DWORD read = 0;
            if (!ReadFile(f, buf.data(), DWORD(buf.size()), &read, nullptr)) {
                ok = false;
                break;
            }
            if (read == 0)
                break;
            if (!BCRYPT_SUCCESS(BCryptHashData(hash, buf.data(), read, 0))) {
                ok = false;
                break;
            }
        }
        UCHAR digest[32];
        if (ok && BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof digest, 0))) {
            static const char *hex = "0123456789abcdef";
            for (UCHAR b : digest) {
                result += hex[b >> 4];
                result += hex[b & 15];
            }
        }
    }
    if (hash)
        BCryptDestroyHash(hash);
    if (alg)
        BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return result;
}

namespace {

// An https URL split into its lower-case host and its path without query or fragment.
// False for anything else, a user name, a port other than 443, or a path with "." or
// ".." segments (also percent-encoded).
bool splitHttpsUrl(std::string_view url, std::string &host, std::string_view &pathOnly)
{
    if (!startsWithI(url, "https://") || url.find('\\') != std::string_view::npos)
        return false;
    auto rest = url.substr(8);
    auto slash = rest.find('/');
    if (slash == std::string_view::npos)
        return false;
    host = lower(std::string(rest.substr(0, slash)));
    auto path = rest.substr(slash);
    if (host.find('@') != std::string::npos)
        return false;
    if (auto colon = host.find(':'); colon != std::string::npos) {
        if (host.substr(colon) != ":443")
            return false;
        host.resize(colon);
    }
    auto query = path.find_first_of("?#");
    pathOnly = path.substr(0, query);
    if (pathOnly.find("/../") != std::string_view::npos || pathOnly.find("/./") != std::string_view::npos ||
        pathOnly.find("%2e") != std::string_view::npos || pathOnly.find("%2E") != std::string_view::npos)
        return false;
    return true;
}

} // namespace

bool isAllowedSnippetUrl(std::string_view url)
{
    std::string host;
    std::string_view pathOnly;
    if (!splitHttpsUrl(url, host, pathOnly) || host != "raw.githubusercontent.com" ||
        url.find_first_of("?#") != std::string_view::npos)
        return false;
    constexpr std::string_view prefix = "/Abdullah-Masood-05/Nilesoft-Shell-Snippets/";
    if (!startsWithI(pathOnly, prefix))
        return false;
    // A full commit SHA, never a branch, then a file.
    auto rest = pathOnly.substr(prefix.size());
    if (rest.size() < 42 || rest[40] != '/')
        return false;
    for (size_t i = 0; i < 40; ++i)
        if (!std::isxdigit(static_cast<unsigned char>(rest[i])))
            return false;
    return rest.size() > 41 && rest.find("//") == std::string_view::npos && rest.back() != '/';
}

bool isAllowedDownloadUrl(std::string_view url)
{
    std::string host;
    std::string_view pathOnly;
    if (!splitHttpsUrl(url, host, pathOnly))
        return false;
    if (host == "github.com")
        return startsWithI(pathOnly, "/Abdullah-Masood-05/Shell/releases/download/") &&
               pathOnly.size() > std::string_view("/Abdullah-Masood-05/Shell/releases/download/").size();
    return host == "objects.githubusercontent.com" || host == "release-assets.githubusercontent.com";
}

// --- network ----------------------------------------------------------------------------

HttpResult httpGet(const std::string &url, const std::function<bool(std::string_view)> &allowRedirect)
{
    std::string body;
    constexpr size_t limit = 8u << 20;
    auto res = perform(
        url,
        [&](const char *data, size_t size) {
            if (body.size() + size > limit)
                return false;
            body.append(data, size);
            return true;
        },
        {}, allowRedirect);
    if (res.ok)
        res.body = std::move(body);
    return res;
}

HttpResult httpDownload(const std::string &url, const fs::path &file, const Progress &progress)
{
    HttpResult res;
    if (!isAllowedDownloadUrl(url)) {
        res.error = nsp::trf("The download address is not a release of Nilesoft Shell Pro: {}", url.substr(0, 120));
        return res;
    }
    auto part = fs::path(file.wstring() + L".part");
    {
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        if (!out) {
            res.error = nsp::trf("Couldn't write {}.", narrow(part.wstring()));
            return res;
        }
        res = perform(
            url,
            [&](const char *data, size_t size) {
                out.write(data, std::streamsize(size));
                return bool(out);
            },
            progress, [](std::string_view u) { return isAllowedDownloadUrl(u); });
        out.close();
        if (res.ok && !out) {
            res.ok = false;
            res.error = nsp::trf("Couldn't write {}.", narrow(part.wstring()));
        }
    }
    std::error_code ec;
    if (res.ok) {
        fs::rename(part, file, ec);
        if (ec) {
            res.ok = false;
            res.error = nsp::trf("Couldn't save {}.", narrow(file.wstring()));
        }
    }
    if (!res.ok)
        fs::remove(part, ec);
    return res;
}

std::wstring feedOverride()
{
    wchar_t buf[2048];
    DWORD n = GetEnvironmentVariableW(L"NSP_UPDATE_FEED", buf, DWORD(std::size(buf)));
    return n > 0 && n < std::size(buf) ? std::wstring(buf, n) : std::wstring{};
}

FeedResult fetchReleases(const std::wstring &feed)
{
    FeedResult r;
    std::string body;
    if (feed.empty() || feed.rfind(L"https://", 0) == 0 || feed.rfind(L"HTTPS://", 0) == 0) {
        auto res = httpGet(feed.empty() ? std::string(kReleasesUrl) : narrow(feed));
        if (!res.ok) {
            r.error = res.error;
            r.offline = res.offline;
            r.rateLimited = res.rateLimited;
            return r;
        }
        body = std::move(res.body);
    } else {
        r.local = true;
        if (!readFile(fs::path(feed), body)) {
            r.error = nsp::trf("NSP_UPDATE_FEED: couldn't read {}.", narrow(feed));
            return r;
        }
    }
    std::string error;
    r.releases = parseReleases(body, error);
    if (!error.empty()) {
        r.error = error;
        return r;
    }
    r.ok = true;
    return r;
}

fs::path updatesRoot()
{
    PWSTR local = nullptr;
    fs::path root;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)) && local)
        root = fs::path(local) / L"Nilesoft" / L"Shell" / L"updates";
    CoTaskMemFree(local);
    return root;
}

DownloadResult downloadAndVerify(const Release &release, const Asset &asset, const fs::path &dir,
                                 const Progress &progress, bool allowLocal)
{
    DownloadResult r;
    if (!safeFileName(asset.name)) {
        r.error = nsp::tr("The release lists an asset with an unusable name.");
        return r;
    }
    const Asset *sumsAsset = release.find(kSumsName);
    if (!sumsAsset) {
        r.error = nsp::tr("This release has no SHA256SUMS.txt, so the download can't be verified.");
        return r;
    }
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        r.error = nsp::trf("Couldn't create {}.", narrow(dir.wstring()));
        return r;
    }

    // The checksums first: without them nothing is downloaded.
    std::string sumsText;
    const bool localSums = allowLocal && !isHttps(sumsAsset->url);
    if (localSums) {
        if (!readFile(fs::path(widen(sumsAsset->url)), sumsText)) {
            r.error = nsp::trf("Couldn't read {}.", sumsAsset->url);
            return r;
        }
    } else {
        if (!isAllowedDownloadUrl(sumsAsset->url)) {
            r.error = nsp::tr("SHA256SUMS.txt is not served from this project's releases.");
            return r;
        }
        auto res = httpGet(sumsAsset->url, [](std::string_view u) { return isAllowedDownloadUrl(u); });
        if (!res.ok) {
            r.error = nsp::trf("Couldn't download SHA256SUMS.txt: {}", res.error);
            return r;
        }
        sumsText = std::move(res.body);
    }
    auto sums = parseSha256Sums(sumsText);
    auto expected = sums.find(asset.name);
    if (expected == sums.end()) {
        r.error = nsp::trf("SHA256SUMS.txt has no entry for {}.", asset.name);
        return r;
    }

    r.file = dir / widen(asset.name);
    if (fs::is_regular_file(r.file, ec) && sha256File(r.file) == expected->second) {
        r.ok = true; // downloaded and verified before
        return r;
    }
    fs::remove(r.file, ec);

    if (allowLocal && !isHttps(asset.url)) {
        if (!CopyFileW(widen(asset.url).c_str(), r.file.c_str(), FALSE)) {
            r.error = nsp::trf("Couldn't copy {}.", asset.url);
            return r;
        }
        if (progress)
            progress(1, 1);
    } else {
        auto res = httpDownload(asset.url, r.file, progress);
        if (!res.ok) {
            r.cancelled = res.error == "Cancelled."; // perform()'s marker, not shown
            r.error = r.cancelled ? res.error : nsp::trf("The download failed: {}", res.error);
            return r;
        }
    }

    auto actual = sha256File(r.file);
    if (actual != expected->second) {
        fs::remove(r.file, ec);
        const std::string got = actual.empty() ? std::string(nsp::tr("nothing")) : actual.substr(0, 16) + "…";
        r.error = nsp::trf("The download doesn't match SHA256SUMS.txt, so it was deleted. Expected {}…, got {}.",
                           expected->second.substr(0, 16), got);
        return r;
    }
    r.ok = true;
    return r;
}

// --- applying ---------------------------------------------------------------------------

bool extractZip(const fs::path &zip, const fs::path &dest, std::string &error)
{
    std::error_code ec;
    fs::create_directories(dest, ec);
    if (!fs::is_empty(dest, ec)) {
        error = nsp::tr("The staging folder is not empty.");
        return false;
    }
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    const fs::path tar = fs::path(sys) / L"tar.exe";
    if (!fs::is_regular_file(tar, ec)) {
        error = nsp::tr("tar.exe (part of Windows 10 and later) was not found.");
        return false;
    }
    std::wstring cmd = L"\"" + tar.wstring() + L"\" -xf \"" + zip.wstring() + L"\" -C \"" + dest.wstring() + L"\"";
    STARTUPINFOW si{ sizeof si };
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(tar.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        dest.c_str(), &si, &pi)) {
        error = nsp::trf("Couldn't start tar.exe: {}.", winError(GetLastError()));
        return false;
    }
    DWORD wait = WaitForSingleObject(pi.hProcess, 180000);
    DWORD code = 1;
    if (wait == WAIT_OBJECT_0)
        GetExitCodeProcess(pi.hProcess, &code);
    else
        TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (code != 0) {
        error = nsp::trf("The zip couldn't be extracted (tar.exe exit code {}).", code);
        return false;
    }
    // Everything must have landed inside dest (tar refuses ".." and absolute names anyway).
    const auto root = folderKey(fs::weakly_canonical(dest, ec));
    for (auto it = fs::recursive_directory_iterator(dest, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (it->is_symlink(ec)) {
            error = nsp::tr("The zip contains a link, which an update never does.");
            return false;
        }
        auto p = folderKey(fs::weakly_canonical(it->path(), ec));
        if (p.compare(0, root.size(), root) != 0) {
            error = nsp::tr("The zip contains a file outside its folder.");
            return false;
        }
    }
    return true;
}

namespace {

bool sameContent(const fs::path &a, const fs::path &b)
{
    std::string x, y;
    return readFile(a, x) && readFile(b, y) && x == y;
}

// Changed after it was written to disk: last write later than creation (as the MSI
// decides whether an unversioned file was modified by the user).
bool userEdited(const fs::path &p)
{
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d))
        return false;
    auto t = [](FILETIME f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    return t(d.ftLastWriteTime) > t(d.ftCreationTime) + 2ull * 10000000ull;
}

fs::path freeOldName(const fs::path &target)
{
    std::error_code ec;
    for (int n = 1;; ++n) {
        fs::path p(target.wstring() + L"." + std::to_wstring(n) + L".update.old");
        if (!fs::exists(p, ec))
            return p;
    }
}

bool iequalsExt(const fs::path &p, const wchar_t *ext) { return _wcsicmp(p.extension().c_str(), ext) == 0; }

} // namespace

ApplyResult applyPortable(const fs::path &staging, const fs::path &installDir)
{
    ApplyResult r;
    std::error_code ec;
    if (!fs::is_regular_file(staging / L"NilesoftShellPro.exe", ec) || !fs::is_regular_file(staging / L"shell.dll", ec)) {
        r.error = nsp::tr("The update doesn't contain NilesoftShellPro.exe and shell.dll.");
        return r;
    }
    if (!fs::is_directory(installDir, ec)) {
        r.error = nsp::trf("The install folder {} doesn't exist.", narrow(installDir.wstring()));
        return r;
    }

    std::vector<fs::path> files;
    for (auto it = fs::recursive_directory_iterator(staging, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec))
        if (it->is_regular_file(ec))
            files.push_back(fs::relative(it->path(), staging, ec));
    if (ec) {
        r.error = nsp::tr("Couldn't read the update's files.");
        return r;
    }

    struct Step
    {
        fs::path target;
        fs::path backup; // empty: the file is new
    };
    std::vector<Step> done;
    auto rollback = [&] {
        for (auto it = done.rbegin(); it != done.rend(); ++it) {
            DeleteFileW(it->target.c_str());
            if (!it->backup.empty())
                MoveFileExW(it->backup.c_str(), it->target.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
    };

    for (const auto &rel : files) {
        if (rel.empty() || rel.is_absolute() || rel.wstring().find(L"..") != std::wstring::npos)
            continue;
        const fs::path src = staging / rel;
        const fs::path target = installDir / rel;
        fs::create_directories(target.parent_path(), ec);
        if (fs::exists(target, ec)) {
            if (iequalsExt(rel, L".nss")) {
                if (sameContent(src, target))
                    continue;
                if (userEdited(target)) {
                    // Keep the user's file; the new default goes next to it.
                    CopyFileW(src.c_str(), (target.wstring() + L".new").c_str(), FALSE);
                    r.keptFiles.push_back(rel.wstring());
                    continue;
                }
            }
            fs::path backup = freeOldName(target);
            // The app that started the update may still be closing: retry for a few seconds.
            bool moved = false;
            for (int attempt = 0; attempt < 50 && !(moved = MoveFileExW(target.c_str(), backup.c_str(), 0)); ++attempt)
                Sleep(100);
            if (!moved) {
                const auto why = winError(GetLastError());
                r.error = nsp::trf("Couldn't replace {}: {}.", narrow(target.wstring()), why);
                rollback();
                return r;
            }
            done.push_back({ target, backup });
        } else {
            done.push_back({ target, {} });
        }
        if (!CopyFileW(src.c_str(), target.c_str(), FALSE)) {
            const auto why = winError(GetLastError());
            r.error = nsp::trf("Couldn't write {}: {}.", narrow(target.wstring()), why);
            rollback();
            return r;
        }
    }
    // Old files nothing holds open go now; a shell.dll Explorer has loaded stays until
    // cleanupOldFiles() on a later start.
    for (const auto &s : done)
        if (!s.backup.empty())
            DeleteFileW(s.backup.c_str());
    r.ok = true;
    return r;
}

void cleanupOldFiles(const fs::path &installDir)
{
    std::error_code ec;
    std::vector<fs::path> old;
    for (auto it = fs::recursive_directory_iterator(installDir, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        auto name = lowerW(it->path().filename().wstring());
        if (name.size() > 11 && name.compare(name.size() - 11, 11, L".update.old") == 0 && it->is_regular_file(ec))
            old.push_back(it->path());
    }
    for (const auto &p : old)
        DeleteFileW(p.c_str());
}

void cleanupDownloads(const Version &current)
{
    auto root = updatesRoot();
    std::error_code ec;
    if (root.empty() || !fs::is_directory(root, ec))
        return;
    std::vector<fs::path> gone;
    for (const auto &e : fs::directory_iterator(root, ec)) {
        if (!e.is_directory(ec))
            continue;
        auto v = Version::parse(narrow(e.path().filename().wstring()));
        if (v && compare(*v, current) <= 0)
            gone.push_back(e.path());
    }
    for (const auto &p : gone)
        fs::remove_all(p, ec);
}

} // namespace updater
