#include "FileIcon.h"

#include <windows.h>
#include <shlobj.h>

namespace fileicon {

std::optional<std::vector<uint8_t>> extract(const std::wstring &path, int index, int size, uint32_t &w, uint32_t &h)
{
    HICON icon = nullptr;
    if (SHDefExtractIconW(path.c_str(), index, 0, &icon, nullptr, UINT(size)) != S_OK || !icon)
        return std::nullopt;
    std::optional<std::vector<uint8_t>> out;
    ICONINFO ii{};
    if (GetIconInfo(icon, &ii)) {
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
                std::vector<uint8_t> rgba(bgra.size());
                for (size_t i = 0; i < size_t(bw) * size_t(bh); ++i) {
                    rgba[i * 4 + 0] = bgra[i * 4 + 2];
                    rgba[i * 4 + 1] = bgra[i * 4 + 1];
                    rgba[i * 4 + 2] = bgra[i * 4 + 0];
                    rgba[i * 4 + 3] = anyAlpha ? bgra[i * 4 + 3] : (mask.empty() || mask[i * 4] == 0 ? 255 : 0);
                }
                w = uint32_t(bw);
                h = uint32_t(bh);
                out = std::move(rgba);
            }
        }
        if (ii.hbmColor)
            DeleteObject(ii.hbmColor);
        if (ii.hbmMask)
            DeleteObject(ii.hbmMask);
    }
    DestroyIcon(icon);
    return out;
}

} // namespace fileicon
