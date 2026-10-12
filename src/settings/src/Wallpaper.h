#pragma once

// The desktop wallpaper for the Appearance preview: SPI_GETDESKWALLPAPER decoded
// with WIC, cropped to fill and shrunk, plus a small blurred copy for the blur
// and acrylic effects. Falls back to a generated gradient when there is no
// wallpaper file (a solid colour or a slideshow without a current image).

#include <cstdint>
#include <vector>

namespace wallpaper {

struct Pixels
{
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba; // width * height * 4

    bool empty() const { return width == 0 || height == 0; }
};

struct Images
{
    Pixels sharp;   // about width x height
    Pixels blurred; // a quarter of that, blurred; Slint scales it up
};

// Runs on a COM-initialised thread (the Worker).
Images load(uint32_t width = 960, uint32_t height = 640);

// Box blur, three passes (close to a Gaussian). In place.
void blur(Pixels &px, int radius);

} // namespace wallpaper
