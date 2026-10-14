#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Colors for one terminal skin, all 0xRRGGBB.
typedef struct {
    uint32_t background;
    uint32_t surface;
    uint32_t primary;
    uint32_t secondary;
    uint32_t accent;
    uint32_t track;
    uint32_t border;
    uint8_t glow_opacity;
} passport_palette_t;

// Derive a dark single-color terminal skin from one user color, matching the
// proportions of the built-in amber and digital-green skins. Colors too dark
// to read on the dark background are brightened while keeping their hue.
void passport_palette_from_color(uint32_t color, passport_palette_t *palette);

#ifdef __cplusplus
}
#endif
