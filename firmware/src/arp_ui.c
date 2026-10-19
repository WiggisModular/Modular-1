/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 morgulbrut */
/* The arpeggiator pages of the LCD (ARP, SEQ and LFO buttons): what K1..K4 change, as big values. Included after ui.c
 * (put, pad, fmt_int, colours) and arp.c (the settings).
 *
 *   MELODY (ARP)   K1 arp mode   K2 scale (with its note count)   K3 root note   K4 speed (BPM)
 *   RHYTHM (SEQ)   K1 steps      K2 pulses  K3 rotation    K4 rate        + the Euclid pattern as dots
 *   CHAOS  (LFO)   K1 melody chaos   K2 velocity chaos   K3 rhythm chaos   K4 octaves */

#define AU_TOP 24u                      /* below the header */
#define AU_ROW0 56u
#define AU_ROWH 38u
#define DOT_Y0 206u
#define DOT_PITCH 13u

static const char *const AU_TITLE[4] = {"", "MELODY", "RHYTHM", "CHAOS"};
static const char *const AU_LABEL[4][4] = {
    {"", "", "", ""},
    {"ARP MODE", "SCALE", "ROOT NOTE", "SPEED"},
    {"STEPS", "PULSES", "ROTATION", "RATE"},
    {"MELODY CHAOS", "VELOCITY CHAOS", "RHYTHM CHAOS", "OCTAVES"},
};

static char *put_note(char *d, uint32_t note)           /* C4, C#4: 60 = C4 */
{
    static const char *const N[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    char num[8];
    d = put(d, N[note % 12u]);
    fmt_int(num, (int32_t)(note / 12u) - 1);
    return put(d, num);
}

static void au_value(uint32_t page, uint32_t k, char *out)
{
    char num[12], *p = out;
    uint32_t v = 0;
    switch (page * 4u + k) {
    case 4:  p = put(p, ARP_MODE_NAME[arp_c.mode]); break;
    case 5:                             /* the name and its number of notes: Maj Pentatonic (6) */
        p = put(p, ARP_SCALES[arp_c.scale].name);
        p = put(p, " (");
        fmt_int(num, ARP_SCALES[arp_c.scale].n);
        p = put(p, num);
        p = put(p, ")");
        break;
    case 6:  p = put_note(p, arp_c.root); break;
    case 7:  fmt_int(num, arp_c.bpm); p = put(p, num); p = put(p, " BPM"); break;
    case 8:  v = arp_c.steps; break;
    case 9:  v = arp_c.pulses; break;
    case 10: v = arp_c.rot; break;
    case 11: p = put(p, ARP_RATE_NAME[arp_c.rate]); break;
    case 12: v = arp_c.chaos; break;
    case 13: v = arp_c.vchaos; break;
    case 14: v = arp_c.rchaos; break;
    case 15: v = arp_c.octaves; break;
    }
    if (page == 2u && k < 3u) {
        fmt_int(num, (int32_t)v);
        p = put(p, num);
    } else if (page == 3u) {
        fmt_int(num, (int32_t)v);
        p = put(p, num);
        if (k < 3u)
            p = put(p, "%");
    }
    *p = 0;
}

/* where each knob stands in its range, 0..216 pixels (the bar under the value) */
static uint32_t au_bar(uint32_t page, uint32_t k)
{
    uint32_t v, max;
    switch (page * 4u + k) {
    case 4:  v = arp_c.mode; max = ARP_NMODES - 1u; break;
    case 5:  v = arp_c.scale; max = ARP_NSCALES - 1u; break;
    case 6:  v = arp_c.root; max = 100u; break;
    case 7:  v = arp_c.bpm - 40u; max = 260u; break;
    case 8:  v = arp_c.steps - 1u; max = ARP_MAX_STEPS - 1u; break;
    case 9:  v = arp_c.pulses; max = arp_c.steps; break;
    case 10: v = arp_c.rot; max = arp_c.steps > 1u ? arp_c.steps - 1u : 1u; break;
    case 11: v = arp_c.rate; max = ARP_NRATES - 1u; break;
    case 12: v = arp_c.chaos; max = 100u; break;
    case 13: v = arp_c.vchaos; max = 100u; break;
    case 14: v = arp_c.rchaos; max = 100u; break;
    default: v = arp_c.octaves - 1u; max = 2u; break;
    }
    return max ? v * 216u / max : 0u;
}

static void au_row(uint32_t page, uint32_t k)
{
    char line[40], val[24], *p = line;
    uint32_t y = AU_ROW0 + AU_ROWH * k, w = au_bar(page, k);
    *p++ = 'K';
    *p++ = (char)('1' + k);
    *p++ = ' ';
    p = put(p, AU_LABEL[page][k]);
    pad(p, line, 36);
    text5x7(12, y, line, 1, C_DIM, C_BG);
    au_value(page, k, val);
    p = val + str_len(val);
    pad(p, val, 24);
    text_mid(12, y + 10u, val, C_FG, C_BG);
    lcd_fill(12, y + 24u, w, 2, C_ACC);
    lcd_fill(12 + w, y + 24u, 216u - w, 2, 0x2104u);
}

/* The Euclid pattern as dots, one per step, rotated as it plays: ring position i is the pattern's step (i + rotation).
 * `cur` is the playing position (0-based) or -1. */
static void au_dot(uint32_t i, int cur)
{
    uint32_t x = 12u + (i % 16u) * DOT_PITCH, y = DOT_Y0 + (i / 16u) * DOT_PITCH;
    int open = (int)euclid_gate(arp_c.steps, arp_c.pulses, (i + arp_c.rot) % arp_c.steps);
    lcd_fill(x, y, 10, 10, C_BG);
    if (cur)
        lcd_fill(x, y, 10, 10, open ? C_FG : C_DIM);
    else if (open)
        lcd_fill(x + 1u, y + 1u, 8, 8, C_ACC);
    else
        lcd_fill(x + 3u, y + 3u, 4, 4, 0x4208u);
}

static uint32_t au_dots_shown;                  /* steps drawn last (the pattern changed size) */
static int au_dot_cur = -1;

static void au_dots(void)
{
    uint32_t i, n = arp_c.steps;
    if (au_dots_shown > n)
        lcd_fill(12, DOT_Y0, 216, 2u * DOT_PITCH, C_BG);
    for (i = 0; i < n; i++)
        au_dot(i, (int)i == au_dot_cur);
    au_dots_shown = n;
}

/* the playing step moved: only the two dots that change */
static void au_step_poll(uint32_t page)
{
    static uint8_t seen;
    int cur = arp_step_ui ? (int)arp_step_ui - 1 : -1;
    if (page != 2u || (seen == arp_ui_mark && cur == au_dot_cur))
        return;
    seen = arp_ui_mark;
    if (cur != au_dot_cur) {
        int old = au_dot_cur;
        au_dot_cur = cur;
        if (old >= 0 && (uint32_t)old < arp_c.steps)
            au_dot((uint32_t)old, 0);
        if (cur >= 0 && (uint32_t)cur < arp_c.steps)
            au_dot((uint32_t)cur, 1);
    }
}

static void au_title(uint32_t page)
{
    text5x7(12, AU_TOP + 2u, "ARP", 1, C_DIM, C_BG);
    text_mid(12, AU_TOP + 12u, AU_TITLE[page], C_ACC, C_BG);
}

/* page 1..3; full: also the parts that only change with the page */
static void arp_view_draw(uint32_t page, int full)
{
    uint32_t k;
    au_title(page);
    for (k = 0; k < 4u; k++)
        au_row(page, k);
    if (page == 2u) {
        au_dot_cur = arp_step_ui ? (int)arp_step_ui - 1 : -1;
        if (full)
            au_dots_shown = 0;
        au_dots();
    }
    if (full)
        text5x7(12, 232, "ARP SEQ LFO: PAGE", 1, 0x4208u, C_BG);
}
