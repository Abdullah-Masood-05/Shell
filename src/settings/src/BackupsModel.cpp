#include "BackupsModel.h"

#include "nss/Text.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <map>
#include <set>

namespace backups {

namespace {

bool parseCreated(const std::string &s, std::tm &tm)
{
    tm = {};
    int y, mo, d, h, mi, se;
    if (std::sscanf(s.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &se) != 6)
        return false;
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = se;
    tm.tm_isdst = -1;
    return true;
}

std::string decodedText(const std::string &bytes)
{
    auto dec = nss::decode(bytes);
    return dec.ok ? dec.text : bytes;
}

} // namespace

SnapshotDiff diffSnapshot(const nss::ConfigStore &store, const nss::SnapshotInfo &snapshot)
{
    SnapshotDiff out;
    const auto rootDir = store.rootDir();

    std::map<std::string, std::string> current; // lower-case relative path -> relative path
    for (const auto &f : store.treeFiles())
        current.emplace(nss::toLower(nss::narrow(f.lexically_relative(rootDir).generic_wstring())),
                        nss::narrow(f.lexically_relative(rootDir).generic_wstring()));

    std::set<std::string> seen;
    for (const auto &sf : snapshot.files) {
        FileDiff fd;
        fd.path = sf.path;
        auto key = nss::toLower(sf.path);
        seen.insert(key);
        std::string oldBytes, newBytes;
        if (!nss::readFile(snapshot.dir / fs::path(nss::widen(sf.path)), oldBytes)) {
            out.error = "cannot read " + sf.path + " from the snapshot";
            continue;
        }
        auto it = current.find(key);
        if (it == current.end()) {
            fd.status = FileDiff::Removed;
            fd.diff = diff::unified(decodedText(oldBytes), {});
        } else {
            nss::readFile(rootDir / fs::path(nss::widen(it->second)), newBytes);
            if (oldBytes == newBytes) {
                fd.status = FileDiff::Same;
            } else {
                fd.status = FileDiff::Modified;
                fd.diff = diff::unified(decodedText(oldBytes), decodedText(newBytes));
            }
        }
        out.files.push_back(std::move(fd));
    }
    for (const auto &[key, rel] : current) {
        if (seen.count(key))
            continue;
        std::string newBytes;
        nss::readFile(rootDir / fs::path(nss::widen(rel)), newBytes);
        FileDiff fd;
        fd.path = rel;
        fd.status = FileDiff::Added;
        fd.diff = diff::unified({}, decodedText(newBytes));
        out.files.push_back(std::move(fd));
    }
    std::stable_sort(out.files.begin(), out.files.end(), [](const FileDiff &a, const FileDiff &b) {
        bool ca = a.status != FileDiff::Same, cb = b.status != FileDiff::Same;
        if (ca != cb)
            return ca;
        return nss::toLower(a.path) < nss::toLower(b.path);
    });
    out.changed = int(std::count_if(out.files.begin(), out.files.end(),
                                    [](const FileDiff &f) { return f.status != FileDiff::Same; }));
    return out;
}

bool deleteSnapshot(const nss::ConfigStore &store, const std::string &id, std::string *error)
{
    auto s = store.findSnapshot(id);
    if (!s) {
        if (error)
            *error = "snapshot '" + id + "' not found";
        return false;
    }
    if (s->pinned) {
        if (error)
            *error = "pinned snapshots can't be deleted; unpin it first";
        return false;
    }
    std::error_code ec;
    fs::remove_all(s->dir, ec);
    if (ec) {
        if (error)
            *error = "cannot delete " + nss::narrow(s->dir.wstring()) + ": " + ec.message();
        return false;
    }
    return true;
}

bool matchesTree(const nss::ConfigStore &store, const nss::SnapshotInfo &snapshot)
{
    for (const auto &sf : snapshot.files) {
        std::string bytes;
        if (!nss::readFile(store.rootDir() / fs::path(nss::widen(sf.path)), bytes))
            return false;
        if (nss::ConfigStore::sha256(bytes) != sf.sha256)
            return false;
    }
    return true;
}

std::string displayTime(const nss::SnapshotInfo &snapshot)
{
    std::tm tm;
    if (!parseCreated(snapshot.created, tm))
        return snapshot.id;
    static const char *months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    char buf[64];
    std::snprintf(buf, sizeof buf, "%s %d, %d, %02d:%02d:%02d", months[std::clamp(tm.tm_mon, 0, 11)], tm.tm_mday,
                  tm.tm_year + 1900, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

std::string relativeTime(const nss::SnapshotInfo &snapshot)
{
    std::tm tm;
    if (!parseCreated(snapshot.created, tm))
        return {};
    auto t = std::mktime(&tm);
    if (t == -1)
        return {};
    auto secs = static_cast<long long>(std::difftime(std::time(nullptr), t));
    auto plural = [](long long n, const char *unit) {
        return std::to_string(n) + " " + unit + (n == 1 ? "" : "s") + " ago";
    };
    if (secs < 60)
        return "just now";
    if (secs < 3600)
        return plural(secs / 60, "minute");
    if (secs < 86400)
        return plural(secs / 3600, "hour");
    if (secs < 2 * 86400)
        return "yesterday";
    return plural(secs / 86400, "day");
}

} // namespace backups
