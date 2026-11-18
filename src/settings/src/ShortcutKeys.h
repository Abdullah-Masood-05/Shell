#pragma once

// Ctrl+C, Ctrl+V, Ctrl+A... in a keyboard layout whose letter keys type other
// letters (Arabic, Russian, Greek, Hebrew). Slint matches its shortcuts, and the
// app's own Ctrl+letter checks in ui/, by the text of the key, and winit gives it
// what the layout types: Ctrl+C in Arabic arrives as Ctrl+"ؤ", so the text boxes
// and the code editor copy, paste, select and undo nothing. Windows itself goes by
// the key (the virtual-key code), which is 'C' in every layout, so SaveService's
// window hook hands Slint the Latin letter of the key instead.
//
// Note: MapVirtualKeyEx(MAPVK_VK_TO_CHAR) can't tell: it returns 'A'..'Z' for the
// letter keys in every layout. ToUnicodeEx gives what the layout really types.

#include <windows.h>

namespace shortcutkeys {

// The letter to give Slint for Ctrl+<vk>: 'a'..'z' ('A'..'Z' with Shift), or 0 to
// leave the key alone (vk isn't a letter key, or the layout already types an ASCII
// letter on it, as English, French or Dvorak do). `typed` is what the layout types
// on the key without modifiers and `count` ToUnicodeEx's return for it (0: nothing,
// < 0: a dead key).
char latinLetter(UINT vk, bool shift, const wchar_t *typed, int count);

// The same for a keyboard layout (GetKeyboardLayout of the window's thread). Reads
// the layout without changing the keyboard state or any dead key in progress.
char latinLetter(UINT vk, bool shift, HKL layout);

} // namespace shortcutkeys
