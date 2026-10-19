/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 morgulbrut */
/* The drum machine, voice 3: four synthesized parts (kick, snare, closed hat, open hat), a 16 step pattern for each, fixed point.
 * It is not a patch of the module engine: it has its own tiny voices and sequencer, and is mixed after eng_render.
 *
 * Threading as in arp.c: the panel (main loop) edits the dr_* settings and requests triggers (dr_req); dr_run() is called by
 * the audio ISR once per half buffer, consumes the requests, runs the clock and the steps; dr_render() adds the sound. The
 * clock is internal (BPM) unless MIDI clock ticks (dr_ext_ticks, counted by the panel) have come in during the last 190 ms.
 * Needs from engine.c: sin_turn, mul16, clampi, ENG_ONE. */

enum { DR_KICK, DR_SNARE, DR_CHAT, DR_OHAT, DR_N };
#define DR_STEPS 16u
#define DR_BLOCK 128u                   /* frames per dr_render call at most */
#define DR_TICKS_PER_STEP 6u            /* a 16th note = 6 ticks of 24 ppqn */
#define DR_TICK_NUM 110250u             /* samples per tick = DR_TICK_NUM / bpm (as in arp.c) */
#define DR_EXT_HOLD 64u                 /* blocks the internal clock waits after an external tick */
#define DR_INC_PER_HZ 97388u            /* u32 phase increment per Hz at 44.1 kHz */

static const char *const DR_NAME[DR_N] = {"KICK", "SNARE", "CLOSED HAT", "OPEN HAT"};
static const char *const DR_SHORT[DR_N] = {"KICK", "SNARE", "C HAT", "O HAT"};
/* decay time in ms at decay 0 and 100, per part */
static const uint16_t DR_TAU_LO[DR_N] = {60, 40, 10, 80};
static const uint16_t DR_TAU_HI[DR_N] = {400, 300, 120, 500};
static const int32_t DR_GAIN[DR_N] = {58000, 44000, 40000, 40000};          /* Q16, the part's full level */

typedef struct {
    const char *name;
    uint16_t pat[DR_N];                 /* bit i = step i (step 1 is bit 0) */
    uint16_t bpm;
    uint8_t decay[DR_N];
} dr_preset_t;

static const dr_preset_t DR_PRESETS[] = {
    {"FOUR FLOOR", {0x1111, 0x1010, 0x1555, 0x4000}, 124, {50, 50, 30, 50}},
    {"BREAKBEAT", {0x0481, 0x1010, 0x5555, 0x4000}, 95, {45, 55, 30, 50}},
    {"BOOM BAP", {0x0409, 0x1010, 0x5555, 0x0000}, 90, {60, 50, 30, 50}},
    {"HOUSE", {0x1111, 0x1010, 0x1515, 0x4040}, 122, {45, 45, 25, 45}},
    {"TECHNO", {0x1111, 0x1010, 0xBBBB, 0x4444}, 130, {40, 40, 20, 40}},
    {"ELECTRO", {0x0441, 0x1010, 0xAAAA, 0x4000}, 108, {50, 50, 25, 50}},
    {"HALF TIME", {0x0401, 0x0100, 0x5555, 0x4000}, 70, {55, 50, 30, 50}},
    {"TRAP", {0x0881, 0x0100, 0xD555, 0x0000}, 140, {60, 45, 20, 50}},
};
#define DR_NPRESETS ((uint32_t)(sizeof DR_PRESETS / sizeof DR_PRESETS[0]))

/* the settings the panel edits (and the state saves) */
static struct {
    uint16_t pat[DR_N];
    uint8_t level[DR_N];                /* 0..100, square law */
    uint8_t decay[DR_N];                /* 0..100 */
    uint8_t steps;                      /* pattern length 1..16 */
    uint8_t preset;                     /* the factory pattern the PRESET knob is at */
    uint16_t bpm;                       /* 40..300 */
    uint8_t part;                       /* the part the panel edits (SELECT) */
} dr;
static volatile uint8_t dr_play;                        /* panel -> ISR: the transport */
static volatile uint8_t dr_step_ui;                     /* ISR -> panel: the step last played, +1 (0 = not running) */
static volatile uint8_t dr_ui_mark;                     /* ISR -> panel: counts steps */
static volatile uint32_t dr_ext_ticks;                  /* panel: MIDI clock ticks received */
static volatile int32_t dr_gain = ENG_ONE;              /* the mixer */
static volatile uint8_t dr_req[DR_N], dr_vel[DR_N];     /* panel -> ISR: a trigger request is dr_req != the ISR's count */

typedef struct {
    int32_t env, penv, g;               /* envelopes Q30, the part's gain Q16 */
    int32_t x30;                        /* decay per sample, Q30 (env -= env * x30) */
    int32_t env2;                       /* snare: the tone's envelope */
    uint32_t ph;
    int32_t p1, p2;                     /* noise high pass memory */
} dr_voice_t;

static dr_voice_t DV[DR_N];
static struct {
    uint8_t seen[DR_N], running, ppq, step, hold;
    uint32_t acc, ext_seen, seed;
} DR = {{0}, 0, 0, 0, 0, 0, 0, 0x2545F491u};

#define DR_KICK_BASE (52u * DR_INC_PER_HZ)
#define DR_KICK_SWEEP ((int32_t)(150u * DR_INC_PER_HZ))
#define DR_KICK_PITCH_X30 811000        /* the pitch sweep decays over 30 ms */
#define DR_SNARE_TONE (190u * DR_INC_PER_HZ)
#define DR_SNARE_TONE_X30 348000        /* 70 ms */
#define DR_CHOKE_X30 24000000           /* about 1 ms: a closed hat ends the open one */

static void dr_set_decay(uint32_t p, dr_voice_t *v)
{
    uint32_t tau_ms = DR_TAU_LO[p] + (uint32_t)(DR_TAU_HI[p] - DR_TAU_LO[p]) * dr.decay[p] / 100u;
    v->x30 = (int32_t)(0x40000000u / (tau_ms * 441u / 10u));
}

static void dr_trigger(uint32_t p, uint32_t vel)        /* ISR side; vel 1..127 */
{
    dr_voice_t *v = &DV[p];
    uint32_t lv = dr.level[p];
    v->g = (int32_t)(((int64_t)DR_GAIN[p] * (lv * lv) / 10000) * (int32_t)vel / 127);
    v->env = 0x40000000;
    v->env2 = 0x40000000;
    v->penv = 0x40000000;
    v->ph = 0;
    dr_set_decay(p, v);
    if (p == DR_CHAT)
        DV[DR_OHAT].x30 = DR_CHOKE_X30;
}

static void dr_defaults(void)
{
    uint32_t p;
    dr.steps = 16;
    dr.part = 0;
    dr.preset = 0;
    for (p = 0; p < DR_N; p++) {
        dr.pat[p] = DR_PRESETS[0].pat[p];
        dr.level[p] = 80;
        dr.decay[p] = DR_PRESETS[0].decay[p];
    }
    dr.bpm = DR_PRESETS[0].bpm;
    dr_play = 0;
}

static void dr_load_preset(uint32_t n)                  /* panel side */
{
    uint32_t p;
    n %= DR_NPRESETS;
    dr.preset = (uint8_t)n;
    for (p = 0; p < DR_N; p++) {
        dr.pat[p] = DR_PRESETS[n].pat[p];
        dr.decay[p] = DR_PRESETS[n].decay[p];
    }
    dr.bpm = DR_PRESETS[n].bpm;
}

static void dr_fix(void)                                /* keep the settings in range */
{
    uint32_t p;
    for (p = 0; p < DR_N; p++) {
        dr.level[p] = dr.level[p] > 100u ? 100u : dr.level[p];
        dr.decay[p] = dr.decay[p] > 100u ? 100u : dr.decay[p];
    }
    dr.steps = dr.steps < 1u ? 1u : dr.steps > DR_STEPS ? (uint8_t)DR_STEPS : dr.steps;
    dr.bpm = dr.bpm < 40u ? 40u : dr.bpm > 300u ? 300u : dr.bpm;
    dr.preset = dr.preset >= DR_NPRESETS ? 0u : dr.preset;
    dr.part = dr.part >= DR_N ? 0u : dr.part;
}

static void dr_hit(uint32_t p, uint32_t vel)            /* panel side: play a part now (a key, MIDI, a step being set) */
{
    if (p >= DR_N)
        return;
    dr_vel[p] = (uint8_t)(vel < 1u ? 1u : vel > 127u ? 127u : vel);
    dr_req[p]++;
}

/* GM drum map -> part (36 / 35 kick, 38 / 40 snare, 42 / 44 closed hat, 46 open hat), else none */
static int dr_midi_part(uint32_t note)
{
    switch (note) {
    case 35: case 36: return DR_KICK;
    case 38: case 40: return DR_SNARE;
    case 42: case 44: return DR_CHAT;
    case 46: return DR_OHAT;
    }
    return -1;
}

/* ---- ISR side */

static void dr_step(void)
{
    uint32_t p, s = DR.step >= dr.steps ? 0u : DR.step;
    for (p = 0; p < DR_N; p++)
        if (dr.pat[p] >> s & 1u)
            dr_trigger(p, 127u);
    dr_step_ui = (uint8_t)(s + 1u);
    dr_ui_mark++;
    DR.step = (uint8_t)(s + 1u >= dr.steps ? 0u : s + 1u);
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
        return;
    }
    if (!DR.running) {                                  /* start: step 1 comes with the first tick */
        DR.running = 1;
        DR.ppq = (uint8_t)(DR_TICKS_PER_STEP - 1u);
        DR.step = 0;
        DR.acc = DR_TICK_NUM - 1u;
    }
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
    *e -= (int32_t)(((int64_t)*e * x30) >> 30);
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
        if (v->env < 16384)
            continue;
        live = 1;
        for (i = 0; i < n; i++) {
            int32_t s;
            if (p == DR_KICK) {
                uint32_t inc = DR_KICK_BASE + (uint32_t)(((int64_t)v->penv * DR_KICK_SWEEP) >> 30);
                v->ph += inc;
                s = (int32_t)(((int64_t)sin_turn(v->ph) * v->env) >> 30);
                dr_decay(&v->penv, DR_KICK_PITCH_X30);
            } else if (p == DR_SNARE) {
                int32_t nz = dr_noise(), hp = nz - v->p1;
                v->p1 = nz;
                v->ph += DR_SNARE_TONE;
                s = (int32_t)(((int64_t)sin_turn(v->ph) * v->env2) >> 31);
                s += (int32_t)(((int64_t)hp * v->env) >> 31);
                dr_decay(&v->env2, DR_SNARE_TONE_X30);
            } else {                                    /* hats: the second difference of white noise */
                int32_t nz = dr_noise(), y1 = nz - v->p1, y2 = y1 - v->p2;
                v->p1 = nz;
                v->p2 = y1;
                s = (int32_t)(((int64_t)y2 * v->env) >> 32);
            }
            acc[i] += mul16(s, v->g);
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
