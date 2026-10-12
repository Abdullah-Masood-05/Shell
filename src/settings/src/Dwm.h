#pragma once

// Native title bar styling. The client area is painted solid by Slint (Mica cannot show
// through a Slint window), so Mica only shows on the title bar.

#include <windows.h>

namespace dwm {

// Immersive dark title bar on/off, plus the Mica system backdrop (Windows 11 22H2+;
// silently ignored on older systems).
void applyTitleBar(HWND hwnd, bool dark);

// Sets the window's small and large icons from the exe's icon resource.
void applyIcon(HWND hwnd);

} // namespace dwm
