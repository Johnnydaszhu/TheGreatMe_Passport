#include <assert.h>
#include <stdio.h>

#include "passport_theme.h"

static unsigned peak(uint32_t color)
{
    unsigned red = (color >> 16) & 0xFFU;
    unsigned green = (color >> 8) & 0xFFU;
    unsigned blue = color & 0xFFU;
    unsigned value = red;
    if (green > value) value = green;
    if (blue > value) value = blue;
    return value;
}

int main(void)
{
    passport_palette_t palette;

    // A bright color is kept as the primary text color.
    passport_palette_from_color(0x3CFF6B, &palette);
    assert(palette.primary == 0x3CFF6B);
    assert(peak(palette.accent) == 0xFF);
    assert(palette.secondary == 0x249940);
    assert(palette.background < palette.surface);
    assert(palette.surface < palette.track);
    assert(palette.track < palette.secondary);
    assert(palette.glow_opacity > 0);

    // A dark pick is brightened but keeps its hue (blue stays the strongest).
    passport_palette_from_color(0x101040, &palette);
    assert(peak(palette.primary) >= 0xA0);
    assert((palette.primary & 0xFFU) == 0xA0);
    assert(((palette.primary >> 16) & 0xFFU) < (palette.primary & 0xFFU));

    // Black becomes a readable neutral gray instead of invisible text.
    passport_palette_from_color(0x000000, &palette);
    assert(palette.primary == 0xA0A0A0);

    // Bits above 0xFFFFFF are ignored.
    passport_palette_from_color(0xFF3CFF6B, &palette);
    assert(palette.primary == 0x3CFF6B);

    puts("passport theme tests passed");
    return 0;
}
