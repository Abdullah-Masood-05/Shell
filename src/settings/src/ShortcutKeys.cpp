#include "ShortcutKeys.h"

namespace shortcutkeys {

char latinLetter(UINT vk, bool shift, const wchar_t *typed, int count)
{
    if (vk < 'A' || vk > 'Z')
        return 0;
    if (count == 1 && typed && ((typed[0] >= L'a' && typed[0] <= L'z') || (typed[0] >= L'A' && typed[0] <= L'Z')))
        return 0;
    return char(shift ? vk : vk + ('a' - 'A'));
}

char latinLetter(UINT vk, bool shift, HKL layout)
{
    if (vk < 'A' || vk > 'Z')
        return 0;
    BYTE state[256] = {}; // no modifiers: what the key types on its own
    wchar_t typed[8] = {};
    const UINT scan = MapVirtualKeyExW(vk, MAPVK_VK_TO_VSC, layout);
    // Flag 4: leave the keyboard state (and a pending dead key) as it is.
    const int count = ToUnicodeEx(vk, scan, state, typed, 8, 4, layout);
    return latinLetter(vk, shift, typed, count);
}

} // namespace shortcutkeys
