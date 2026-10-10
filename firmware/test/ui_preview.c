/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 morgulbrut */
/* Renders the device's LCD (firmware/src/ui.c) on the host into a PPM image, to look at the layout without a device.
 *   ui_preview OUT.ppm PRESET_INDEX [SELECTED_MODULE [OCTAVE]]
 * tools/ui_preview.sh converts the image to PNG. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define memset m_memset                     /* libc.c defines the freestanding versions */
#define memcpy m_memcpy
#define memcmp m_memcmp
#include "../src/libc.c"
#undef memset
#undef memcpy
#undef memcmp

static uint16_t fb[240 * 240];
static uint32_t fm1_ms;
static uint32_t eng_us_last = 316, eng_us_max = 358;
#define FELUCCA_VERSION "M1-PREVIEW"
#define FM1_TICKS_PER_US 24u

static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
    uint32_t i, j;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            if (x + i < 240u && y + j < 240u)
                fb[(y + j) * 240u + x + i] = c;
}
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px)
{
    uint32_t i, j;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            if (x + i < 240u && y + j < 240u) {
                uint16_t v = px[j * w + i];
                fb[(y + j) * 240u + x + i] = (uint16_t)((v >> 8) | (v << 8));     /* the LCD takes big endian */
            }
}
static void lcd_sync(void) {}

#include "../src/engine.c"
#include "../src/presets.h"
#include "../src/font5x7.c"
#include "../src/ui.c"

int main(int argc, char **argv)
{
    uint32_t i, p;
    FILE *f;
    if (argc < 3)
        return 2;
    p = (uint32_t)atoi(argv[2]);
    if (p >= N_PRESETS || eng_load(PRESET_LIST[p].data, PRESET_LIST[p].len)) {
        fprintf(stderr, "no such preset\n");
        return 1;
    }
    {   /* one block, so the modules have computed their effective parameters */
        static int32_t l[128], r[128];
        eng_render(l, r, 128);
    }
    sel = argc > 3 ? (uint32_t)atoi(argv[3]) : 0u;
    octave = argc > 4 ? atoi(argv[4]) : 0;
    patch_label = PRESET_LIST[p].name;
    screen_idle();
    f = fopen(argv[1], "wb");
    fprintf(f, "P6\n240 240\n255\n");
    for (i = 0; i < 240u * 240u; i++) {
        uint16_t c = fb[i];
        fputc(((c >> 11) & 31u) * 255u / 31u, f);
        fputc(((c >> 5) & 63u) * 255u / 63u, f);
        fputc((c & 31u) * 255u / 31u, f);
    }
    fclose(f);
    return 0;
}
