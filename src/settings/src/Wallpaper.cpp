#include "Wallpaper.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <filesystem>

using Microsoft::WRL::ComPtr;

namespace wallpaper {

namespace {

Pixels decode(const std::wstring &path, uint32_t width, uint32_t height)
{
    Pixels out;
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
        return out;
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                  WICDecodeMetadataCacheOnDemand, &decoder)))
        return out;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame)))
        return out;
    UINT w = 0, h = 0;
    if (FAILED(frame->GetSize(&w, &h)) || w == 0 || h == 0)
        return out;

    // Scale to cover width x height, then crop the middle.
    double scale = std::max(double(width) / w, double(height) / h);
    UINT sw = std::max<UINT>(width, UINT(std::ceil(w * scale)));
    UINT sh = std::max<UINT>(height, UINT(std::ceil(h * scale)));
    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(factory->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(frame.Get(), sw, sh, WICBitmapInterpolationModeFant)))
        return out;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr,
                                     0.0, WICBitmapPaletteTypeCustom)))
        return out;
    WICRect rect{ INT((sw - width) / 2), INT((sh - height) / 2), INT(width), INT(height) };
    out.width = width;
    out.height = height;
    out.rgba.resize(size_t(width) * height * 4);
    if (FAILED(converter->CopyPixels(&rect, width * 4, UINT(out.rgba.size()), out.rgba.data()))) {
        out = {};
        return out;
    }
    for (size_t i = 3; i < out.rgba.size(); i += 4)
        out.rgba[i] = 255;
    return out;
}

// No wallpaper file: the desktop colour with a soft light falloff.
Pixels fallback(uint32_t width, uint32_t height)
{
    Pixels out;
    out.width = width;
    out.height = height;
    out.rgba.resize(size_t(width) * height * 4);
    COLORREF c = GetSysColor(COLOR_DESKTOP);
    bool black = GetRValue(c) + GetGValue(c) + GetBValue(c) < 30;
    double r = black ? 0x1B : GetRValue(c), g = black ? 0x3A : GetGValue(c), b = black ? 0x6E : GetBValue(c);
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x) {
            double dx = (double(x) / width - 0.7), dy = (double(y) / height - 0.35);
            double light = std::clamp(1.25 - std::sqrt(dx * dx + dy * dy) * 1.1, 0.45, 1.25);
            uint8_t *p = &out.rgba[(size_t(y) * width + x) * 4];
            p[0] = uint8_t(std::min(255.0, r * light));
            p[1] = uint8_t(std::min(255.0, g * light));
            p[2] = uint8_t(std::min(255.0, b * light));
            p[3] = 255;
        }
    return out;
}

Pixels shrink(const Pixels &src, uint32_t factor)
{
    Pixels out;
    out.width = std::max<uint32_t>(1, src.width / factor);
    out.height = std::max<uint32_t>(1, src.height / factor);
    out.rgba.resize(size_t(out.width) * out.height * 4);
    for (uint32_t y = 0; y < out.height; ++y)
        for (uint32_t x = 0; x < out.width; ++x) {
            uint32_t sum[4] = {};
            uint32_t n = 0;
            for (uint32_t yy = y * factor; yy < std::min(src.height, (y + 1) * factor); ++yy)
                for (uint32_t xx = x * factor; xx < std::min(src.width, (x + 1) * factor); ++xx) {
                    const uint8_t *p = &src.rgba[(size_t(yy) * src.width + xx) * 4];
                    for (int k = 0; k < 4; ++k)
                        sum[k] += p[k];
                    ++n;
                }
            uint8_t *d = &out.rgba[(size_t(y) * out.width + x) * 4];
            for (int k = 0; k < 4; ++k)
                d[k] = uint8_t(n ? sum[k] / n : 0);
        }
    return out;
}

} // namespace

void blur(Pixels &px, int radius)
{
    if (px.empty() || radius <= 0)
        return;
    const int w = int(px.width), h = int(px.height);
    std::vector<uint8_t> tmp(px.rgba.size());
    auto pass = [&](const std::vector<uint8_t> &in, std::vector<uint8_t> &out, bool horizontal) {
        const int len = horizontal ? w : h, lines = horizontal ? h : w;
        for (int line = 0; line < lines; ++line) {
            auto at = [&](int i) -> const uint8_t * {
                i = std::clamp(i, 0, len - 1);
                int x = horizontal ? i : line, y = horizontal ? line : i;
                return &in[(size_t(y) * w + x) * 4];
            };
            int sum[4] = {};
            for (int i = -radius; i <= radius; ++i)
                for (int k = 0; k < 4; ++k)
                    sum[k] += at(i)[k];
            const int div = radius * 2 + 1;
            for (int i = 0; i < len; ++i) {
                int x = horizontal ? i : line, y = horizontal ? line : i;
                uint8_t *d = &out[(size_t(y) * w + x) * 4];
                for (int k = 0; k < 4; ++k)
                    d[k] = uint8_t(sum[k] / div);
                const uint8_t *add = at(i + radius + 1), *sub = at(i - radius);
                for (int k = 0; k < 4; ++k)
                    sum[k] += add[k] - sub[k];
            }
        }
    };
    for (int n = 0; n < 3; ++n) {
        pass(px.rgba, tmp, true);
        pass(tmp, px.rgba, false);
    }
}

Images load(uint32_t width, uint32_t height)
{
    Images out;
    wchar_t path[MAX_PATH * 2] = {};
    if (SystemParametersInfoW(SPI_GETDESKWALLPAPER, UINT(std::size(path)), path, 0) && path[0]) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(path, ec))
            out.sharp = decode(path, width, height);
    }
    if (out.sharp.empty())
        out.sharp = fallback(width, height);
    out.blurred = shrink(out.sharp, 4);
    blur(out.blurred, 5);
    return out;
}

} // namespace wallpaper
