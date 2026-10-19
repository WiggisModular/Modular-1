/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 morgulbrut */
/* The drum machine, voice 3: six synthesized parts (kick, snare, closed hat, open hat, clap, cowbell), each with four controls (tune,
 * drive, decay and a fourth: the kick's frequency delay, the noise amount of snare and hats, the clap's spread, the cowbell's tone),
 * a 16 step matrix (every step has a trigger type, a condition and a volume), fixed point.
 * It is not a patch of the module engine: it has its own tiny voices and sequencer, and is mixed after eng_render.
 *
 * Threading as in arp.c: the panel (main loop) edits the dr_* settings and requests triggers (dr_req); dr_run() is called by
 * the audio ISR once per half buffer, consumes the requests, runs the clock and the steps; dr_render() adds the sound. The
 * clock is internal (BPM) unless MIDI clock ticks (dr_ext_ticks, counted by the panel) have come in during the last 190 ms.
 * Needs from engine.c: sin_turn, mul16, clampi, ENG_ONE. */

enum { DR_KICK, DR_SNARE, DR_CHAT, DR_OHAT, DR_CLAP, DR_COW, DR_N };
enum { DP_TUNE, DP_DRIVE, DP_DECAY, DP_X, DP_N };       /* the controls of a part (dr.pr[part][..], 0..100) */
#define DR_STEPS 16u
#define FS_DR 44100u                    /* sample rate (audio.c's FS comes later in the unit) */
#define DR_BLOCK 128u                   /* frames per dr_render call at most */
#define DR_TICKS_PER_STEP 6u            /* a 16th note = 6 ticks of 24 ppqn */
#define DR_TICK_NUM 110250u             /* samples per tick = DR_TICK_NUM / bpm (as in arp.c) */
#define DR_EXT_HOLD 64u                 /* blocks the internal clock waits after an external tick */
#define DR_INC_PER_HZ 97388u            /* u32 phase increment per Hz at 44.1 kHz */

static const char *const DR_NAME[DR_N] = {"KICK", "SNARE", "CLOSED HAT", "OPEN HAT", "CLAP", "COWBELL"};
static const char *const DR_SHORT[DR_N] = {"KICK", "SNARE", "C HAT", "O HAT", "CLAP", "COWBELL"};
static const char *const DR_PNAME[DR_N][DP_N] = {
    {"TUNE", "DRIVE", "DECAY", "FREQ DELAY"}, {"TUNE", "DRIVE", "DECAY", "NOISE"}, {"TUNE", "DRIVE", "DECAY", "NOISE"},
    {"TUNE", "DRIVE", "DECAY", "NOISE"}, {"TUNE", "DRIVE", "DECAY", "SPREAD"}, {"TUNE", "DRIVE", "DECAY", "TONE"},
};
/* where the controls start (a pattern load only sets the decay); the fourth: kick 20 = a 34 ms pitch drop, snare / hats / clap / cowbell 50..70 */
static const uint8_t DR_PDEF[DR_N][DP_N] = {{40, 0, 50, 20}, {50, 0, 50, 50}, {50, 0, 30, 70}, {50, 0, 50, 70}, {50, 0, 40, 50}, {50, 0, 45, 50}};
/* decay time in ms at decay 0 and 100, per part */
static const uint16_t DR_TAU_LO[DR_N] = {60, 40, 10, 80, 30, 40};
static const uint16_t DR_TAU_HI[DR_N] = {400, 300, 120, 500, 300, 500};
static const int32_t DR_GAIN[DR_N] = {62600, 48500, 211000, 184000, 99000, 63000};    /* Q16, before the drive: each part peaks at about 0.95 at level 100 (drum_test checks) */

/* What a step does (dr.st[part][step].mode), its condition (.cond) and its volume (.vel) */
enum { DM_OFF, DM_ON, DM_R2, DM_R3, DM_R4, DM_FLAM, DM_N };
static const char *const DM_NAME[DM_N] = {"OFF", "ON", "RATCHET 2", "RATCHET 3", "RATCHET 4", "FLAM"};
enum { DV_GHOST, DV_NORMAL, DV_HARD, DV_N };
static const char *const DV_NAME[DV_N] = {"GHOST", "NORMAL", "HARD"};
static const uint8_t DV_VEL[DV_N] = {45, 110, 127};
/* conditions: 0 always, 1..5 a probability, 6.. A:B (the step plays in A passes of the pattern, then rests for B) */
#define DC_PROB 6u
#define DC_N (DC_PROB + 16u)
static const uint8_t DC_PERCENT[DC_PROB] = {100, 90, 75, 50, 25, 10};
#define DC_A(c) (((c) - DC_PROB) / 4u + 1u)
#define DC_B(c) (((c) - DC_PROB) % 4u + 1u)
#define DR_FLAM_MS 20u

typedef struct { uint8_t mode, cond, vel; } dr_step_t;

typedef struct {
    const char *name;
    uint16_t pat[DR_N];                 /* the factory patterns: bit i = step i (step 1 is bit 0), plain ON steps; DR_TWEAKS adds the rest */
    uint16_t bpm;
    uint8_t decay[4];                   /* the decay control of kick, snare and the hats (clap and cowbell: DR_PDEF) */
} dr_preset_t;

static const dr_preset_t DR_PRESETS[] = {
    {"FOUR FLOOR", {0x1111, 0x1010, 0x1555, 0x4000, 0x1010, 0x0000}, 124, {50, 50, 30, 50}},
    {"BREAKBEAT", {0x0481, 0x1010, 0x5555, 0x4000, 0x0000, 0x0420}, 95, {45, 55, 30, 50}},
    {"BOOM BAP", {0x0409, 0x1010, 0x5555, 0x0000, 0x1010, 0x0000}, 90, {60, 50, 30, 50}},
    {"HOUSE", {0x1111, 0x1010, 0x1515, 0x4040, 0x1010, 0x0000}, 122, {45, 45, 25, 45}},
    {"TECHNO", {0x1111, 0x1010, 0xBBBB, 0x4444, 0x0000, 0x0A0A}, 130, {40, 40, 20, 40}},
    {"ELECTRO", {0x0441, 0x1010, 0xAAAA, 0x4000, 0x1010, 0x0820}, 108, {50, 50, 25, 50}},
    {"HALF TIME", {0x0401, 0x0100, 0x5555, 0x4000, 0x0100, 0x0000}, 70, {55, 50, 30, 50}},
    {"TRAP", {0x0881, 0x0100, 0xD555, 0x0000, 0x0100, 0x0000}, 140, {60, 45, 20, 50}},
};
#define DR_NPRESETS ((uint32_t)(sizeof DR_PRESETS / sizeof DR_PRESETS[0]))

/* What the bitmasks cannot say: ghost notes (also steps the mask leaves empty) and ratchets, per preset. A step listed here is
 * switched on with the given mode and volume; the others stay as the mask has them. */
typedef struct { uint8_t preset, part, step, mode, vel; } dr_tweak_t;
#define TG(pr, pt, st) {pr, pt, st, DM_ON, DV_GHOST}                        /* a ghost note */
#define TR(pr, pt, st, m) {pr, pt, st, m, DV_NORMAL}                        /* a ratchet (DM_R2..DM_R4) */
#define TRG(pr, pt, st, m) {pr, pt, st, m, DV_GHOST}                        /* a soft ratchet */
static const dr_tweak_t DR_TWEAKS[] = {
    /* 0 FOUR FLOOR: ghost snares before the backbeat, the hat on 13 rolls */
    TG(0, DR_SNARE, 7), TG(0, DR_SNARE, 15), TG(0, DR_CHAT, 3), TR(0, DR_CHAT, 12, DM_R2), TRG(0, DR_CHAT, 15, DM_R2),
    /* 1 BREAKBEAT: ghost snares around the kicks, a hat roll */
    TG(1, DR_SNARE, 9), TG(1, DR_SNARE, 15), TG(1, DR_SNARE, 2), TR(1, DR_CHAT, 6, DM_R2), TRG(1, DR_CHAT, 13, DM_R2),
    /* 2 BOOM BAP: lazy ghost snares, a doubled hat */
    TG(2, DR_SNARE, 7), TG(2, DR_SNARE, 14), TG(2, DR_KICK, 8), TR(2, DR_CHAT, 14, DM_R2), TRG(2, DR_SNARE, 15, DM_R2),
    /* 3 HOUSE: ghost hats and claps, a hat roll into the bar end */
    TG(3, DR_CHAT, 3), TG(3, DR_CHAT, 11), TG(3, DR_SNARE, 15), TR(3, DR_CHAT, 12, DM_R2), TR(3, DR_CHAT, 15, DM_R3),
    /* 4 TECHNO: ghost 16ths between the hats, a snare roll */
    TG(4, DR_CHAT, 1), TG(4, DR_CHAT, 5), TG(4, DR_CHAT, 9), TG(4, DR_CHAT, 13), TR(4, DR_SNARE, 15, DM_R3), TR(4, DR_CHAT, 11, DM_R2),
    /* 5 ELECTRO: ghost snares, a kick double and a hat triplet */
    TG(5, DR_SNARE, 7), TG(5, DR_SNARE, 15), TR(5, DR_KICK, 10, DM_R2), TR(5, DR_CHAT, 9, DM_R3),
    /* 6 HALF TIME: ghost snares around the big one, a hat roll */
    TG(6, DR_SNARE, 6), TG(6, DR_SNARE, 11), TG(6, DR_SNARE, 15), TR(6, DR_CHAT, 12, DM_R2), TRG(6, DR_CHAT, 14, DM_R2),
    /* 7 TRAP: the hat rolls, a ghost snare and kick */
    TR(7, DR_CHAT, 6, DM_R3), TR(7, DR_CHAT, 10, DM_R2), TR(7, DR_CHAT, 15, DM_R4), TG(7, DR_CHAT, 14), TG(7, DR_SNARE, 13), TG(7, DR_KICK, 3),
};
#define DR_NTWEAKS ((uint32_t)(sizeof DR_TWEAKS / sizeof DR_TWEAKS[0]))

/* the settings the panel edits (and the state saves) */
static struct {
    dr_step_t st[DR_N][DR_STEPS];
    uint8_t level[DR_N];                /* 0..100, square law */
    uint8_t pr[DR_N][DP_N];             /* the controls, 0..100 */
    uint8_t steps;                      /* pattern length 1..16 */
    uint8_t preset;                     /* the factory pattern the PRESET knob is at */
    uint16_t bpm;                       /* 40..300 */
    uint8_t part;                       /* the part the panel edits (SELECT) */
    uint8_t cur;                        /* the step the matrix cursor is at (K1) */
} dr;
static volatile uint8_t dr_play;                        /* panel -> ISR: the transport */
static volatile uint8_t dr_step_ui;                     /* ISR -> panel: the step last played, +1 (0 = not running) */
static volatile uint8_t dr_ui_mark;                     /* ISR -> panel: counts steps */
static volatile uint32_t dr_ext_ticks;                  /* panel: MIDI clock ticks received */
static volatile int32_t dr_gain = ENG_ONE;              /* the mixer */
static volatile uint8_t dr_req[DR_N], dr_vel[DR_N];     /* panel -> ISR: a trigger request is dr_req != the ISR's count */
static volatile uint32_t dr_hits[DR_N];                 /* ISR: triggers so far (the host test counts them) */

typedef struct {
    int32_t env, penv, g, lg;           /* envelopes Q30; the gain before the drive (part, velocity) and after it (level), Q16 */
    int32_t x30;                        /* decay per sample, Q30 (env -= env * x30) */
    int32_t env2;                       /* snare: the tone's envelope, cowbell: the attack's */
    uint32_t ph[4];                     /* oscillator phases */
    int32_t p1, p2;                     /* filter memory */
    int32_t bm;                         /* clap: the burst's level, Q16 */
    uint32_t bc, bn;                    /* clap: samples into this burst, bursts done */
} dr_voice_t;

static dr_voice_t DV[DR_N];
static struct {
    uint8_t seen[DR_N], running, ppq, step, hold;
    uint32_t acc, ext_seen, seed;
    uint32_t loop;                      /* passes of the pattern since the start (the A:B conditions) */
    int32_t since, step_len;            /* samples since the last step, and the length of the last one (ratchets, flams) */
    uint8_t pend_n[DR_N], pend_vel[DR_N];   /* hits still to come in this step (ratchet, flam) */
    int32_t pend_in[DR_N], pend_gap[DR_N];
} DR = {.seed = 0x2545F491u};

#define DR_KICK_SWEEP ((int32_t)(150u * DR_INC_PER_HZ))
#define DR_SNARE_TONE_X30 348000        /* 70 ms */
#define DR_COW_ATTACK_X30 1620000       /* 15 ms */
#define DR_CHOKE_X30 24000000           /* about 1 ms: a closed hat ends the open one */
#define DR_COW_RATIO 379u               /* upper / lower oscillator, 8.8 fixed point (800 / 540 Hz) */
#define DR_COEF_PER_HZ 9337u            /* one-pole coefficient Q16 per kHz: 2 pi / 44100 * 65536 * 1000 */

static void dr_set_decay(uint32_t p, dr_voice_t *v)
{
    uint32_t tau_ms = DR_TAU_LO[p] + (uint32_t)(DR_TAU_HI[p] - DR_TAU_LO[p]) * dr.pr[p][DP_DECAY] / 100u;
    v->x30 = (int32_t)(0x40000000u / (tau_ms * 441u / 10u));
}

static void dr_trigger(uint32_t p, uint32_t vel)        /* ISR side; vel 1..127 */
{
    dr_voice_t *v = &DV[p];
    uint32_t lv = dr.level[p], k;
    dr_hits[p]++;
    v->g = (int32_t)((int64_t)DR_GAIN[p] * (int32_t)vel / 127);
    v->lg = (int32_t)(lv * lv * 65536u / 10000u);
    v->env = 0x40000000;
    v->env2 = 0x40000000;
    v->penv = 0x40000000;
    for (k = 0; k < 4u; k++)
        v->ph[k] = 0;
    v->bm = ENG_ONE;
    v->bc = v->bn = 0;
    if (p == DR_CLAP || p == DR_COW)                    /* (the filters of the others keep their memory: no click) */
        v->p1 = v->p2 = 0;
    dr_set_decay(p, v);
    if (p == DR_CHAT)
        DV[DR_OHAT].x30 = DR_CHOKE_X30;
}

static void dr_load_preset(uint32_t n)                  /* panel side */
{
    uint32_t p;
    n %= DR_NPRESETS;
    dr.preset = (uint8_t)n;
    for (p = 0; p < DR_N; p++) {
        uint32_t i;
        for (i = 0; i < DR_STEPS; i++) {
            dr.st[p][i].mode = DR_PRESETS[n].pat[p] >> i & 1u ? DM_ON : DM_OFF;
            dr.st[p][i].cond = 0;
            dr.st[p][i].vel = DV_NORMAL;
        }
        dr.pr[p][DP_DECAY] = p < 4u ? DR_PRESETS[n].decay[p] : DR_PDEF[p][DP_DECAY];
    }
    for (p = 0; p < DR_NTWEAKS; p++)
        if (DR_TWEAKS[p].preset == n) {
            dr_step_t *s = &dr.st[DR_TWEAKS[p].part][DR_TWEAKS[p].step];
            s->mode = DR_TWEAKS[p].mode;
            s->vel = DR_TWEAKS[p].vel;
        }
    dr.bpm = DR_PRESETS[n].bpm;
}

static void dr_defaults(void)
{
    uint32_t p;
    dr.steps = 16;
    dr.part = 0;
    dr.preset = 0;
    dr.cur = 0;
    dr_load_preset(0);
    for (p = 0; p < DR_N; p++) {
        uint32_t k;
        dr.level[p] = 90;
        for (k = 0; k < DP_N; k++)
            if (k != DP_DECAY)
                dr.pr[p][k] = DR_PDEF[p][k];
    }
    dr_play = 0;
}

static void dr_fix(void)                                /* keep the settings in range */
{
    uint32_t p;
    for (p = 0; p < DR_N; p++) {
        uint32_t k;
        dr.level[p] = dr.level[p] > 100u ? 100u : dr.level[p];
        for (k = 0; k < DP_N; k++)
            dr.pr[p][k] = dr.pr[p][k] > 100u ? 100u : dr.pr[p][k];
    }
    dr.steps = dr.steps < 1u ? 1u : dr.steps > DR_STEPS ? (uint8_t)DR_STEPS : dr.steps;
    dr.bpm = dr.bpm < 40u ? 40u : dr.bpm > 300u ? 300u : dr.bpm;
    dr.preset = dr.preset >= DR_NPRESETS ? 0u : dr.preset;
    dr.part = dr.part >= DR_N ? 0u : dr.part;
    dr.cur = dr.cur >= DR_STEPS ? 0u : dr.cur;
    for (p = 0; p < DR_N; p++) {
        uint32_t i;
        for (i = 0; i < DR_STEPS; i++) {
            dr_step_t *s = &dr.st[p][i];
            s->mode = s->mode >= DM_N ? (uint8_t)DM_OFF : s->mode;
            s->cond = s->cond >= DC_N ? 0u : s->cond;
            s->vel = s->vel >= DV_N ? (uint8_t)DV_NORMAL : s->vel;
        }
    }
}

static void dr_hit(uint32_t p, uint32_t vel)            /* panel side: play a part now (a key, MIDI, a step being set) */
{
    if (p >= DR_N)
        return;
    dr_vel[p] = (uint8_t)(vel < 1u ? 1u : vel > 127u ? 127u : vel);
    dr_req[p]++;
}

/* GM drum map -> part (36 / 35 kick, 38 / 40 snare, 42 / 44 closed hat, 46 open hat, 39 clap, 56 cowbell), else none */
static int dr_midi_part(uint32_t note)
{
    switch (note) {
    case 35: case 36: return DR_KICK;
    case 38: case 40: return DR_SNARE;
    case 42: case 44: return DR_CHAT;
    case 46: return DR_OHAT;
    case 39: return DR_CLAP;
    case 56: return DR_COW;
    }
    return -1;
}

/* ---- ISR side */

static uint32_t dr_rnd100(void)
{
    DR.seed = DR.seed * 1664525u + 1013904223u;
    return (DR.seed >> 16) % 100u;
}

static int dr_cond_ok(uint32_t c)
{
    if (c < DC_PROB)
        return c == 0u || dr_rnd100() < DC_PERCENT[c];
    return DR.loop % (DC_A(c) + DC_B(c)) < DC_A(c);
}

static void dr_step(void)
{
    uint32_t p, s = DR.step >= dr.steps ? 0u : DR.step;
    if (DR.since > 800 && DR.since < 90000)             /* the length of a step as it runs: the internal tempo or the external clock */
        DR.step_len = DR.since;
    DR.since = 0;
    for (p = 0; p < DR_N; p++) {
        const dr_step_t *st = &dr.st[p][s];
        uint32_t vel = DV_VEL[st->vel], hits;
        if (!st->mode || !dr_cond_ok(st->cond))
            continue;
        DR.pend_n[p] = 0;
        if (st->mode == DM_FLAM) {                      /* a soft grace note, the hit itself 20 ms later */
            dr_trigger(p, vel * 45u / 100u);
            DR.pend_n[p] = 1;
            DR.pend_in[p] = (int32_t)(FS_DR * DR_FLAM_MS / 1000u);
            DR.pend_gap[p] = 0;
            DR.pend_vel[p] = (uint8_t)vel;
            continue;
        }
        dr_trigger(p, vel);
        hits = st->mode >= DM_R2 && st->mode <= DM_R4 ? st->mode : 1u;      /* DM_R2..DM_R4 = 2..4 hits */
        if (hits > 1u) {                                /* a ratchet: the rest evenly over the step, a little softer each */
            DR.pend_n[p] = (uint8_t)(hits - 1u);
            DR.pend_gap[p] = DR.step_len / (int32_t)hits;
            DR.pend_in[p] = DR.pend_gap[p];
            DR.pend_vel[p] = (uint8_t)(vel * 85u / 100u);
        }
    }
    dr_step_ui = (uint8_t)(s + 1u);
    dr_ui_mark++;
    DR.step = (uint8_t)(s + 1u >= dr.steps ? 0u : s + 1u);
    if (DR.step == 0u)
        DR.loop++;
}

static void dr_pending(uint32_t n)                      /* ratchet and flam hits that fall in this block */
{
    uint32_t p;
    for (p = 0; p < DR_N; p++) {
        if (!DR.pend_n[p])
            continue;
        DR.pend_in[p] -= (int32_t)n;
        while (DR.pend_n[p] && DR.pend_in[p] <= 0) {
            dr_trigger(p, DR.pend_vel[p] ? DR.pend_vel[p] : 1u);
            DR.pend_vel[p] = (uint8_t)(DR.pend_vel[p] * 85u / 100u);
            DR.pend_n[p]--;
            DR.pend_in[p] += DR.pend_gap[p] > 0 ? DR.pend_gap[p] : 1;
        }
    }
}

static void dr_run(uint32_t n)                          /* one audio block; before dr_render */
{
    uint32_t p, ticks = 0;
    for (p = 0; p < DR_N; p++)
        if (DR.seen[p] != dr_req[p]) {
            DR.seen[p] = dr_req[p];
            dr_trigger(p, dr_vel[p]);
        }
    {
        uint32_t ext = dr_ext_ticks, d = ext - DR.ext_seen;
        if (d) {
            DR.ext_seen = ext;
            ticks = d > 8u ? 8u : d;
            DR.hold = DR_EXT_HOLD;
        }
    }
    if (!dr_play) {
        DR.running = 0;
        dr_step_ui = 0;
        for (p = 0; p < DR_N; p++)
            DR.pend_n[p] = 0;
        return;
    }
    if (!DR.running) {                                  /* start: step 1 comes with the first tick */
        DR.running = 1;
        DR.ppq = (uint8_t)(DR_TICKS_PER_STEP - 1u);
        DR.step = 0;
        DR.loop = 0;
        DR.since = 0;
        DR.step_len = (int32_t)(DR_TICK_NUM * DR_TICKS_PER_STEP / (dr.bpm ? dr.bpm : 120u));
        DR.acc = DR_TICK_NUM - 1u;
    }
    dr_pending(n);
    DR.since += (int32_t)n;
    if (DR.hold) {
        DR.hold--;
    } else {
        DR.acc += n * dr.bpm;
        while (DR.acc >= DR_TICK_NUM) {
            DR.acc -= DR_TICK_NUM;
            ticks++;
        }
    }
    while (ticks--) {
        if (++DR.ppq >= DR_TICKS_PER_STEP) {
            DR.ppq = 0;
            dr_step();
        }
    }
}

static inline int32_t dr_noise(void)                    /* white, Q16 +-1 */
{
    DR.seed = DR.seed * 1664525u + 1013904223u;
    return (int32_t)(DR.seed >> 15) - ENG_ONE;
}

static inline void dr_decay(int32_t *e, int32_t x30)
{
    int32_t d = (int32_t)(((int64_t)*e * x30) >> 30);
    *e -= d ? d : (*e > 0);                             /* never stalls above zero: a long decay still ends (the liveness test in dr_render) */
}

static inline uint32_t dr_inc(uint32_t hz100)           /* phase increment of a frequency in 1/100 Hz */
{
    return (uint32_t)((uint64_t)hz100 * DR_INC_PER_HZ / 100u);
}

static inline int32_t dr_coef(uint32_t hz)              /* one-pole coefficient Q16 for a cutoff in Hz, at most 1 */
{
    uint32_t c = hz * DR_COEF_PER_HZ / 1000u;
    return (int32_t)(c > 65535u ? 65535u : c);
}

static inline int32_t dr_sq(uint32_t ph) { return ph & 0x80000000u ? -ENG_ONE : ENG_ONE; }

/* drive: gain up to 8x into a soft clip (1.5 x - 0.5 x^3), so the loud start of a hit is crushed and the tail stays clean */
static inline int32_t dr_shape(int32_t s, uint32_t drive)
{
    int64_t x;
    int64_t x2;
    if (!drive)
        return s;
    x = (int64_t)s * (ENG_ONE + (int32_t)drive * 4588) >> 16;
    x = x > ENG_ONE ? ENG_ONE : x < -ENG_ONE ? -ENG_ONE : x;
    x2 = x * x >> 16;
    return (int32_t)((3 * x - (x2 * x >> 16)) / 2);
}

/* add n frames (<= 128) of drums to left / right, clamped */
static void dr_render(int32_t *left, int32_t *right, uint32_t n)
{
    int32_t acc[DR_BLOCK];
    uint32_t p, i, live = 0;
    int32_t gain = dr_gain;
    if (n > DR_BLOCK)
        n = DR_BLOCK;
    for (i = 0; i < n; i++)
        acc[i] = 0;
    for (p = 0; p < DR_N; p++) {
        dr_voice_t *v = &DV[p];
        const uint8_t *pr = dr.pr[p];
        uint32_t drive = pr[DP_DRIVE], x = pr[DP_X], tune = pr[DP_TUNE];
        uint32_t inc0 = 0, inc1 = 0, inc2 = 0, inc3 = 0, gap = 1, bstep = 0;
        int32_t a1 = 0, a2 = 0, wn = (int32_t)(x * 65536u / 100u), wt = ENG_ONE - wn;
        if (v->env < 16384)
            continue;
        live = 1;
        switch (p) {                                    /* what stays the same for the whole block */
        case DR_KICK:
            inc0 = dr_inc(3000u + tune * 60u);          /* 30 .. 90 Hz */
            inc1 = (uint32_t)(0x40000000u / ((5u + x * 145u / 100u) * 441u / 10u));    /* the pitch drop takes 5 .. 150 ms (Q30 decay per sample) */
            break;
        case DR_SNARE:
            inc0 = dr_inc(10000u + tune * 180u);        /* 100 .. 280 Hz */
            break;
        case DR_CHAT:
        case DR_OHAT:
            inc0 = dr_inc(30000u + tune * 400u);        /* four metallic squares, the first at 300 .. 700 Hz */
            inc1 = inc0 * 376u >> 8;
            inc2 = inc0 * 425u >> 8;
            inc3 = inc0 * 494u >> 8;
            a1 = dr_coef(2000u + tune * 60u);           /* the high pass: 2 .. 8 kHz */
            break;
        case DR_CLAP: {
            uint32_t c = 700u + tune * 20u;             /* the band's centre, 0.7 .. 2.7 kHz */
            a1 = dr_coef(2u * c);
            a2 = dr_coef(c / 3u);
            gap = (3u + x * 12u / 100u) * 441u / 10u;   /* the three bursts are 3 .. 15 ms apart */
            bstep = 45875u / gap;
            break;
        }
        default:                                        /* cowbell */
            inc0 = dr_inc(40000u + tune * 300u);        /* 400 .. 700 Hz, the second square a fifth-ish above */
            inc1 = inc0 * DR_COW_RATIO >> 8;
            a1 = dr_coef(3000u);
            break;
        }
        for (i = 0; i < n; i++) {
            int32_t s;
            if (p == DR_KICK) {
                v->ph[0] += inc0 + (uint32_t)(((int64_t)v->penv * DR_KICK_SWEEP) >> 30);
                s = (int32_t)(((int64_t)sin_turn(v->ph[0]) * v->env) >> 30);
                dr_decay(&v->penv, (int32_t)inc1);
            } else if (p == DR_SNARE) {
                int32_t nz = dr_noise(), hp = nz - v->p1;
                v->p1 = nz;
                v->ph[0] += inc0;
                s = mul16((int32_t)(((int64_t)sin_turn(v->ph[0]) * v->env2) >> 30), wt);
                s += mul16((int32_t)(((int64_t)hp * v->env) >> 30), wn);
                dr_decay(&v->env2, DR_SNARE_TONE_X30);
            } else if (p == DR_CHAT || p == DR_OHAT) {  /* noise and metallic squares, high passed */
                int32_t m, h;
                v->ph[0] += inc0;
                v->ph[1] += inc1;
                v->ph[2] += inc2;
                v->ph[3] += inc3;
                m = (dr_sq(v->ph[0]) + dr_sq(v->ph[1]) + dr_sq(v->ph[2]) + dr_sq(v->ph[3])) >> 2;
                m = mul16(dr_noise(), wn) + mul16(m, wt);
                v->p1 += (int32_t)(((int64_t)(m - v->p1) * a1) >> 16);
                h = m - v->p1;
                s = (int32_t)(((int64_t)h * v->env) >> 30);
            } else if (p == DR_CLAP) {                  /* band passed noise in three quick bursts, then the tail */
                int32_t nz = dr_noise(), bp;
                v->p1 += (int32_t)(((int64_t)(nz - v->p1) * a1) >> 16);
                v->p2 += (int32_t)(((int64_t)(v->p1 - v->p2) * a2) >> 16);
                bp = v->p1 - v->p2;
                if (v->bn < 3u) {
                    v->bm -= (int32_t)bstep;
                    if (++v->bc >= gap) {
                        v->bc = 0;
                        v->bn++;
                        v->bm = ENG_ONE;
                    }
                }
                s = (int32_t)(((int64_t)mul16(bp, v->bm) * v->env) >> 30);
            } else {                                    /* cowbell: two squares, low passed, a quick extra attack */
                int32_t m;
                v->ph[0] += inc0;
                v->ph[1] += inc1;
                m = mul16(dr_sq(v->ph[0]), wt) + mul16(dr_sq(v->ph[1]), wn);
                v->p1 += (int32_t)(((int64_t)(m - v->p1) * a1) >> 16);
                s = (int32_t)(((int64_t)v->p1 * v->env) >> 30);
                s = mul16(s, 32768 + (v->env2 >> 15));
                dr_decay(&v->env2, DR_COW_ATTACK_X30);
            }
            acc[i] += mul16(dr_shape(mul16(s, v->g), drive), v->lg);
            dr_decay(&v->env, v->x30);
        }
    }
    if (!live)
        return;
    for (i = 0; i < n; i++) {
        int32_t d = mul16(acc[i], gain);
        left[i] = clampi(left[i] + d, -ENG_ONE, ENG_ONE);
        right[i] = clampi(right[i] + d, -ENG_ONE, ENG_ONE);
    }
}
