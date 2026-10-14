#include "passport_theme.h"

#include <stddef.h>

// Text on the near-black background needs at least this much in its
// brightest channel to stay legible on the device screen.
#define PASSPORT_MIN_PRIMARY_LEVEL 0xA0

static uint32_t channel(uint32_t color, unsigned shift)
{
    return (color >> shift) & 0xFFU;
}

static uint32_t pack(uint32_t red, uint32_t green, uint32_t blue)
{
    return (red << 16) | (green << 8) | blue;
}

// Scale toward black: permille 1000 keeps the color, 0 gives black.
static uint32_t shade(uint32_t color, uint32_t permille)
{
    return pack(
        channel(color, 16) * permille / 1000U,
        channel(color, 8) * permille / 1000U,
        channel(color, 0) * permille / 1000U
    );
}

// Blend toward white: permille 0 keeps the color, 1000 gives white.
static uint32_t tint(uint32_t color, uint32_t permille)
{
    uint32_t red = channel(color, 16);
    uint32_t green = channel(color, 8);
    uint32_t blue = channel(color, 0);
    return pack(
        red + (255U - red) * permille / 1000U,
        green + (255U - green) * permille / 1000U,
        blue + (255U - blue) * permille / 1000U
    );
}

static uint32_t legible(uint32_t color)
{
    uint32_t red = channel(color, 16);
    uint32_t green = channel(color, 8);
    uint32_t blue = channel(color, 0);
    uint32_t peak = red;
    if (green > peak) peak = green;
    if (blue > peak) peak = blue;
    if (peak == 0) return pack(PASSPORT_MIN_PRIMARY_LEVEL, PASSPORT_MIN_PRIMARY_LEVEL, PASSPORT_MIN_PRIMARY_LEVEL);
    if (peak >= PASSPORT_MIN_PRIMARY_LEVEL) return color & 0xFFFFFFU;
    return pack(
        red * PASSPORT_MIN_PRIMARY_LEVEL / peak,
        green * PASSPORT_MIN_PRIMARY_LEVEL / peak,
        blue * PASSPORT_MIN_PRIMARY_LEVEL / peak
    );
}

void passport_palette_from_color(uint32_t color, passport_palette_t *palette)
{
    if (!palette) return;
    uint32_t primary = legible(color);
    palette->primary = primary;
    palette->accent = tint(primary, 350);
    palette->secondary = shade(primary, 600);
    palette->border = shade(primary, 880);
    palette->track = shade(primary, 160);
    palette->surface = shade(primary, 50);
    palette->background = shade(primary, 20);
    palette->glow_opacity = 44;
}
