#include "ProgramIcon.h"

#include "nss/Text.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cwctype>

namespace {

// "C:\x\app.dll,3" -> ("C:\x\app.dll", 3)
std::pair<std::wstring, int> splitIndex(const std::wstring &path)
{
    auto comma = path.rfind(L',');
    if (comma == std::wstring::npos || comma + 1 >= path.size())
        return { path, 0 };
    auto tail = path.substr(comma + 1);
    if (!std::all_of(tail.begin(), tail.end(), [](wchar_t c) { return iswdigit(c) || c == L'-'; }))
        return { path, 0 };
    return { path.substr(0, comma), _wtoi(tail.c_str()) };
}

std::optional<IconPixels> toPixels(HICON icon)
{
    std::optional<IconPixels> out;
    ICONINFO ii{};
    if (!GetIconInfo(icon, &ii))
        return out;
    BITMAP bm{};
    if (ii.hbmColor && GetObjectW(ii.hbmColor, sizeof bm, &bm) && bm.bmWidth > 0 && bm.bmHeight > 0) {
        const int bw = bm.bmWidth, bh = bm.bmHeight;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = bw;
        bi.bmiHeader.biHeight = -bh; // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        std::vector<uint8_t> bgra(size_t(bw) * size_t(bh) * 4);
        HDC dc = GetDC(nullptr);
        int lines = GetDIBits(dc, ii.hbmColor, 0, UINT(bh), bgra.data(), &bi, DIB_RGB_COLORS);
        bool anyAlpha = false;
        for (size_t i = 3; i < bgra.size(); i += 4)
            anyAlpha |= bgra[i] != 0;
        std::vector<uint8_t> mask;
        if (lines == bh && !anyAlpha && ii.hbmMask) {
            // An icon without alpha: the AND mask says what's transparent.
            mask.resize(bgra.size());
            GetDIBits(dc, ii.hbmMask, 0, UINT(bh), mask.data(), &bi, DIB_RGB_COLORS);
        }
        ReleaseDC(nullptr, dc);
        if (lines == bh) {
            IconPixels px;
            px.width = uint32_t(bw);
            px.height = uint32_t(bh);
            px.rgba.resize(bgra.size());
            for (size_t i = 0; i < size_t(bw) * size_t(bh); ++i) {
                px.rgba[i * 4 + 0] = bgra[i * 4 + 2];
                px.rgba[i * 4 + 1] = bgra[i * 4 + 1];
                px.rgba[i * 4 + 2] = bgra[i * 4 + 0];
                px.rgba[i * 4 + 3] = anyAlpha ? bgra[i * 4 + 3] : (mask.empty() || mask[i * 4] == 0 ? 255 : 0);
            }
            out = std::move(px);
        }
    }
    if (ii.hbmColor)
        DeleteObject(ii.hbmColor);
    if (ii.hbmMask)
        DeleteObject(ii.hbmMask);
    return out;
}

bool exists(const std::wstring &p)
{
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

} // namespace

std::optional<IconPixels> programIcon(const std::string &source, int size)
{
    auto [path, index] = splitIndex(nss::widen(source));
    if (path.size() >= 2 && path.front() == L'"' && path.back() == L'"')
        path = path.substr(1, path.size() - 2);
    if (path.empty())
        return std::nullopt;
    if (!exists(path)) {
        // A name Windows finds by itself: "calc.exe", "mspaint".
        wchar_t buf[MAX_PATH * 2]{};
        DWORD n = SearchPathW(nullptr, path.c_str(), L".exe", DWORD(std::size(buf)), buf, nullptr);
        if (!n || n >= std::size(buf))
            return std::nullopt;
        path.assign(buf, n);
    }
    auto ext = path.size() > 4 ? path.substr(path.size() - 4) : std::wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);

    HICON icon = nullptr;
    bool program = ext == L".exe" || ext == L".dll" || ext == L".ico";
    if (program && SHDefExtractIconW(path.c_str(), index, 0, &icon, nullptr, UINT(size)) != S_OK)
        icon = nullptr;
    if (!icon) {
        // Shortcuts, documents, scripts: the icon Explorer shows.
        SHFILEINFOW info{};
        if (SHGetFileInfoW(path.c_str(), 0, &info, sizeof info, SHGFI_ICON | SHGFI_LARGEICON) && info.hIcon)
            icon = info.hIcon;
    }
    if (!icon)
        return std::nullopt;
    auto out = toPixels(icon);
    DestroyIcon(icon);
    return out;
}
