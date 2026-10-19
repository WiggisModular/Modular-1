/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 morgulbrut */
/* The drum machine's page (voice 3, SEL): four rows of 16 step cells, the selected part's level and decay, tempo and length.
 * Included after menu.c (put, pad, fmt_int, text5x7, text_mid, colours) and drums.c.
 *
 *   SELECT   the part to edit         K1 level   K2 decay (of that part)   K3 tempo (BPM)   K4 pattern length
 *   keys 1..16 (F3 ..)  toggle that step of the part   keys 17..20  select + play a part
 *   PRESET   the factory pattern      PLAY  start / stop (the playhead runs along the cells)
 */

static void leds_arp(void);                  /* main.c */

#define DU_Y0 60u                       /* the first row */
#define DU_ROWH 34u
#define DU_PITCH 13u
#define DU_CELL_W 11u
#define DU_CELL_H 14u

static uint8_t du_shown[DR_N][DR_STEPS];            /* the cell state last drawn, +1 (0: unknown) */
static int du_cur = -1;                             /* the playhead column drawn, -1: none */

/* cell state: bit 0 on, bit 1 beyond the pattern length, bit 2 playhead, bit 3 first step of a beat */
static void du_cell(uint32_t p, uint32_t i)
{
    uint32_t st = (dr.pat[p] >> i & 1u) | (i >= dr.steps ? 2u : 0u) | ((int)i == du_cur ? 4u : 0u) | ((i & 3u) == 0u ? 8u : 0u);
    uint16_t c;
    if (du_shown[p][i] == st + 1u)
        return;
    du_shown[p][i] = (uint8_t)(st + 1u);
    if (st & 2u)
        c = 0x1082u;
    else if (st & 1u)
        c = (st & 4u) ? C_FG : C_ACC;
    else
        c = (st & 4u) ? C_DIM : (st & 8u) ? 0x39E7u : 0x2104u;
    lcd_fill(12u + i * DU_PITCH, DU_Y0 + p * DU_ROWH + 11u, DU_CELL_W, DU_CELL_H, c);
}

static void du_label(uint32_t p)
{
    char line[40], num[12], *q = line;
    uint16_t col = p == dr.part ? C_ACC : C_DIM;
    q = put(q, DR_NAME[p]);
    pad(q, line, 12);
    text5x7(12, DU_Y0 + p * DU_ROWH, line, 1, col, C_BG);
    q = put(line, "LV ");
    fmt_int(num, dr.level[p]);
    q = put(q, num);
    q = put(q, "  DEC ");
    fmt_int(num, dr.decay[p]);
    q = put(q, num);
    pad(q, line, 15);
    text5x7(124, DU_Y0 + p * DU_ROWH, line, 1, col, C_BG);
}

static void du_info(void)
{
    char line[40], num[12], *q = line;
    fmt_int(num, dr.bpm);
    q = put(q, num);
    q = put(q, " BPM  ");
    fmt_int(num, dr.steps);
    q = put(q, num);
    q = put(q, " STEPS");
    pad(q, line, 24);
    text_mid(12, 204, line, C_FG, C_BG);
}

static void du_title(void)
{
    char line[40], *q = line;
    text5x7(12, AU_TOP + 2u, "DRUMS", 1, C_DIM, C_BG);
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
        text5x7(12, 221, "KEYS 1-16: STEPS  17-20: PARTS", 1, 0x4208u, C_BG);
        text5x7(12, 230, "SELECT: PART  PRESET: PATTERN", 1, 0x4208u, C_BG);
    }
    du_title();
    for (p = 0; p < DR_N; p++) {
        du_label(p);
        for (i = 0; i < DR_STEPS; i++)
            du_cell(p, i);
    }
    du_info();
}

/* the playhead moved: only the cells of the two columns */
static void dr_step_poll(void)
{
    static uint8_t seen;
    int cur = dr_step_ui ? (int)dr_step_ui - 1 : -1;
    uint32_t p;
    if (!dsel || page || (seen == dr_ui_mark && cur == du_cur))
        return;
    seen = dr_ui_mark;
    if (cur != du_cur) {
        int old = du_cur;
        du_cur = cur;
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
    if (dsel && !page)
        ui_dirty = ui_dirty ? ui_dirty : 1;
}

static void dr_pattern_step(int32_t d)              /* the PRESET knob on the drum page */
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
        int32_t c = st[2u + k], mul = 1;
        if (!c)
            continue;
        if (cfg.accel && fm1_ms - last_ms[k] < 80u)
            mul = 5;
        last_ms[k] = fm1_ms;
        c *= mul;
        switch (k) {
        case 0: dr.level[dr.part] = (uint8_t)clampi((int32_t)dr.level[dr.part] + c, 0, 100); break;
        case 1: dr.decay[dr.part] = (uint8_t)clampi((int32_t)dr.decay[dr.part] + c, 0, 100); break;
        case 2: dr.bpm = (uint16_t)clampi((int32_t)dr.bpm + c, 40, 300); break;
        default: dr.steps = (uint8_t)clampi((int32_t)dr.steps + (c > 0 ? 1 : -1), 1, (int32_t)DR_STEPS); break;
        }
        changed = 1;
    }
    if (changed && !ui_dirty)
        ui_dirty = 1;
}

/* the keys while the drums are selected: rising edges of the 27 key bits */
static void dr_keys(uint32_t raw)
{
    static uint32_t prev;
    uint32_t rise = raw & ~prev, i;
    prev = raw;
    for (i = 0; i < 16u; i++)
        if (rise >> i & 1u) {
            dr.pat[dr.part] ^= (uint16_t)(1u << i);
            if (dr.pat[dr.part] >> i & 1u)
                dr_hit(dr.part, 127u);                  /* a step switched on is heard */
            if (!ui_dirty)
                ui_dirty = 1;
        }
    for (i = 0; i < DR_N; i++)
        if (rise >> (16u + i) & 1u) {
            dr.part = (uint8_t)i;
            dr_hit(i, 127u);
            if (!ui_dirty)
                ui_dirty = 1;
        }
}
