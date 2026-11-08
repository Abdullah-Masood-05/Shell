// ShortcutKeys: the Latin letter SaveService's window hook gives Slint for Ctrl+<key>
// in a non-Latin keyboard layout. The layout cases only use layouts already loaded in
// this session (GetKeyboardLayoutList): nothing is loaded or activated.

#include "doctest.h"

#include "ShortcutKeys.h"

#include <windows.h>

#include <vector>

using shortcutkeys::latinLetter;

namespace {

HKL loadedLayout(WORD langId)
{
    int n = GetKeyboardLayoutList(0, nullptr);
    std::vector<HKL> list(size_t(n > 0 ? n : 0));
    n = GetKeyboardLayoutList(n, list.data());
    for (int i = 0; i < n; ++i)
        if (LOWORD(reinterpret_cast<UINT_PTR>(list[size_t(i)])) == langId)
            return list[size_t(i)];
    return nullptr;
}

} // namespace

TEST_CASE("shortcut keys: a key that types another script gives its Latin letter")
{
    const wchar_t arabicC[] = L"\u0624"; // Arabic (101), the C key
    CHECK(latinLetter('C', false, arabicC, 1) == 'c');
    CHECK(latinLetter('C', true, arabicC, 1) == 'C');
    const wchar_t russianV[] = L"\u043C"; // Russian, the V key
    CHECK(latinLetter('V', false, russianV, 1) == 'v');
    const wchar_t turkishI[] = L"\u0131"; // Turkish Q, dotless i
    CHECK(latinLetter('I', false, turkishI, 1) == 'i');
    // A dead key or nothing typed: still the key's letter.
    CHECK(latinLetter('A', false, arabicC, -1) == 'a');
    CHECK(latinLetter('Z', false, nullptr, 0) == 'z');
    // The Arabic lam-alef key (B) types two characters.
    const wchar_t lamAlef[] = L"\u0644\u0627";
    CHECK(latinLetter('B', false, lamAlef, 2) == 'b');
}

TEST_CASE("shortcut keys: a layout that types an ASCII letter is left alone")
{
    CHECK(latinLetter('C', false, L"c", 1) == 0);
    CHECK(latinLetter('C', true, L"C", 1) == 0);
    // AZERTY / Dvorak: the key's letter is the layout's, whatever the position.
    CHECK(latinLetter('Q', false, L"q", 1) == 0);
    // Not a letter key.
    CHECK(latinLetter('1', false, L"\u0661", 1) == 0);
    CHECK(latinLetter(VK_INSERT, false, nullptr, 0) == 0);
}

TEST_CASE("shortcut keys: real layouts loaded in this session")
{
    if (HKL us = loadedLayout(0x0409)) {
        CHECK(latinLetter('C', false, us) == 0);
        CHECK(latinLetter('A', true, us) == 0);
    } else {
        MESSAGE("English (US) layout not loaded: skipped");
    }
    if (HKL ar = loadedLayout(0x0401)) {
        CHECK(latinLetter('C', false, ar) == 'c');
        CHECK(latinLetter('V', false, ar) == 'v');
        CHECK(latinLetter('A', false, ar) == 'a');
        // What the old check used: it says 'C' even here, so it never fired.
        CHECK((MapVirtualKeyExW('C', MAPVK_VK_TO_CHAR, ar) & 0x7FFFFFFF) == 'C');
    } else {
        MESSAGE("Arabic layout not loaded: skipped");
    }
}
