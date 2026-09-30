#ifndef MERCENARIES_APP_ICON_H
#define MERCENARIES_APP_ICON_H
#include <windows.h>
/* Keep the full embedded image for taskbar/Alt-Tab scaling. LoadIcon selects
 * a legacy system-size image. LR_SHARED also caches by resource without
 * distinguishing sizes, so caption and large icons need independent loads.
 * The small handle is also used by taskbar hover previews, which may request
 * it at a higher DPI than the caption. Retain a 64px source so those previews
 * downsample instead of enlarging a 16px bitmap. Both handles are reused. */
static HICON mercenaries_app_icon(HINSTANCE instance, int large)
{
    static HICON icons[2];
    const unsigned slot = large ? 1u : 0u;
    if (!icons[slot]) {
        const int width = large ? 256 : 64;
        const int height = large ? 256 : 64;
        icons[slot] = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(101),
            IMAGE_ICON, width, height, 0);
    }
    return icons[slot];
}
#endif
