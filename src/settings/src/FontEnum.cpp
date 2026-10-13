#include "FontEnum.h"

#include "Text.h"

#include <windows.h>

#include <algorithm>
#include <set>

namespace fonts {

namespace {

std::string lower(std::string_view s)
{
    std::string out(s);
    for (auto &c : out)
        if (c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
    return out;
}

int CALLBACK onFont(const LOGFONTW *lf, const TEXTMETRICW *, DWORD, LPARAM data)
{
    auto *names = reinterpret_cast<std::set<std::wstring> *>(data);
    if (lf->lfFaceName[0] && lf->lfFaceName[0] != L'@')
        names->insert(lf->lfFaceName);
    return 1;
}

} // namespace

std::vector<std::string> families()
{
    std::set<std::wstring> names;
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    HDC dc = GetDC(nullptr);
    EnumFontFamiliesExW(dc, &lf, &onFont, reinterpret_cast<LPARAM>(&names), 0);
    ReleaseDC(nullptr, dc);

    std::vector<std::string> out;
    out.reserve(names.size());
    for (const auto &n : names)
        out.push_back(text::narrow(n));
    std::sort(out.begin(), out.end(), [](const std::string &a, const std::string &b) { return lower(a) < lower(b); });
    out.erase(std::unique(out.begin(), out.end(), [](const std::string &a, const std::string &b) { return lower(a) == lower(b); }),
              out.end());
    return out;
}

bool installed(const std::vector<std::string> &families, std::string_view name)
{
    auto key = lower(name);
    return std::any_of(families.begin(), families.end(), [&](const std::string &f) { return lower(f) == key; });
}

std::vector<std::string> filter(const std::vector<std::string> &families, std::string_view query)
{
    auto q = lower(query);
    while (!q.empty() && q.front() == ' ')
        q.erase(q.begin());
    while (!q.empty() && q.back() == ' ')
        q.pop_back();
    if (q.empty())
        return families;
    std::vector<std::string> starts, contains;
    for (const auto &f : families) {
        auto l = lower(f);
        auto pos = l.find(q);
        if (pos == 0)
            starts.push_back(f);
        else if (pos != std::string::npos)
            contains.push_back(f);
    }
    starts.insert(starts.end(), contains.begin(), contains.end());
    return starts;
}

} // namespace fonts
