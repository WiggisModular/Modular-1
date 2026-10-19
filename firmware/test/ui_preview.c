/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 morgulbrut */
/* Renders the device's LCD (firmware/src/ui.c) on the host into a PPM image, to look at the layout without a device.
 *   ui_preview OUT.ppm PRESET_INDEX [SELECTED_MODULE [OCTAVE [PAGE [STEP]]]]   (PAGE 9: the drum matrix, 8: its sound page)
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
static uint32_t fm1_ticks(void) { return 0; }
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
#include "../src/arp.c"
#include "../src/drums.c"
#include "../src/ui.c"
#include "../src/arp_ui.c"
static void fm1_led_key(uint32_t id, int on) { (void)id; (void)on; }
static void leds_avail(void) {}
static void leds_arp(void) {}
static void leds_octave(void) {}
static void uboot_now(void) {}
static void defaults_now(void) {}
#include "../src/menu.c"
#include "../src/drum_ui.c"

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
    page = argc > 5 ? (uint8_t)atoi(argv[5]) : 0u;          /* 1..3: the arpeggiator pages; 9: the drums (STEP = the playhead) */
    if (page == 9u || page == 8u) {                         /* 9: the drum matrix, 8: its sound page (STEP = the playhead; PRESET_INDEX picks the drum pattern) */
        page = page == 8u ? 1u : 0u;
        dsel = 1;
        dr_defaults();
        dr_load_preset(p);                                  /* the pattern is the PRESET_INDEX (modulo 8) */
        dr.part = DR_CHAT;
        dr.cur = 14;
        dr_play = argc > 6;
        dr_step_ui = argc > 6 ? (uint8_t)atoi(argv[6]) : 0u;
        argc = 6;
    }
    if (argc > 6) {                                         /* page 2: a playing step */
        arp_play = 1;
        arp_step_ui = (uint8_t)atoi(argv[6]);
    }
    booting = 0;
    if (argc > 7) {                                         /* page 5: the menu's tab (4 = MANUAL) */
        int t = atoi(argv[7]);
        menu_tab = (uint8_t)(t < 0 ? 0 : t >= MT_COUNT ? MT_COUNT - 1 : t);
    }
    if (argc > 8) {                                         /* .. and the first line of the manual */
        int t = atoi(argv[8]), max = (int)MAN_N - (int)MAN_VIS;
        man_top = (uint32_t)(t < 0 ? 0 : t > max ? max : t);
    }
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
