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

/* the keys that mute voice 1..4: the black keys F#3, G#3, A#3, C#4 (OP1..OP4) */
static const uint8_t MIX_KEY_BIT[ENG_VOICES] = {1, 3, 5, 8};
#define MIX_KEYS ((1u << 1) | (1u << 3) | (1u << 5) | (1u << 8))

static uint8_t mix_vol[ENG_VOICES] = {100, 100, 100, 100};   /* percent, square law (so the lower half has some travel) */
static uint8_t mix_mute[ENG_VOICES];

static void mix_apply(uint32_t v)
{
    uint32_t p = mix_vol[v];
    eng_voice_gain(v, mix_mute[v] ? 0 : (int32_t)((p * p * 65536u) / 10000u));
    fm1_led_key(14u + MIX_KEY_BIT[v], mix_mute[v]);        /* the mute key's LED is lit while its voice is muted */
}

static void mix_apply_all(void)
{
    uint32_t v;
    for (v = 0; v < ENG_VOICES; v++)
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
        text5x7(12, 226, "K1-K4: LEVEL  OP1-OP4 KEYS: MUTE", 1, 0x4208u, C_BG);
    for (v = 0; v < ENG_VOICES; v++)
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

enum { MR_LEDS, MR_CPU, MR_ACCEL, MR_OCT, MR_CH1, MR_CH2, MR_CH3, MR_CH4, MR_CLOCK, MR_PROG, MR_VERSION, MR_PEAK, MR_UPDATE, MR_DEFAULTS, MR_COUNT };
enum { MT_DISPLAY, MT_CONTROL, MT_MIDI, MT_SYSTEM, MT_COUNT };
static const char *const ROW_NAME[MR_COUNT] = {"LEDS", "CPU IN BAR", "KNOB ACCEL", "OCTAVE", "V1 CHANNEL", "V2 CHANNEL", "V3 CHANNEL", "V4 CHANNEL", "CLOCK IN", "PROG CHANGE",
                                              "VERSION", "CPU PEAK", "UPDATE MODE", "DEFAULTS"};
static const uint8_t ROW_TAB[MR_COUNT] = {MT_DISPLAY, MT_DISPLAY, MT_CONTROL, MT_CONTROL, MT_MIDI, MT_MIDI, MT_MIDI, MT_MIDI, MT_MIDI, MT_MIDI, MT_SYSTEM, MT_SYSTEM, MT_SYSTEM, MT_SYSTEM};
static const char *const TAB_NAME[MT_COUNT] = {"DISPLAY", "CONTROL", "MIDI", "SYSTEM"};

static uint8_t menu_tab, menu_sel = MR_LEDS;     /* the tab and the selected row (a row number) */
static uint32_t cpu_peak;                       /* the highest CPU % since boot */
static uint32_t update_arm;                     /* UPDATE MODE / DEFAULTS: fm1_ms | 1 of the first OCT+ (the row is menu_sel) */

static uint32_t menu_first(uint32_t tab)
{
    uint32_t r = 0;
    while (ROW_TAB[r] != tab)
        r++;
    return r;
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
    case MR_CH3:
    case MR_CH4: {
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

static void menu_draw(int full)
{
    uint32_t t, r, n = menu_count(menu_tab), y = 72u, first = menu_first(menu_tab);
    if (full) {
        lcd_fill(0, 24, 240, 216, C_BG);
        text5x7(12, 230, "ALGO: TAB  PRESET: ROW  K1-4: VALUE", 1, 0x4208u, C_BG);
    }
    for (t = 0; t < MT_COUNT; t++)       /* the tabs: four bars, the open one in the accent colour */
        lcd_fill(12u + t * 55u, 30, 51, 4, t == menu_tab ? C_ACC : 0x2104u);
    {
        char line[20], *p = put(line, TAB_NAME[menu_tab]);
        pad(p, line, 12);
        text_mid(12, 42, line, C_ACC, C_BG);
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
    case MR_CH3:
    case MR_CH4: {
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
