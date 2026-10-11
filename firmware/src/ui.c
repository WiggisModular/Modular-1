/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 morgulbrut */
/* The LCD: patch name and CPU load, the octave, a small picture of the patch, the selected module with what the four
 * knobs change (for the FM-1 panel module: where each knob goes), and the two parameter inputs.
 *
 *   y   2  MODULAR-1 and version        y  40  3/8 SVF FILTER (the selected module)
 *   y  19  patch name, octave, message  y  58  the patch picture, 220 x 56
 *   y  29  CPU                          y 120  K1..K4 rows        y 200 P1, P2 */
#define C_BG 0x0000u
#define C_FG 0xFFFFu
#define C_ACC 0xFD20u
#define C_DIM 0x7BEFu
#define C_RED 0xF800u

static uint32_t cur_preset;
static int load_err;
static const char *patch_label = "";
static uint8_t page;                            /* 0: the selected module, 1..3: the arpeggiator pages MELODY, RHYTHM, CHAOS (arp_ui.c) */
static uint32_t sel;                            /* the module the knobs act on (SELECT turns it) */
static volatile uint8_t ui_dirty;               /* 1: the knob rows need a redraw, 2: the module changed too (title, picture) */
static int32_t octave;                          /* the keys' octave shift, -3..3 (OCT- / OCT+) */
static uint32_t msg_until;                      /* a message in the header (SAVED ...) until this fm1_ms */

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
        i = i < 0 ? 0 : i >= n ? n - 1 : i;
        return put(d, names[i]);
    }
    d = put_num(d, q16);
    if (def->punit && def->punit[j][0]) {
        *d++ = ' ';
        d = put(d, def->punit[j]);
    }
    return d;
}

/* ---- the picture of the patch: modules as coloured boxes in columns by depth, cables as lines */

#define TH_X 10u
#define TH_Y 36u
#define TH_W 220u
#define TH_H 120u
static uint16_t th_buf[TH_W * TH_H] __attribute__((section(".pool")));   /* byte swapped, as the LCD takes it */

static const uint16_t TYPE_COLOR[N_TYPES] = {
    [T_FM1] = 0x07E0, [T_OSC] = 0xFD20, [T_LFO] = 0xF81F, [T_NOISE] = 0xB596, [T_CONST] = 0xBDF7, [T_SCALE] = 0x867F,
    [T_ADSR] = 0x57EA, [T_VCA] = 0xFFE0, [T_FILTER] = 0x07FF, [T_MIXER] = 0xFC10, [T_DELAY] = 0x9A7F, [T_OUTPUT] = 0xF800,
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
        text5x7(TH_X + 4, TH_Y + 54, "EMPTY PATCH", 1, C_DIM, C_BG);
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
    bw = ((int32_t)TH_W - 8) / (int32_t)ncols - 8;               /* room for the cable between the columns */
    bw = bw > 44 ? 44 : bw < 16 ? 16 : bw;
    bh = (int32_t)TH_H / (int32_t)maxrows - 3;
    bh = bh > 13 ? 13 : bh < 6 ? 6 : bh;
    {
        int32_t pitch = (int32_t)TH_H / (int32_t)maxrows;
        for (i = 0; i < n; i++) {
            int32_t off = ((int32_t)maxrows - (int32_t)rows_in[layer[i]]) * pitch / 2;
            px[i] = 4 + (ncols > 1u ? (int32_t)layer[i] * ((int32_t)TH_W - 8 - bw) / ((int32_t)ncols - 1) : ((int32_t)TH_W - bw) / 2 - 4);
            py[i] = off + (int32_t)row[i] * pitch + (pitch - bh) / 2;
        }
    }
    for (c = 0; c < nc; c++) {                                   /* the cables first, the boxes over them */
        uint32_t f, fp, t, tp;
        eng_cable(c, &f, &fp, &t, &tp);
        if (layer[t] > layer[f])
            th_line(px[f] + bw, py[f] + bh / 2, px[t] - 1, py[t] + bh / 2, 0x6B4D);
        else
            th_line(px[f] + bw / 2, py[f] + bh / 2, px[t] + bw / 2, py[t] + bh / 2, 0xA145);   /* feedback / same column */
    }
    for (i = 0; i < n; i++) {
        int t = eng_type(i);
        uint16_t col = t >= 0 ? TYPE_COLOR[t] : C_DIM;
        th_rect(px[i], py[i], bw, bh, col);
        if (t >= 0 && bh >= 9) {                                 /* the module's name, as much as fits, black on its colour */
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

static void status_draw(void)                   /* the engine's CPU load, a few times a second */
{
    char line[24], num[12], *p = line;
    p = put(p, "CPU ");
    fmt_int(num, (int32_t)(eng_us_last * 100u / 2902u));
    p = put(p, num);
    p = put(p, "% PK ");
    fmt_int(num, (int32_t)(eng_us_max * 100u / 2902u));
    p = put(p, num);
    p = put(p, "%");
    pad(p, line, 14);
    text5x7(130, 12, line, 1, C_DIM, C_BG);
}

static void header_draw(void)                   /* the patch name, the octave, a message */
{
    char b[40], num[12], *p = b;
    if (load_err) {
        p = put(p, "LOAD ERR ");
        fmt_int(num, load_err);
        p = put(p, num);
        pad(p, b, 30);
        text5x7(12, 2, b, 1, C_RED, C_BG);
    } else {
        p = put(p, patch_label);
        pad(p, b, 30);
        text5x7(12, 2, b, 1, C_FG, C_BG);
    }
    p = b;
    p = put(p, "OCT ");
    if (octave > 0)
        *p++ = '+';
    fmt_int(num, octave);
    p = put(p, num);
    pad(p, b, 6);
    text5x7(12, 12, b, 1, octave ? C_ACC : C_DIM, C_BG);
}

static void ui_msg(const char *s, uint32_t ms)  /* a short message at the right of the header */
{
    char b[10], *p = b;
    p = put(p, s);
    pad(p, b, 7);
    text5x7(66, 12, b, 1, C_ACC, C_BG);
    msg_until = fm1_ms + ms;
}

static void ui_msg_clear(void)
{
    text5x7(66, 12, "       ", 1, C_BG, C_BG);
    msg_until = 0;
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

/* the selected module: name, what the four knobs change, the two parameter inputs */
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
    p = line;
    fmt_int(p, (int32_t)sel + 1);
    p += str_len(p);
    *p++ = '/';
    fmt_int(p, (int32_t)n);
    p += str_len(p);
    *p++ = ' ';
    p = put(p, d->name);
    pad(p, line, 36);
    if (full) {
        text5x7(12, 24, line, 1, C_ACC, C_BG);
        thumb_show();
    }
    for (k = 0; k < 4u; k++) {
        uint32_t y = 160u + 12u * k;
        char *start = line;
        p = line;
        *p++ = 'K';
        *p++ = (char)('1' + k);
        *p++ = ' ';
        if (type == T_FM1) {                            /* the panel: the knob's destination and a bar for its value */
            int32_t v = eng_get_param(sel, FM1_P_K1 + k);
            uint32_t w = (uint32_t)(((int64_t)(v < 0 ? 0 : v > 65536 ? 65536 : v) * 216) >> 16);
            int has = knob_target(sel, k, tgt);
            if (has) {
                tgt[16] = 0;
                p = put(p, tgt);
            } else {
                p = put(p, "-");
            }
            pad(p, start, 36);
            text5x7(12, y, line, 1, has ? C_FG : C_DIM, C_BG);
            lcd_fill(12, y + 8u, w, 2, C_ACC);
            lcd_fill(12 + w, y + 8u, 216u - w, 2, 0x2104u);
        } else {
            uint32_t mask = eng_knob_mask(sel, k), first = 0, cnt = 0;
            for (j = 0; j < d->npar; j++)
                if (mask >> j & 1u) {
                    if (!cnt)
                        first = j;
                    cnt++;
                }
            if (!cnt) {
                p = put(p, "-");
                pad(p, start, 36);
            } else {
                char nm[10], val[14], *q = nm;
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
                p = put(p, nm);
                pad(p, start + 3, 8);                   /* names are at most 7 characters; the value column is 8 wide */
                p = start + 11;
                put_param(val, d, first, eng_get_eff(sel, first));      /* what the sound uses, knobs and cables included */
                val[8] = 0;
                p = put(p, val);
                pad(p, start, 36);
            }
            text5x7(12, y, line, 1, cnt ? C_FG : C_DIM, C_BG);
            lcd_fill(12, y + 8u, 216u, 2, C_BG);
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
        pad(p, start, 40);
        text5x7(12, 210u + 9u * k, line, 1, d->cvbase >= 0 && mask ? C_FG : C_DIM, C_BG);
    }
    if (full)
        text5x7(12, 230, "SAVE: STORE   OCT-/OCT+: OCTAVE", 1, 0x4208u, C_BG);
}

static void arp_view_draw(uint32_t page, int full);

static void view_draw(int full)                 /* whichever page is showing */
{
    if (page)
        arp_view_draw(page, full);
    else
        module_draw(full);
}

static void screen_idle(void)
{
    lcd_fill(0, 0, 240, 240, C_BG);
    text5x7(198, 2, FELUCCA_VERSION, 1, C_ACC, C_BG);
    header_draw();
    status_draw();
    view_draw(1);
}
