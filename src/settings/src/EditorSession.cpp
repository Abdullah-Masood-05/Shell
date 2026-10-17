#include "EditorSession.h"

#include "Tr.h"

#include "nss/ConfigGraph.h"

#include <nlohmann/json.hpp>

#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <map>

namespace editor {

namespace {

std::atomic<unsigned> g_counter{ 0 };

bool isUnder(const fs::path &child, const fs::path &parent)
{
    auto c = nss::pathKey(child), p = nss::pathKey(parent);
    if (p.empty())
        return false;
    if (p.back() != '\\')
        p += '\\';
    return c.rfind(p, 0) == 0;
}

std::string labelFor(const fs::path &rootDir, const fs::path &file)
{
    if (isUnder(file, rootDir))
        return nss::narrow(file.lexically_relative(rootDir).generic_wstring());
    return nss::narrow(file.wstring());
}

fs::path knownFolder(REFKNOWNFOLDERID id)
{
    PWSTR p = nullptr;
    fs::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &p)) && p)
        out = p;
    CoTaskMemFree(p);
    return out;
}

bool writeBytes(const fs::path &path, std::string_view bytes)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), std::streamsize(bytes.size()));
    return bool(f);
}

} // namespace

std::vector<FileEntry> loadTree(const fs::path &root, const fs::path &appDir)
{
    nss::GraphOptions options;
    options.appDir = appDir;
    auto graph = nss::ConfigGraph::load(root, options);
    const auto rootDir = root.parent_path();
    const auto &files = graph.files();

    // Children of each file (resolved and unresolved), in import-line order.
    struct Child
    {
        size_t line;
        int file;        // index into files, or -1
        int unresolved;  // index into graph.unresolved(), or -1
    };
    std::vector<std::vector<Child>> children(files.size());
    std::vector<int> roots;
    for (size_t i = 0; i < files.size(); ++i) {
        if (files[i].parent >= 0)
            children[size_t(files[i].parent)].push_back({ files[i].importLine, int(i), -1 });
        else
            roots.push_back(int(i));
    }
    const auto &unresolved = graph.unresolved();
    for (size_t u = 0; u < unresolved.size(); ++u) {
        auto key = nss::pathKey(unresolved[u].file);
        for (size_t i = 0; i < files.size(); ++i) {
            if (nss::pathKey(files[i].path) == key) {
                children[i].push_back({ unresolved[u].line, -1, int(u) });
                break;
            }
        }
    }
    for (auto &c : children)
        std::stable_sort(c.begin(), c.end(), [](const Child &a, const Child &b) { return a.line < b.line; });

    std::vector<FileEntry> out;
    auto visit = [&](auto &self, int index, int depth) -> void {
        const auto &f = files[size_t(index)];
        FileEntry e;
        e.path = f.path.lexically_normal();
        e.label = labelFor(rootDir, e.path);
        e.name = nss::narrow(e.path.filename().wstring());
        e.depth = depth;
        e.managed = isManaged(rootDir, e.path);
        e.missing = !f.doc;
        e.lang = f.context.mode == nss::ParseMode::Lang;
        e.note = f.error;
        out.push_back(std::move(e));
        for (const auto &c : children[size_t(index)]) {
            if (c.file >= 0) {
                self(self, c.file, depth + 1);
            } else {
                const auto &u = unresolved[size_t(c.unresolved)];
                FileEntry x;
                x.unresolved = true;
                // One line: whitespace runs collapsed, long expressions shortened.
                std::string flat;
                for (char ch : u.expression) {
                    bool space = ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
                    if (space && (flat.empty() || flat.back() == ' '))
                        continue;
                    flat += space ? ' ' : ch;
                }
                if (flat.size() > 60) {
                    size_t cut = 57;
                    while (cut > 0 && (static_cast<unsigned char>(flat[cut]) & 0xC0) == 0x80)
                        --cut; // not inside a UTF-8 sequence
                    flat = flat.substr(0, cut) + "...";
                }
                x.name = flat;
                x.label = u.expression;
                x.depth = depth + 1;
                x.note = u.reason;
                x.path.clear();
                out.push_back(std::move(x));
            }
        }
    };
    for (int r : roots)
        visit(visit, r, 0);
    return out;
}

bool isManaged(const fs::path &rootDir, const fs::path &file)
{
    auto key = nss::pathKey(file);
    return key == nss::pathKey(rootDir / L"imports" / L"settings-app.nss") ||
           key == nss::pathKey(rootDir / L"imports" / L"settings-app-menus.nss");
}

bool isProtectedPath(const fs::path &file)
{
    for (auto id : { FOLDERID_ProgramFiles, FOLDERID_ProgramFilesX86, FOLDERID_ProgramFilesX64 }) {
        auto dir = knownFolder(id);
        if (!dir.empty() && isUnder(file, dir))
            return true;
    }
    return false;
}

std::string toEditorText(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '\r') {
            out += '\n';
            if (i + 1 < text.size() && text[i + 1] == '\n')
                ++i;
        } else {
            out += c;
        }
    }
    return out;
}

std::string fromEditorText(std::string_view text, nss::Eol eol)
{
    auto nl = nss::eolString(eol);
    std::string out;
    out.reserve(text.size() + text.size() / 16);
    for (char c : text) {
        if (c == '\n')
            out.append(nl);
        else if (c != '\r')
            out += c;
    }
    return out;
}

std::optional<Buffer> loadBuffer(const fs::path &path, std::string *error)
{
    std::string bytes;
    if (!nss::readFile(path, bytes, error))
        return std::nullopt;
    auto dec = nss::decode(bytes);
    if (!dec.ok) {
        if (error)
            *error = dec.error;
        return std::nullopt;
    }
    Buffer b;
    b.path = path;
    b.format = dec.format;
    b.original = toEditorText(dec.text);
    b.text = b.original;
    b.diskHash = nss::ConfigStore::sha256(bytes);
    return b;
}

CheckOutcome parseCheck(std::string_view text, const fs::path &tempDir, const fs::path &realDir)
{
    CheckOutcome out;
    auto j = nlohmann::json::parse(text.begin(), text.end(), nullptr, false);
    if (!j.is_object()) {
        out.failure = std::string(text);
        if (out.failure.empty())
            out.failure = nsp::tr("shell.exe -config:check gave no result");
        return out;
    }
    out.ran = true;
    out.ok = j.value("ok", false);
    // Without an explicit temp folder, the checked root's folder is the temp copy.
    fs::path from = tempDir;
    if (from.empty() && !realDir.empty())
        from = fs::path(nss::widen(j.value("root", std::string{}))).parent_path();
    auto mapPath = [&](const std::string &file) {
        fs::path p(nss::widen(file));
        if (!from.empty() && !realDir.empty() && isUnder(p, from))
            return (realDir / p.lexically_relative(from)).lexically_normal();
        return p.lexically_normal();
    };
    for (const char *key : { "errors", "warnings" }) {
        auto it = j.find(key);
        if (it == j.end() || !it->is_array())
            continue;
        for (const auto &e : *it) {
            if (!e.is_object())
                continue;
            Issue i;
            i.error = std::string_view(key) == "errors";
            i.file = mapPath(e.value("file", std::string{}));
            i.line = e.value("line", 0);
            i.column = e.value("column", 0);
            i.message = e.value("message", std::string{});
            out.issues.push_back(std::move(i));
        }
    }
    if (!out.ok && std::none_of(out.issues.begin(), out.issues.end(), [](const Issue &i) { return i.error; }))
        out.issues.insert(out.issues.begin(), Issue{ true, {}, 0, 0, nsp::tr("The configuration does not load.") });
    return out;
}

CheckOutcome checkTree(const fs::path &shellExe, const fs::path &root, const std::vector<SaveRequest> &overlays)
{
    auto validator = nss::makeShellCheckValidator(shellExe);
    if (overlays.empty()) {
        auto r = validator(root);
        return parseCheck(r.message);
    }

    std::error_code ec;
    const auto rootDir = root.parent_path();
    auto temp = fs::temp_directory_path(ec) / (L"nss-editor-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                                              std::to_wstring(++g_counter));
    fs::create_directories(temp, ec);
    // shell.exe reports long paths; %TEMP% may be an 8.3 name (C:\Users\ABCDEF~1).
    if (wchar_t full[MAX_PATH * 4]{}; GetLongPathNameW(temp.c_str(), full, DWORD(std::size(full))))
        temp = full;
    nss::ConfigStore store(root);
    for (const auto &file : store.treeFiles()) {
        auto dest = temp / file.lexically_relative(rootDir);
        fs::create_directories(dest.parent_path(), ec);
        fs::copy_file(file, dest, fs::copy_options::overwrite_existing, ec);
    }
    for (const auto &o : overlays) {
        if (!isUnder(o.path, rootDir))
            continue; // outside the tree: checked as it is on disk
        writeBytes(temp / o.path.lexically_relative(rootDir),
                   nss::encode(fromEditorText(o.text, o.format.eol), o.format));
    }
    auto r = validator(temp / root.filename());
    auto out = parseCheck(r.message, temp, rootDir);
    fs::remove_all(temp, ec);
    return out;
}

std::string saveReason(const fs::path &root, const std::vector<SaveRequest> &files)
{
    std::string names;
    for (const auto &f : files) {
        if (!names.empty())
            names += ", ";
        names += labelFor(root.parent_path(), f.path);
    }
    return "Config Editor: " + names;
}

nss::SaveResult save(const fs::path &shellExe, const fs::path &root, const std::vector<SaveRequest> &files, bool force,
                     const nss::RetentionPolicy &retention)
{
    nss::ConfigStore store(root);
    store.setRetention(retention);
    std::vector<nss::FileWrite> writes;
    for (const auto &f : files) {
        if (isProtectedPath(f.path)) {
            nss::SaveResult r;
            r.error = nsp::trf("refusing to write under Program Files: {}", nss::narrow(f.path.wstring()));
            return r;
        }
        writes.push_back(nss::FileWrite{ f.path, fromEditorText(f.text, f.format.eol), f.format });
    }
    nss::SaveOptions options;
    options.reason = saveReason(root, files);
    if (!force)
        options.validator = nss::makeShellCheckValidator(shellExe);
    return store.save(std::move(writes), options);
}

std::pair<size_t, size_t> lineOffsets(std::string_view text, int line)
{
    size_t begin = 0;
    for (int l = 1; l < line; ++l) {
        auto nl = text.find('\n', begin);
        if (nl == std::string_view::npos)
            return { text.size(), text.size() };
        begin = nl + 1;
    }
    auto end = text.find('\n', begin);
    if (end == std::string_view::npos)
        end = text.size();
    return { begin, end };
}

} // namespace editor
