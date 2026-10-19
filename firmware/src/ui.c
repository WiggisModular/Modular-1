/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 morgulbrut */
/* The LCD: patch name and CPU load, the octave, a small picture of the patch, the selected module with what the four
 * knobs change (for the FM-1 panel module: where each knob goes), and the two parameter inputs.
 *
 *   y   2  MODULAR-1 and version        y  40  3/8 FILTER (the selected module)
 *   y  19  patch name, octave, message  y  58  the patch picture, 220 x 56
 *   y  29  CPU                          y 120  K1..K4 rows        y 200 P1, P2 */
#define C_BG 0x0000u
#define C_FG 0xFFFFu
/* The panel's voices: the patch voices and, as the last one, the drum machine (drums.c, drum_ui.c). SEL steps through them.
 * `dsel` is set while the drums are selected (eng_voice() then stays at the patch voice used last). */
#define PVOICES (ENG_VOICES + 1u)
#define DRUM_V ENG_VOICES
static uint8_t dsel;
/* the accent: each voice has its own colour (orange, purple, green for the drums) */
static const uint16_t VOICE_COLOR[PVOICES] = {0xFD20u, 0xA19Fu, 0x7FE0u};
#define C_ACC (VOICE_COLOR[dsel ? DRUM_V : eng_voice()])
#define C_DIM 0x7BEFu
#define C_RED 0xF800u

/* the panel state of each voice (the patch it runs): SEL switches the voice the keys, knobs and LCD act on (eng_voice_select) */
static struct {
    uint32_t preset, module;               /* the preset in the list; the module the knobs act on (SELECT turns it) */
    int load_err;
    const char *label;
} vs[ENG_VOICES];
#define cur_preset (vs[eng_voice()].preset)
#define load_err (vs[eng_voice()].load_err)
#define patch_label (vs[eng_voice()].label)
#define PAGE_MIXER 4u                           /* GLO: the mixer (menu.c) */
#define PAGE_MENU 5u                            /* HOME held: the settings menu (menu.c) */
static uint8_t page;                            /* 0: the selected module, 1..3: the arpeggiator pages MELODY, RHYTHM, CHAOS (arp_ui.c), 4 mixer, 5 menu */

/* the settings of the menu (menu.c); saved with the state (SAVE) */
static struct {
    uint8_t leds;                       /* the idle buttons glow: 0 off, 1 dim low, 2 dim higher */
    uint8_t cpu_bar;                    /* the CPU load in the top bar */
    uint8_t accel;                      /* a knob turned fast steps 5 times as far */
    uint8_t ch[PVOICES];                /* the MIDI channel of each voice (the last: the drums): 1..16, 0 all (omni) */
    uint8_t clock_in;                   /* MIDI clock drives the clock output */
    uint8_t prog_chg;                   /* MIDI program change loads a preset */
} cfg = {2, 1, 1, {1, 2, 10}, 1, 1};
static void cfg_defaults(void)
{
    cfg.cpu_bar = cfg.accel = cfg.clock_in = cfg.prog_chg = 1;
    cfg.leds = 2;
    cfg.ch[0] = 1;
    cfg.ch[1] = 2;
    cfg.ch[2] = 10;                     /* the drums: the GM drum channel */
}
static uint32_t cpu_pct;                /* the audio ISR's peak time over the last half second, in % of the 2.9 ms half buffer */
#define sel (vs[eng_voice()].module)
static volatile uint8_t ui_dirty;               /* 1: the knob rows need a redraw, 2: the module changed too (title, picture) */
static int32_t octave;                          /* the keys' octave shift, -3..3 (OCT- / OCT+) */
static uint32_t msg_until;                      /* a message in the top bar (SAVED ...) until this fm1_ms */

static char *put(char *d, const char *s)
{
    while (*s)
        *d++ = *s++;
    return d;
}

static char *pad(char *d, const char *start, uint32_t width)
{
    while (d < start + width)
        *d++ = ' ';
    *d = 0;
    return d;
}

/* hundredths as -0.50 / 12.5 / 1200 (fmt_fix leaves out the leading zero) */
static char *put_hundredths(char *d, int32_t x, int decimals)
{
    int32_t a = x < 0 ? -x : x;
    char num[12];
    if (x < 0)
        *d++ = '-';
    if (decimals == 0) {
        fmt_int(num, (a + 50) / 100);
        return put(d, num);
    }
    fmt_int(num, a / 100);
    d = put(d, num);
    *d++ = '.';
    if (decimals == 1) {
        *d++ = (char)('0' + (a / 10) % 10);
    } else {
        *d++ = (char)('0' + (a / 10) % 10);
        *d++ = (char)('0' + a % 10);
    }
    *d = 0;
    return d;
}

/* a Q16 number, three significant figures: 1200, 12.5, 0.25 */
static char *put_num(char *d, int32_t q16)
{
    int64_t x = ((int64_t)q16 * 100) / 65536;            /* hundredths */
    int64_t a = x < 0 ? -x : x;
    return put_hundredths(d, (int32_t)x, a >= 10000 ? 0 : a >= 1000 ? 1 : 2);
}

/* parameter j of a module type as the LCD shows it: a choice by name (SAW), else the number and its unit (1200 HZ) */
static char *put_param(char *d, const def_t *def, uint32_t j, int32_t q16)
{
    if (def->pchoice && def->pchoice[j]) {
        const char *const *names = def->pchoice[j];
        int32_t i = (q16 + 32768) >> 16, n = 0;
        while (names[n])
            n++;
        if (def->pmorph && def->pmorph[j] && n > 1) {          /* between two shapes: SAW>SQU 40%, else the nearest */
            int32_t k = q16 >> 16, pct;
            k = k < 0 ? 0 : k > n - 2 ? n - 2 : k;
            pct = (int32_t)(((int64_t)(q16 - (k << 16)) * 100 + 32768) >> 16);
            if (pct > 3 && pct < 97) {
                char num[12];
                uint32_t c;
                for (c = 0; c < 3u && names[k][c]; c++)         /* the first three letters of each */
                    *d++ = names[k][c];
                *d++ = '>';
                for (c = 0; c < 3u && names[k + 1][c]; c++)
                    *d++ = names[k + 1][c];
                *d++ = ' ';
                fmt_int(num, pct);
                d = put(d, num);
                *d++ = '%';
                *d = 0;
                return d;
            }
        }
        i = i < 0 ? 0 : i >= n ? n - 1 : i;
        d = put(d, names[i]);
        *d = 0;
        return d;
    }
    d = put_num(d, q16);
    if (def->punit && def->punit[j][0]) {
        *d++ = ' ';
        d = put(d, def->punit[j]);
    }
    *d = 0;
    return d;
}

static uint32_t log2_q8(uint32_t x)             /* log2 of x >= 1 in 1/256, linear inside each octave */
{
    uint32_t b = 31u - (uint32_t)__builtin_clz(x);
    uint32_t frac = b >= 8u ? (x >> (b - 8u)) - 256u : (x << (8u - b)) - 256u;
    return b * 256u + frac;
}

/* where parameter j stands in its range, 0..width pixels (logarithmic parameters on a log scale) */
static uint32_t param_bar(const def_t *def, uint32_t j, int32_t q16, uint32_t width)
{
    int32_t lo = def->pmin[j], hi = def->pmax[j];
    int64_t v = q16 < lo ? lo : q16 > hi ? hi : q16;
    if (hi <= lo)
        return 0;
    if (def->plog && def->plog[j] && lo > 0) {
        uint32_t a = log2_q8((uint32_t)lo), b = log2_q8((uint32_t)hi), x = log2_q8((uint32_t)v);
        return b > a ? (x - a) * width / (b - a) : 0u;
    }
    return (uint32_t)(((v - lo) * (int64_t)width) / (hi - lo));
}

/* ---- the picture of the patch: modules as coloured boxes in rows by depth (a vertical strip), cables as lines */

#define TH_X 164u                       /* the right third of the screen, below the top bar */
#define TH_Y 26u
#define TH_W 76u
#define TH_H 198u
#define LEFT_W 136u                     /* the left two thirds: text and bars from x = 12 */
static uint16_t th_buf[TH_W * TH_H] __attribute__((section(".pool")));   /* byte swapped, as the LCD takes it */

static const uint16_t TYPE_COLOR[N_TYPES] = {
    [T_FM1] = 0x07E0, [T_OSC] = 0xFD20, [T_LFO] = 0xF81F, [T_NOISE] = 0xB596, [T_CONST] = 0xBDF7, [T_SCALE] = 0x867F,
    [T_ADSR] = 0x57EA, [T_VCA] = 0xFFE0, [T_FILTER] = 0x07FF, [T_MIXER] = 0xFC10, [T_DELAY] = 0x9A7F, [T_REVERB] = 0x07F3, [T_OUTPUT] = 0xF800, [T_CHIP] = 0xFE60,
};

static void th_px(int32_t x, int32_t y, uint16_t c)
{
    if (x >= 0 && y >= 0 && x < (int32_t)TH_W && y < (int32_t)TH_H)
        th_buf[(uint32_t)y * TH_W + (uint32_t)x] = (uint16_t)((c >> 8) | (c << 8));
}

static void th_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)
{
    int32_t i, j;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            th_px(x + i, y + j, c);
}

static void th_frame(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)
{
    int32_t i;
    for (i = 0; i < w; i++) {
        th_px(x + i, y, c);
        th_px(x + i, y + h - 1, c);
    }
    for (i = 0; i < h; i++) {
        th_px(x, y + i, c);
        th_px(x + w - 1, y + i, c);
    }
}

static void th_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t c)
{
    int32_t dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y0 - y1 : y1 - y0;     /* dy <= 0 */
    int32_t sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
        int32_t e2;
        th_px(x0, y0, c);
        if (x0 == x1 && y0 == y1)
            break;
        e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

static void th_text(int32_t x, int32_t y, const char *s, uint16_t c)   /* 5x7 text into the picture */
{
    for (; *s; s++, x += 6) {
        uint32_t ch = (uint8_t)*s, i, r;
        if (ch < 0x20u || ch > 0x5Au)
            ch = 0x20u;
        for (i = 0; i < 5u; i++)
            for (r = 0; r < 7u; r++)
                if ((FONT5X7[ch - 0x20u][i] >> r) & 1u)
                    th_px(x + (int32_t)i, y + (int32_t)r, c);
    }
}

static void thumb_draw(void)
{
    uint32_t n = (uint32_t)eng_nmodules(), nc = (uint32_t)eng_ncables(), i, c, ncols = 1, maxrows = 1, changed = 1, round;
    uint8_t layer[32], row[32], rows_in[8];
    int32_t bw, bh, px[32], py[32];
    for (i = 0; i < TH_W * TH_H; i++)
        th_buf[i] = 0;
    if (!n) {
        text5x7(TH_X + 4, TH_Y + 90, "EMPTY", 1, C_DIM, C_BG);
        return;
    }
    for (i = 0; i < n; i++)
        layer[i] = 0;
    for (round = 0; changed && round < 40u; round++) {            /* depth = longest chain of cables to the module, capped at 7 */
        changed = 0;
        for (c = 0; c < nc; c++) {
            uint32_t f, fp, t, tp;
            eng_cable(c, &f, &fp, &t, &tp);
            if (layer[f] < 7u && layer[t] < layer[f] + 1u) {
                layer[t] = (uint8_t)(layer[f] + 1u);
                changed = 1;
            }
        }
    }
    for (i = 0; i < 8u; i++)
        rows_in[i] = 0;
    for (i = 0; i < n; i++) {
        row[i] = rows_in[layer[i]]++;
        if (layer[i] + 1u > ncols)
            ncols = layer[i] + 1u;
    }
    for (i = 0; i < ncols; i++)
        if (rows_in[i] > maxrows)
            maxrows = rows_in[i];
    {   /* layers run top to bottom, the modules of a layer side by side; a 2 px margin all round */
        int32_t pitch_y = ((int32_t)TH_H - 4) / (int32_t)ncols, pitch_x = ((int32_t)TH_W - 4) / (int32_t)maxrows;
        bh = pitch_y - 8;
        bh = bh > 15 ? 15 : bh < 6 ? 6 : bh;
        bw = pitch_x - 4;
        bw = bw > 60 ? 60 : bw < 10 ? 10 : bw;
        for (i = 0; i < n; i++) {
            int32_t off = ((int32_t)maxrows - (int32_t)rows_in[layer[i]]) * pitch_x / 2;
            px[i] = 2 + off + (int32_t)row[i] * pitch_x + (pitch_x - bw) / 2;
            py[i] = 2 + (int32_t)layer[i] * pitch_y + (pitch_y - bh) / 2;
        }
    }
    for (c = 0; c < nc; c++) {                                   /* the cables first, the boxes over them */
        uint32_t f, fp, t, tp;
        eng_cable(c, &f, &fp, &t, &tp);
        if (layer[t] > layer[f])
            th_line(px[f] + bw / 2, py[f] + bh, px[t] + bw / 2, py[t] - 1, 0x6B4D);
        else
            th_line(px[f] + bw / 2, py[f] + bh / 2, px[t] + bw / 2, py[t] + bh / 2, 0xA145);   /* feedback / same row */
    }
    for (i = 0; i < n; i++) {
        int t = eng_type(i);
        uint16_t col = t >= 0 ? TYPE_COLOR[t] : C_DIM;
        th_rect(px[i], py[i], bw, bh, col);
        if (t >= 0 && bh >= 9 && bw >= 14) {                                 /* the module's name, as much as fits, black on its colour */
            char nm[8];
            uint32_t k = 0, maxc = (uint32_t)(bw - 2) / 6u;
            const char *src = DEFS[t].name;
            maxc = maxc > 7u ? 7u : maxc;
            while (src[k] && src[k] != ' ' && k < maxc) {
                nm[k] = src[k];
                k++;
            }
            nm[k] = 0;
            th_text(px[i] + (bw - (int32_t)k * 6 + 1) / 2, py[i] + (bh - 7) / 2, nm, C_BG);
        }
        if (i == sel)
            th_frame(px[i] - 2, py[i] - 2, bw + 4, bh + 4, C_FG);
    }
}

static void thumb_show(void)
{
    uint32_t y;
    thumb_draw();
    for (y = 0; y < TH_H; y += 14u) {                           /* 14 rows (6 KB) per transfer */
        uint32_t h = TH_H - y < 14u ? TH_H - y : 14u;
        lcd_blit(TH_X, TH_Y + y, TH_W, h, th_buf + y * TH_W);
        lcd_sync();
    }
}

/* ---- text parts */

/* the bar at the top: black on the accent colour, the text centred (the preset name; for a moment a message, or red for a
 * load error) */
#define BAR_H 22u
static void bar_text(const char *s, uint16_t bg)
{
    char b[28];
    uint32_t n = str_len(s), w, i;
    n = n > 26u ? 26u : n;
    for (i = 0; i < n; i++)
        b[i] = s[i];
    b[n] = 0;
    w = n * 9u;
    lcd_fill(0, 0, 240, BAR_H, bg);
    if (n)
        text_mid((240u - w) / 2u, (BAR_H - 11u) / 2u, b, C_BG, bg);
}

/* the name at the left and the CPU load at the right, black on the accent colour */
static void bar_name_cpu(const char *s, uint16_t bg)
{
    char b[24], num[12], *p = num;
    uint32_t n = str_len(s), i, w;
    n = n > 19u ? 19u : n;
    for (i = 0; i < n; i++)
        b[i] = s[i];
    b[n] = 0;
    fmt_int(num, (int32_t)cpu_pct);
    p = num + str_len(num);
    *p++ = '%';
    *p = 0;
    w = str_len(num) * 9u;
    lcd_fill(0, 0, 240, BAR_H, bg);
    text_mid(8, (BAR_H - 11u) / 2u, b, C_BG, bg);
    text_mid(232u - w, (BAR_H - 11u) / 2u, num, C_BG, bg);
}

static void header_draw(void)
{
    if (load_err) {
        char b[24], num[12], *p = b;
        p = put(p, "LOAD ERR ");
        fmt_int(num, load_err);
        put(p, num);
        p[str_len(num)] = 0;
        bar_text(b, C_RED);
    } else {
        char b[40], *p = b;
        *p++ = (char)('1' + (dsel ? DRUM_V : eng_voice()));
        *p++ = ' ';
        if (dsel) {
            p = put(p, "DRUMS ");
            p = put(p, DR_PRESETS[dr.preset].name);
        } else {
            p = put(p, patch_label ? patch_label : "");
        }
        *p = 0;
        if (cfg.cpu_bar) {
            bar_name_cpu(b, C_ACC);
        } else {
            b[24] = 0;
            bar_text(b, C_ACC);
        }
    }
    msg_until = 0;
}

static void ui_msg(const char *s, uint32_t ms)  /* a short message in the bar instead of the name */
{
    bar_text(s, C_FG);
    msg_until = fm1_ms + ms;
}

static void ui_msg_clear(void)
{
    header_draw();
}

/* where knob k of the FM-1 panel module goes: "<MODULE> <PARAMETER>" of the first module input the cable from k leads to */
static int knob_target(uint32_t panel, uint32_t k, char *out)
{
    uint32_t nc = (uint32_t)eng_ncables(), c, more = 0;
    int found = 0;
    for (c = 0; c < nc; c++) {
        uint32_t f, fp, t, tp;
        const def_t *d;
        char *p = out;
        const char *what = 0;
        eng_cable(c, &f, &fp, &t, &tp);
        if (f != panel || fp != FM1_O_K1 + k)
            continue;
        d = &DEFS[eng_type(t)];
        if (d->cvbase >= 0 && (int32_t)tp >= d->cvbase && (int32_t)tp < d->cvbase + 2) {
            uint32_t mask = eng_map(t, tp - (uint32_t)d->cvbase), j;
            for (j = 0; j < d->npar; j++)
                if (mask >> j & 1u) {
                    what = d->pname[j];
                    break;
                }
            if (!what) {
                if (found)
                    more = 1;
                continue;                               /* a parameter input that controls nothing */
            }
        } else {
            what = d->inname[tp];
        }
        if (found) {
            more = 1;
            continue;
        }
        {   /* "<MODULE> <PARAMETER>"; if that is too long for a row the last word of the module name, else the parameter alone */
            const char *mn = d->name, *last = mn, *s;
            for (s = mn; *s; s++)
                if (*s == ' ')
                    last = s + 1;
            if (str_len(mn) + 1u + str_len(what) > 16u)
                mn = str_len(last) + 1u + str_len(what) <= 16u ? last : "";
            if (*mn) {
                p = put(p, mn);
                *p++ = ' ';
            }
            p = put(p, what);
            *p = 0;
        }
        found = 1;
    }
    if (found && more) {
        uint32_t l = str_len(out);
        l = l > 15u ? 15u : l;
        out[l] = '+';
        out[l + 1u] = 0;
    }
    return found;
}

/* the selected module, laid out like the arpeggiator pages: name, then per knob a small label, the value and a bar; the
 * picture of the patch stands at the right (TH_X..) */
static void module_row(uint32_t k, const char *label, const char *val, int active, int32_t bar)
{
    char line[40], v[24], *p = line;
    uint32_t y = 56u + 36u * k;
    *p++ = 'K';
    *p++ = (char)('1' + k);
    *p++ = ' ';
    p = put(p, label);
    pad(p, line, 22);
    text5x7(12, y, line, 1, C_DIM, C_BG);
    p = put(v, val);
    pad(p, v, 15);
    text_mid(12, y + 10u, v, active ? C_FG : C_DIM, C_BG);
    if (bar >= 0) {
        uint32_t w = (uint32_t)bar > LEFT_W ? LEFT_W : (uint32_t)bar;
        lcd_fill(12, y + 24u, w, 2, C_ACC);
        lcd_fill(12 + w, y + 24u, LEFT_W - w, 2, 0x2104u);
    } else {
        lcd_fill(12, y + 24u, LEFT_W, 2, C_BG);
    }
}

static void module_draw(int full)
{
    char line[48], tgt[40], *p;
    uint32_t n = (uint32_t)eng_nmodules(), k, j;
    int type = eng_type(sel);
    const def_t *d;
    if (!n || type < 0) {
        text5x7(12, 24, "NO MODULES         ", 1, C_FG, C_BG);
        return;
    }
    d = &DEFS[type];
    if (full) {
        p = put(line, d->name);
        pad(p, line, 15);
        text_mid(12, 30, line, C_ACC, C_BG);
        thumb_show();
    }
    for (k = 0; k < 4u; k++) {
        if (type == T_FM1) {                            /* the panel: the knob's destination and a bar for its value */
            int32_t v = eng_get_param(sel, FM1_P_K1 + k);
            int has = knob_target(sel, k, tgt);
            tgt[15] = 0;
            module_row(k, "", has ? tgt : "-", has, (int32_t)(((int64_t)(v < 0 ? 0 : v > 65536 ? 65536 : v) * LEFT_W) >> 16));
        } else {
            uint32_t mask = eng_knob_mask(sel, k), first = 0, cnt = 0;
            for (j = 0; j < d->npar; j++)
                if (mask >> j & 1u) {
                    if (!cnt)
                        first = j;
                    cnt++;
                }
            if (!cnt) {
                module_row(k, "", "-", 0, -1);
            } else {
                char nm[10], val[24], *q = nm;
                q = put(q, d->pname[first]);
                *q = 0;
                if (str_len(nm) > 8u)
                    nm[8] = 0;
                if (cnt > 1u) {                         /* more than one parameter: a + after the name */
                    uint32_t l = str_len(nm);
                    l = l > 7u ? 7u : l;
                    nm[l] = '+';
                    nm[l + 1u] = 0;
                }
                int32_t eff = eng_get_eff(sel, first);          /* what the sound uses, knobs and cables included */
                if (d == &DEFS[T_DELAY] && first == DELAY_P_TIME && eng_get_eff(sel, DELAY_P_SYNC) >= 32768) {
                    char *v = put(val, DELAY_DIV_NAME[delay_div(eff)]);      /* synced: the time knob picks a note length */
                    *v = 0;
                } else
                    put_param(val, d, first, eff);
                val[15] = 0;
                module_row(k, nm, val, 1, (int32_t)param_bar(d, first, eff, LEFT_W));
            }
        }
    }
    for (k = 0; k < 2u; k++) {
        uint32_t mask = d->cvbase >= 0 ? eng_map(sel, k) : 0u;
        char *start = line;
        p = line;
        *p++ = 'P';
        *p++ = (char)('1' + k);
        *p++ = ' ';
        if (d->cvbase < 0) {
            p = put(p, "NO PARAMETER INPUTS");
        } else {
            int32_t a = (int32_t)(((int64_t)eng_att(sel, k) * 100) / 65536);
            if (a >= 0)
                *p++ = '+';
            p = put_hundredths(p, a, 2);
            for (j = 0; j < d->npar; j++)
                if (mask >> j & 1u) {
                    *p++ = ' ';
                    p = put(p, d->pname[j]);
                }
        }
        start[24] = 0;                                  /* the left two thirds only */
        pad(p < start + 24 ? p : start + 24, start, 24);
        text5x7(12, 204u + 9u * k, line, 1, d->cvbase >= 0 && mask ? C_FG : C_DIM, C_BG);
    }
    if (full)
        text5x7(12, 230, "SAVE: STORE  OCT-/OCT+: OCTAVE", 1, 0x4208u, C_BG);
}

static void arp_view_draw(uint32_t page, int full);
static void mixer_draw(int full);
static void menu_draw(int full);
static void drum_view_draw(int full);

static void view_draw(int full)                 /* whichever page is showing */
{
    if (page == PAGE_MIXER)
        mixer_draw(full);
    else if (page == PAGE_MENU)
        menu_draw(full);
    else if (dsel)
        drum_view_draw(full);
    else if (page)
        arp_view_draw(page, full);
    else
        module_draw(full);
}

static uint8_t booting = 1;                     /* the loading screen is up (main.c boot_step): the pages stay unseen */

static void screen_idle(void)                   /* redraws in place: every text pads its field, no blanking of the screen */
{
    if (booting)
        return;
    header_draw();
    view_draw(1);
}
