/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 morgulbrut */
/* The drum machine's pages (voice 3, SEL). Included after menu.c (put, pad, fmt_int, text5x7, text_mid, colours) and drums.c.
 *
 *   MATRIX (page 0): rows = the parts, columns = the 16 steps; a cursor cell (part, step) is edited with
 *     SELECT  the part (row)            K1  the step (column)
 *     K2      what the step does: OFF, ON, RATCHET 2 / 3 / 4, FLAM
 *     K3      its condition: 100 90 75 50 25 10 %, or A:B (plays in A passes of the pattern, rests for B; A, B = 1..4)
 *     K4      its volume: GHOST, NORMAL, HARD
 *     the 16 white keys   move the cursor to that step and switch it on or off (a step switched on is heard; DR_STEP_KEY)
 *     6 black keys        select + play a part (DR_PART_KEY)   PRESET  the factory pattern     PLAY  start / stop
 *   SOUND (page 1, the ARP button): K1..K4 the four controls of the selected part (tune, drive, decay and the fourth: DR_PNAME)
 *   MIX (page 2, the ARP button again; once more goes back to the matrix): K1 level of the selected part, K2 tempo (BPM), K3 pattern length
 *   On every page SELECT picks the part and the black keys select + play a part.
 */

static void leds_arp(void);                  /* main.c */

#define DU_Y0 58u                       /* the first row: the part's name, the cells 10 px below */
#define DU_ROWH 22u
#define DU_PITCH 13u
#define DU_CELL_W 11u
#define DU_CELL_H 11u
#define DU_CELL_DY 10u

static uint16_t du_shown[DR_N][DR_STEPS];           /* the cell state last drawn, +1 (0: unknown) */
static int du_cur = -1;                             /* the playhead column drawn, -1: none */

static uint16_t du_dim(uint16_t c) { return (uint16_t)((c >> 1) & 0x7BEFu); }   /* half the brightness (RGB565) */

/* One cell: the base colour by volume, the stripes of a ratchet or the two bars of a flam, a red corner when the step has a
 * condition, the playhead (white), the cursor (a cyan frame). */
static void du_cell(uint32_t p, uint32_t i)
{
    const dr_step_t *s = &dr.st[p][i];
    uint32_t x = 12u + i * DU_PITCH, y = DU_Y0 + p * DU_ROWH + DU_CELL_DY;
    uint32_t head = (int)i == du_cur, cursor = dsel && p == dr.part && i == dr.cur && !page, oor = i >= dr.steps;
    uint16_t st = (uint16_t)(s->mode | s->vel << 3 | (s->cond ? 32u : 0u) | (oor ? 64u : 0u) | (head ? 128u : 0u) | (cursor ? 256u : 0u) |
                             ((i & 3u) == 0u ? 512u : 0u));
    uint16_t c, bg;
    if (du_shown[p][i] == st + 1u)
        return;
    if (du_shown[p][i] && (du_shown[p][i] - 1u) & 256u) {   /* the cursor frame lies outside the cell: wipe it when the cursor leaves */
        lcd_fill(x - 1u, y - 1u, DU_CELL_W + 2u, 1u, C_BG);
        lcd_fill(x - 1u, y + DU_CELL_H, DU_CELL_W + 2u, 1u, C_BG);
        lcd_fill(x - 1u, y, 1u, DU_CELL_H, C_BG);
        lcd_fill(x + DU_CELL_W, y, 1u, DU_CELL_H, C_BG);
    }
    du_shown[p][i] = (uint16_t)(st + 1u);
    bg = oor ? 0x1082u : head ? C_DIM : (i & 3u) == 0u ? 0x39E7u : 0x2104u;
    lcd_fill(x, y, DU_CELL_W, DU_CELL_H, bg);
    if (s->mode) {
        c = head ? C_FG : s->vel == DV_GHOST ? du_dim(C_ACC) : C_ACC;
        if (s->mode == DM_FLAM) {                        /* a short grace bar, a gap, the main bar */
            lcd_fill(x, y, 3u, DU_CELL_H, c);
            lcd_fill(x + 5u, y, DU_CELL_W - 5u, DU_CELL_H, c);
        } else if (s->mode >= DM_R2 && s->mode <= DM_R4) {   /* N stripes */
            uint32_t n = s->mode, k, w = (DU_CELL_W - (n - 1u)) / n;
            for (k = 0; k < n; k++)
                lcd_fill(x + k * (w + 1u), y, w, DU_CELL_H, c);
        } else {
            lcd_fill(x, y, DU_CELL_W, DU_CELL_H, c);
        }
        if (s->vel == DV_HARD)
            lcd_fill(x, y, DU_CELL_W, 2u, head ? C_ACC : C_FG);     /* a bright cap: hard */
    }
    if (s->cond)
        lcd_fill(x + DU_CELL_W - 3u, y + DU_CELL_H - 3u, 3u, 3u, C_RED);
    if (cursor) {
        lcd_fill(x - 1u, y - 1u, DU_CELL_W + 2u, 1u, 0x07FFu);
        lcd_fill(x - 1u, y + DU_CELL_H, DU_CELL_W + 2u, 1u, 0x07FFu);
        lcd_fill(x - 1u, y, 1u, DU_CELL_H, 0x07FFu);
        lcd_fill(x + DU_CELL_W, y, 1u, DU_CELL_H, 0x07FFu);
    }
}

static char *du_put_cond(char *q, uint32_t c)
{
    char num[12];
    if (c < DC_PROB) {
        fmt_int(num, DC_PERCENT[c]);
        q = put(q, num);
        return put(q, "%");
    }
    fmt_int(num, (int32_t)DC_A(c));
    q = put(q, num);
    q = put(q, ":");
    fmt_int(num, (int32_t)DC_B(c));
    return put(q, num);
}

static void du_label(uint32_t p)                        /* the matrix: the part's name above its row */
{
    char line[40], *q = line;
    uint16_t col = p == dr.part ? C_ACC : C_DIM;
    q = put(q, DR_NAME[p]);
    pad(q, line, 12);
    text5x7(12, DU_Y0 + p * DU_ROWH, line, 1, col, C_BG);
}

static void du_bar(uint32_t y, uint32_t v, uint32_t h, uint16_t col)       /* a value bar over the 216 px */
{
    uint32_t w = v * 216u / 100u;
    lcd_fill(12, y, w, h, col);
    lcd_fill(12 + w, y, 216u - w, h, 0x2104u);
}

static void du_sound(void)                              /* page 1: the four controls of the selected part */
{
    char line[40], num[12], *q;
    uint32_t k;
    for (k = 0; k < DP_N; k++) {
        q = put(line, DR_PNAME[dr.part][k]);
        q = put(q, " ");
        pad(q, line, 12);
        text_mid(12, 62u + k * 34u, line, C_FG, C_BG);
        fmt_int(num, dr.pr[dr.part][k]);
        q = put(line, num);
        pad(q, line, 4);
        text_mid(12 + 12u * 9u, 62u + k * 34u, line, C_ACC, C_BG);
        du_bar(62u + k * 34u + 14u, dr.pr[dr.part][k], 8u, C_ACC);
    }
}

static void du_mix(void)                                /* page 2: the level of every part, tempo and length */
{
    char line[40], num[12], *q;
    uint32_t p;
    for (p = 0; p < DR_N; p++) {
        uint16_t col = p == dr.part ? C_ACC : C_DIM;
        q = put(line, DR_NAME[p]);
        q = put(q, " ");
        pad(q, line, 12);
        text5x7(12, DU_Y0 + p * DU_ROWH, line, 1, col, C_BG);
        fmt_int(num, dr.level[p]);
        q = put(line, num);
        pad(q, line, 4);
        text5x7(12 + 12u * 6u, DU_Y0 + p * DU_ROWH, line, 1, col, C_BG);
        du_bar(DU_Y0 + p * DU_ROWH + 10u, dr.level[p], 8u, p == dr.part ? C_ACC : du_dim(C_ACC));
    }
}

static void du_info(void)
{
    char line[40], num[12], *q = line;
    if (page == 1u) {                                   /* (the controls fill the page) */
        return;
    }
    if (page == 2u) {
        fmt_int(num, dr.bpm);
        q = put(q, num);
        q = put(q, " BPM  ");
        fmt_int(num, dr.steps);
        q = put(q, num);
        q = put(q, " STEPS");
        pad(q, line, 24);
        text_mid(12, 204, line, C_FG, C_BG);
        return;
    }
    {                                                   /* the cursor's step */
        const dr_step_t *s = &dr.st[dr.part][dr.cur];
        fmt_int(num, (int32_t)dr.cur + 1);
        q = put(q, num);
        q = put(q, " ");
        q = put(q, DR_SHORT[dr.part]);
        q = put(q, "  ");
        q = put(q, DM_NAME[s->mode]);
        pad(q, line, 24);
        text_mid(12, 196, line, C_FG, C_BG);
        q = put(line, "COND ");
        q = du_put_cond(q, s->cond);
        q = put(q, "  VOL ");
        q = put(q, DV_NAME[s->vel]);
        pad(q, line, 24);
        text_mid(12, 209, line, C_FG, C_BG);
    }
}

static void du_title(void)
{
    char line[40], *q = line;
    text5x7(12, AU_TOP + 2u, page == 2u ? "DRUMS: MIX   " : page == 1u ? "DRUMS: SOUND " : "DRUMS: MATRIX", 1, C_DIM, C_BG);
    q = put(q, DR_NAME[dr.part]);
    pad(q, line, 14);
    text_mid(12, AU_TOP + 12u, line, C_ACC, C_BG);
    text5x7(176, AU_TOP + 2u, dr_play ? "PLAYING" : "STOPPED", 1, dr_play ? C_ACC : C_DIM, C_BG);
}

static void drum_view_draw(int full)
{
    uint32_t p, i;
    if (full) {
        for (p = 0; p < DR_N; p++)
            for (i = 0; i < DR_STEPS; i++)
                du_shown[p][i] = 0;
        du_cur = dr_step_ui ? (int)dr_step_ui - 1 : -1;
        if (page == 1u) {
            text5x7(12, 221, "K1..K4: THE PART'S SOUND", 1, 0x4208u, C_BG);
            text5x7(12, 230, "SELECT: PART   ARP: MIX", 1, 0x4208u, C_BG);
        } else if (page == 2u) {
            text5x7(12, 221, "K1 LEVEL K2 BPM K3 LENGTH", 1, 0x4208u, C_BG);
            text5x7(12, 230, "SELECT: PART   ARP: BACK TO MATRIX", 1, 0x4208u, C_BG);
        } else {
            text5x7(12, 221, "K1 STEP K2 TRIG K3 COND K4 VOLUME", 1, 0x4208u, C_BG);
            text5x7(12, 230, "SELECT: PART  KEYS: STEP  ARP: SOUND", 1, 0x4208u, C_BG);
        }
    }
    du_title();
    if (page == 1u) {
        du_sound();
    } else if (page == 2u) {
        du_mix();
    } else {
        for (p = 0; p < DR_N; p++) {
            du_label(p);
            for (i = 0; i < DR_STEPS; i++)
                du_cell(p, i);
        }
    }
    du_info();
}

/* the playhead moved: only the cells of the two columns */
static void dr_step_poll(void)
{
    static uint8_t seen;
    int cur = dr_step_ui ? (int)dr_step_ui - 1 : -1;
    uint32_t p;
    if (!dsel || page > 2u || (seen == dr_ui_mark && cur == du_cur))
        return;
    seen = dr_ui_mark;
    if (cur != du_cur) {
        int old = du_cur;
        du_cur = cur;
        if (page)
            return;
        for (p = 0; p < DR_N; p++) {
            if (old >= 0)
                du_cell(p, (uint32_t)old);
            if (cur >= 0)
                du_cell(p, (uint32_t)cur);
        }
    }
}

static void dr_toggle_play(void)
{
    dr_play = !dr_play;
    leds_arp();
    if (dsel)
        ui_dirty = ui_dirty ? ui_dirty : 1;
}

static void dr_pattern_step(int32_t d)              /* the PRESET knob on the drum pages */
{
    int32_t n = (int32_t)DR_NPRESETS, i = ((int32_t)dr.preset + d) % n;
    dr_load_preset((uint32_t)(i < 0 ? i + n : i));
    screen_idle();
}

/* SELECT, K1..K4 (encoder clicks in st[0], st[2..5]) */
static void dr_knobs(const int32_t *st)
{
    static uint32_t last_ms[4];
    uint32_t k;
    int changed = 0;
    if (st[0]) {
        int32_t m = ((int32_t)dr.part + st[0]) % (int32_t)DR_N;
        dr.part = (uint8_t)(m < 0 ? m + (int32_t)DR_N : m);
        changed = 1;
    }
    for (k = 0; k < 4u; k++) {
        int32_t c = st[2u + k];
        if (!c)
            continue;
        changed = 1;
        if (page == 0u) {                               /* the matrix */
            dr_step_t *s = &dr.st[dr.part][dr.cur];
            switch (k) {
            case 0: {
                int32_t m = ((int32_t)dr.cur + c) % (int32_t)DR_STEPS;
                dr.cur = (uint8_t)(m < 0 ? m + (int32_t)DR_STEPS : m);
                break;
            }
            case 1: s->mode = (uint8_t)clampi((int32_t)s->mode + c, 0, (int32_t)DM_N - 1); break;
            case 2: s->cond = (uint8_t)clampi((int32_t)s->cond + c, 0, (int32_t)DC_N - 1); break;
            default: s->vel = (uint8_t)clampi((int32_t)s->vel + c, 0, (int32_t)DV_N - 1); break;
            }
            continue;
        }
        if (cfg.accel && fm1_ms - last_ms[k] < 80u)
            c *= 5;
        last_ms[k] = fm1_ms;
        if (page == 1u) {                               /* the part's four controls */
            dr.pr[dr.part][k] = (uint8_t)clampi((int32_t)dr.pr[dr.part][k] + c, 0, 100);
            continue;
        }
        switch (k) {
        case 0: dr.level[dr.part] = (uint8_t)clampi((int32_t)dr.level[dr.part] + c, 0, 100); break;
        case 1: dr.bpm = (uint16_t)clampi((int32_t)dr.bpm + c, 40, 300); break;
        case 2: dr.steps = (uint8_t)clampi((int32_t)dr.steps + (c > 0 ? 1 : -1), 1, (int32_t)DR_STEPS); break;
        default: break;
        }
    }
    if (changed && !ui_dirty)
        ui_dirty = 1;
}

/* the keys while the drums are selected: rising edges of the 27 key bits */
static void dr_keys(uint32_t raw, int active)
{
    static uint32_t prev;
    uint32_t rise = raw & ~prev, i;
    prev = raw;
    if (!active)
        return;
    for (i = 0; i < 16u; i++)
        if (rise >> DR_STEP_KEY[i] & 1u) {
            dr_step_t *s = &dr.st[dr.part][i];
            dr.cur = (uint8_t)i;
            s->mode = s->mode ? (uint8_t)DM_OFF : (uint8_t)DM_ON;
            if (s->mode)
                dr_hit(dr.part, DV_VEL[s->vel]);        /* a step switched on is heard */
            if (!ui_dirty)
                ui_dirty = 1;
        }
    for (i = 0; i < DR_N; i++)
        if (rise >> DR_PART_KEY[i] & 1u) {
            dr.part = (uint8_t)i;
            dr_hit(i, 127u);
            if (!ui_dirty)
                ui_dirty = 1;
        }
}
