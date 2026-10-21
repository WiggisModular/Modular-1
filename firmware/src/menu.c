/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 morgulbrut */
/* Two pages of the panel, after ui.c and before main.c:
 *
 * MIXER (GLO): K1 / K2 the level of voice 1 / 2, the low F# (OP1) and G# (OP2) keys mute them (and play no notes then).
 *
 * MENU (HOME held 0.7 s, HOME again closes it): the settings, in tabs like Felucca's menu (its layout, its rows are ours).
 * ALGORITHM steps between the tabs, PRESET and SELECT between the rows, K1..K4 and OCT- / OCT+ change the value of the
 * selected row. The values are applied at once and saved with the state (SAVE: main.c save bits).
 *
 * The state is plain bytes the main loop writes; the audio ISR only sees the voice levels through eng_voice_gain. */

#ifndef FELUCCA_VERSION
#define FELUCCA_VERSION "M1"
#endif
static void leds_avail(void);
static void leds_octave(void);
static void uboot_now(void);
static void defaults_now(void);

/* ---- mixer */

/* the keys that mute voice 1..3: the black keys F#3, G#3, A#3 (OP1..OP3) */
static const uint8_t MIX_KEY_BIT[PVOICES] = {1, 3, 5};
#define MIX_KEYS ((1u << 1) | (1u << 3) | (1u << 5))

static uint8_t mix_vol[PVOICES] = {100, 100, 100};   /* percent, square law (so the lower half has some travel) */
static uint8_t mix_mute[PVOICES];

static void mix_apply(uint32_t v)
{
    uint32_t p = mix_vol[v];
    int32_t g = mix_mute[v] ? 0 : (int32_t)((p * p * 65536u) / 10000u);
    if (v < ENG_VOICES)
        eng_voice_gain(v, g);
    else
        dr_gain = g;                                       /* the drums */
    fm1_led_key(14u + MIX_KEY_BIT[v], mix_mute[v]);        /* the mute key's LED is lit while its voice is muted */
}

static void mix_apply_all(void)
{
    uint32_t v;
    for (v = 0; v < PVOICES; v++)
        mix_apply(v);
}

static void mix_row(uint32_t v, int32_t y)
{
    char line[40], num[12], *p = line;
    uint16_t col = VOICE_COLOR[v];
    uint32_t w = (uint32_t)mix_vol[v] * 216u / 100u;
    p = put(p, "K");
    *p++ = (char)('1' + v);
    p = put(p, "  VOICE ");
    *p++ = (char)('1' + v);
    if (v == DRUM_V)
        p = put(p, " DRUMS");
    p = put(p, "  (OP");
    *p++ = (char)('1' + v);
    p = put(p, ": MUTE)");
    pad(p, line, 28);
    text5x7(12, (uint32_t)y, line, 1, C_DIM, C_BG);
    if (mix_mute[v]) {
        p = put(line, "MUTED");
    } else {
        fmt_int(num, mix_vol[v]);
        p = put(line, num);
        *p++ = '%';
    }
    pad(p, line, 14);
    text_mid(12, (uint32_t)y + 11u, line, mix_mute[v] ? C_RED : C_FG, C_BG);
    lcd_fill(12, (uint32_t)y + 27u, w, 4, mix_mute[v] ? C_DIM : col);
    lcd_fill(12 + w, (uint32_t)y + 27u, 216u - w, 4, 0x2104u);
}

static void mixer_draw(int full)
{
    uint32_t v;
    if (full)
        text5x7(12, 226, "K1-K3: LEVEL  OP1-OP3 KEYS: MUTE", 1, 0x4208u, C_BG);
    for (v = 0; v < PVOICES; v++)
        mix_row(v, 32 + 48 * (int32_t)v);
}

static void mix_turn(uint32_t v, int32_t clicks, int fast)
{
    int32_t x = (int32_t)mix_vol[v] + clicks * (fast ? 5 : 1);
    mix_vol[v] = (uint8_t)(x < 0 ? 0 : x > 100 ? 100 : x);
    mix_apply(v);
    if (!ui_dirty)
        ui_dirty = 1;
}

static void mix_toggle(uint32_t v)
{
    mix_mute[v] ^= 1u;
    mix_apply(v);
    if (!ui_dirty)
        ui_dirty = 1;
}

/* ---- settings menu */

enum { MR_LEDS, MR_CPU, MR_ACCEL, MR_OCT, MR_CH1, MR_CH2, MR_CH3, MR_CLOCK, MR_PROG, MR_VERSION, MR_PEAK, MR_UPDATE, MR_DEFAULTS, MR_COUNT };
enum { MT_DISPLAY, MT_CONTROL, MT_MIDI, MT_SYSTEM, MT_MANUAL, MT_COUNT };   /* (MANUAL has no rows: a text) */
static const char *const ROW_NAME[MR_COUNT] = {"LEDS", "CPU IN BAR", "KNOB ACCEL", "OCTAVE", "V1 CHANNEL", "V2 CHANNEL", "DRUM CHANNEL", "CLOCK IN", "PROG CHANGE",
                                              "VERSION", "CPU PEAK", "UPDATE MODE", "DEFAULTS"};
static const uint8_t ROW_TAB[MR_COUNT] = {MT_DISPLAY, MT_DISPLAY, MT_CONTROL, MT_CONTROL, MT_MIDI, MT_MIDI, MT_MIDI, MT_MIDI, MT_MIDI, MT_SYSTEM, MT_SYSTEM, MT_SYSTEM, MT_SYSTEM};
static const char *const TAB_NAME[MT_COUNT] = {"DISPLAY", "CONTROL", "MIDI", "SYSTEM", "MANUAL"};

static uint8_t menu_tab, menu_sel = MR_LEDS;     /* the tab and the selected row (a row number) */
static uint32_t cpu_peak;                       /* the highest CPU % since boot */
static uint32_t update_arm;                     /* UPDATE MODE / DEFAULTS: fm1_ms | 1 of the first OCT+ (the row is menu_sel) */

static uint32_t menu_first(uint32_t tab)
{
    uint32_t r = 0;
    while (r < MR_COUNT && ROW_TAB[r] != tab)
        r++;
    return r < MR_COUNT ? r : 0u;                 /* (the manual has no rows) */
}

static uint32_t menu_count(uint32_t tab)
{
    uint32_t r, n = 0;
    for (r = 0; r < MR_COUNT; r++)
        n += ROW_TAB[r] == tab;
    return n;
}

static void menu_value(uint32_t row, char *out)
{
    static const char *const ONOFF[2] = {"OFF", "ON"};
    char *p = out;
    switch (row) {
    case MR_LEDS: p = put(p, cfg.leds == 0 ? "OFF" : cfg.leds == 1 ? "DIM LOW" : "DIM HIGH"); break;
    case MR_CPU: p = put(p, ONOFF[cfg.cpu_bar]); break;
    case MR_ACCEL: p = put(p, ONOFF[cfg.accel]); break;
    case MR_OCT:
        if (octave > 0)
            *p++ = '+';
        fmt_int(p, octave);
        p += str_len(p);
        break;
    case MR_CH1:
    case MR_CH2:
    case MR_CH3: {
        uint32_t c = cfg.ch[row - MR_CH1];
        if (!c) {
            p = put(p, "OMNI");
        } else {
            fmt_int(p, (int32_t)c);
            p += str_len(p);
        }
        break;
    }
    case MR_CLOCK: p = put(p, ONOFF[cfg.clock_in]); break;
    case MR_PROG: p = put(p, ONOFF[cfg.prog_chg]); break;
    case MR_VERSION: p = put(p, FELUCCA_VERSION); break;
    case MR_PEAK:
        fmt_int(p, (int32_t)cpu_peak);
        p += str_len(p);
        *p++ = '%';
        break;
    default: p = put(p, update_arm && row == menu_sel ? "OCT+ AGAIN" : "OCT+ TWICE"); break;
    }
    *p = 0;
}

static void menu_row_draw(uint32_t row, uint32_t y, int hot)
{
    char name[20], val[16], *p;
    uint32_t w;
    uint16_t bg = hot ? 0x2104u : C_BG;
    lcd_fill(0, y, 240, 22, bg);
    p = put(name, ROW_NAME[row]);
    *p = 0;
    text_mid(12, y + 5u, name, hot ? C_FG : C_DIM, bg);
    if (hot)
        lcd_fill(0, y, 4, 22, C_ACC);
    menu_value(row, val);
    w = str_len(val) * 9u;
    text_mid(228u - w, y + 5u, val, hot ? C_ACC : C_FG, bg);
}

/* the MANUAL tab (manual.c): MAN_VIS lines from man_top, SELECT scrolls */
#include "manual.c"
#define MAN_Y 62u
#define MAN_VIS 16u
#define MAN_PITCH 10u
static uint32_t man_top;

static void manual_draw(void)
{
    uint32_t i, h = MAN_VIS * MAN_PITCH, thumb, ty;
    for (i = 0; i < MAN_VIS; i++) {
        uint32_t k = man_top + i, y = MAN_Y + i * MAN_PITCH, c = 0, nm = 0;
        char line[MAN_COLS + 2u], *p = line;
        uint16_t col = C_FG;
        const char *s = k < MAN_N ? MANUAL[k] : "";
        if (s[0] == '#')
            col = C_ACC;
        else if (s[0] == '~')
            col = C_DIM;
        else if (s[0] == ' ' && s[1] != ' ')         /* "NAME: text": the name (up to a colon in the first 18) in the accent colour */
            while (s[1u + c] && c < 18u && s[1u + c] != ':')
                c++;
        if (s[0] == ' ' && s[1] != ' ' && s[1u + c] == ':')
            nm = c + 1u;
        if (nm) {                                   /* the name with its colon, then the rest */
            uint32_t j;
            for (j = 0; j < nm; j++)
                line[j] = s[1u + j];
            line[nm] = 0;
            text5x7(12, y, line, 1, C_ACC, C_BG);
            p = put(line, s + 1u + nm);
            pad(p, line, MAN_COLS - nm);
            text5x7(12u + 6u * nm, y, line, 1, col, C_BG);
            continue;
        }
        if (s[0])
            p = put(p, s + 1);
        pad(p, line, MAN_COLS);
        text5x7(12, y, line, 1, col, C_BG);
    }
    thumb = h * MAN_VIS / MAN_N;                    /* the scroll bar at the right */
    thumb = thumb < 8u ? 8u : thumb;
    ty = MAN_N > MAN_VIS ? (h - thumb) * man_top / (MAN_N - MAN_VIS) : 0u;
    lcd_fill(232, MAN_Y, 4, h, 0x2104u);
    lcd_fill(232, MAN_Y + ty, 4, thumb, C_ACC);
}

static void manual_scroll(int32_t s)
{
    int32_t t = (int32_t)man_top + s, max = (int32_t)MAN_N - (int32_t)MAN_VIS;
    t = t > max ? max : t < 0 ? 0 : t;
    if ((uint32_t)t != man_top) {
        man_top = (uint32_t)t;
        ui_dirty = 1;
    }
}

static void menu_draw(int full)
{
    static uint8_t shown = 255;
    uint32_t t, r, n = menu_count(menu_tab), y = 72u, first = menu_first(menu_tab);
    if (full || shown != menu_tab) {                /* (a new tab: the page is cleared, the hint changes) */
        lcd_fill(0, 24, 240, 216, C_BG);
        text5x7(12, 230, menu_tab == MT_MANUAL ? "SELECT: SCROLL      ALGO: TAB" : "ALGO: TAB  PRESET: ROW  K1-4: VALUE", 1, 0x4208u, C_BG);
        shown = menu_tab;
    }
    for (t = 0; t < MT_COUNT; t++)       /* the tabs: bars, the open one in the accent colour */
        lcd_fill(12u + t * 44u, 30, 40, 4, t == menu_tab ? C_ACC : 0x2104u);
    {
        char line[20], *p = put(line, TAB_NAME[menu_tab]);
        pad(p, line, 12);
        text_mid(12, 42, line, C_ACC, C_BG);
    }
    if (menu_tab == MT_MANUAL) {
        manual_draw();
        return;
    }
    for (r = 0; r < n; r++)
        menu_row_draw(first + r, y + r * 24u, first + r == menu_sel);
    if (y + n * 24u < 224u)
        lcd_fill(0, y + n * 24u, 240, 224u - (y + n * 24u), C_BG);   /* (the rows a longer tab had) */
}

static void menu_open(void)             /* via page_set (main.c): the tab and row it was left at */
{
    if (ROW_TAB[menu_sel] != menu_tab)
        menu_sel = (uint8_t)menu_first(menu_tab);
    update_arm = 0;
}

static void menu_set_row(uint32_t row, int32_t s)       /* s > 0: the next value, s < 0: the previous (stopping at the ends) */
{
    int up = s > 0;
    int32_t d = s > 0 ? 1 : -1;
    if (menu_tab == MT_MANUAL)
        return;                                     /* (no row: the buttons do nothing on the manual) */
    switch (row) {
    case MR_LEDS: {
        int32_t v = (int32_t)cfg.leds + d;
        cfg.leds = (uint8_t)(v < 0 ? 0 : v > 2 ? 2 : v);
        leds_avail();
        break;
    }
    case MR_CPU: cfg.cpu_bar = (uint8_t)up; header_draw(); break;
    case MR_ACCEL: cfg.accel = (uint8_t)up; break;
    case MR_OCT:
        octave += d;
        octave = octave < -3 ? -3 : octave > 3 ? 3 : octave;
        leds_octave();
        break;
    case MR_CH1:
    case MR_CH2:
    case MR_CH3: {
        int32_t v = (int32_t)cfg.ch[row - MR_CH1] + d;
        cfg.ch[row - MR_CH1] = (uint8_t)(v < 0 ? 0 : v > 16 ? 16 : v);
        break;
    }
    case MR_CLOCK: cfg.clock_in = (uint8_t)up; break;
    case MR_PROG: cfg.prog_chg = (uint8_t)up; break;
    case MR_UPDATE:
    case MR_DEFAULTS:
        if (!up)
            break;
        if (update_arm && fm1_ms - (update_arm & ~1u) < 3000u) {
            update_arm = 0;
            if (row == MR_UPDATE)
                uboot_now();
            else
                defaults_now();
        } else {
            update_arm = fm1_ms | 1u;
        }
        break;
    default: break;
    }
    ui_dirty = 1;
}

static void menu_move(int32_t s)                        /* PRESET / SELECT: the next / previous row of the tab */
{
    uint32_t first = menu_first(menu_tab), n = menu_count(menu_tab), k = menu_sel - first;
    if (!n)
        return;
    k = (k + (s > 0 ? 1u : n - 1u)) % n;
    menu_sel = (uint8_t)(first + k);
    update_arm = 0;
    ui_dirty = 1;
}

static void menu_tab_go(int32_t s)                      /* ALGORITHM: the next / previous tab */
{
    uint32_t t = (menu_tab + (s > 0 ? 1u : MT_COUNT - 1u)) % MT_COUNT;
    menu_tab = (uint8_t)t;
    menu_sel = (uint8_t)menu_first(t);
    update_arm = 0;
    ui_dirty = 1;
}

/* the knobs while the menu is open: st[] as in knobs_poll */
static void menu_knobs(const int32_t *st)
{
    uint32_t k;
    if (st[1])
        menu_tab_go(st[1]);
    if (menu_tab == MT_MANUAL) {                    /* the manual: SELECT (and PRESET) scroll */
        manual_scroll(st[0] + st[6]);
        return;
    }
    if (st[6])
        menu_move(st[6]);
    else if (st[0])
        menu_move(st[0]);
    for (k = 0; k < 4u; k++)
        if (st[2u + k])
            menu_set_row(menu_sel, st[2u + k]);
}

/* the CPU load in the bar: the peak ISR time of the last half second, in % of the 128 frame half buffer (2.9 ms) */
static void cpu_poll(void)
{
    static uint32_t t0;
    uint32_t pct;
    if (fm1_ms - t0 < 500u)
        return;
    t0 = fm1_ms;
    pct = eng_us_max * 100u / 2902u;
    eng_us_max = 0;
    if (pct > 999u)
        pct = 999u;
    if (pct > cpu_peak)
        cpu_peak = pct;
    if (pct != cpu_pct) {
        cpu_pct = pct;
        if (cfg.cpu_bar && !msg_until && !load_err)
            header_draw();
        if (page == PAGE_MENU && ROW_TAB[MR_PEAK] == menu_tab && !ui_dirty)
            ui_dirty = 1;
    }
}
