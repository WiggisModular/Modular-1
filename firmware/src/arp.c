/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 morgulbrut */
/* Arpeggiator and Euclidean rhythm, ported from penta-midi (FW/include/arp.h, euclid.h, notepool.h, scales.h).
 * It plays the engine's voice (eng_note_on / eng_note_off) from an internal 24 ppqn clock.
 *
 * Threading: the panel code (main loop) only writes the arp_c settings and sets arp_dirty / arp_play; arp_run() is
 * called by the audio ISR once per half buffer before the engine renders, copies the settings when they changed, and
 * is the only place that touches the engine, so the clock is sample-locked and nothing races the render. */

enum { ARP_UP, ARP_DOWN, ARP_INCLUDE, ARP_EXCLUDE, ARP_TURING, ARP_WALK, ARP_RANDOM, ARP_NMODES };
enum { RATE_1_1, RATE_1_2, RATE_1_4, RATE_1_4T, RATE_1_8, RATE_1_8T, RATE_1_16, RATE_1_16T, RATE_1_32, ARP_NRATES };

static const char *const ARP_MODE_NAME[ARP_NMODES] = {"UP", "DOWN", "INCLUDE", "EXCLUDE", "TURING", "WALK", "RANDOM"};
static const char *const ARP_RATE_NAME[ARP_NRATES] = {"1/1", "1/2", "1/4", "1/4T", "1/8", "1/8T", "1/16", "1/16T", "1/32"};
static const uint8_t ARP_RATE_TICKS[ARP_NRATES] = {96, 48, 24, 16, 12, 8, 6, 4, 3};   /* 24 ppqn ticks per step */

#define ARP_MAX_STEPS 32u
#define ARP_MAX_POOL 39u                /* the longest scale (chromatic, 13 notes) x 3 octaves */
#define ARP_VEL_DEFAULT 100u
#define ARP_GATE_PERCENT 65u            /* the note ends at 65 % of the step */
#define ARP_TICK_NUM 110250u            /* samples per tick = 44100 * 60 / 24 / bpm = 110250 / bpm */

typedef struct {
    const char *name;
    uint8_t n;
    uint8_t notes[13];                 /* semitones above the root; as in penta-midi the octave is part of most scales */
} arp_scale_t;

static const arp_scale_t ARP_SCALES[] = {
    {"Chromatic", 13, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}},
    {"Major", 8, {0, 2, 4, 5, 7, 9, 11, 12}},
    {"Nat Minor", 8, {0, 2, 3, 5, 7, 8, 10, 12}},
    {"Harm Minor", 8, {0, 2, 3, 5, 7, 8, 11, 12}},
    {"Mel Minor", 8, {0, 2, 3, 5, 7, 9, 11, 12}},
    {"Maj Pentatonic", 6, {0, 2, 4, 7, 9, 12}},
    {"Min Pentatonic", 6, {0, 3, 5, 7, 10, 12}},
    {"Blues", 7, {0, 3, 5, 6, 7, 10, 12}},
    {"Dorian", 8, {0, 2, 3, 5, 7, 9, 10, 12}},
    {"Phrygian", 8, {0, 1, 3, 5, 7, 8, 10, 12}},
    {"Lydian", 8, {0, 2, 4, 6, 7, 9, 11, 12}},
    {"Mixolydian", 8, {0, 2, 4, 5, 7, 9, 10, 12}},
    {"Locrian", 8, {0, 1, 3, 5, 6, 8, 10, 12}},
    {"Lydian Dom", 8, {0, 2, 4, 6, 7, 9, 10, 12}},
    {"Altered Dom", 8, {0, 1, 3, 4, 6, 8, 10, 12}},
    {"Major Bebop", 9, {0, 2, 4, 5, 7, 8, 9, 11, 12}},
    {"Minor Bebop", 9, {0, 2, 3, 4, 5, 7, 9, 10, 12}},
    {"Dominant Bebop", 9, {0, 2, 4, 5, 7, 9, 10, 11, 12}},
    {"Phrygian Dom", 8, {0, 1, 4, 5, 7, 8, 10, 12}},
    {"Whole Tone", 7, {0, 2, 4, 6, 8, 10, 12}},
    {"Dim Whole-Half", 9, {0, 2, 3, 5, 6, 8, 9, 11, 12}},
    {"Dim Half-Whole", 9, {0, 1, 3, 4, 6, 7, 9, 10, 12}},
    {"Augmented", 7, {0, 3, 4, 7, 8, 11, 12}},
    {"Dbl Harm Major", 8, {0, 1, 4, 5, 7, 8, 11, 12}},
    {"Hungarian Min", 8, {0, 2, 3, 6, 7, 8, 11, 12}},
    {"Pelog", 6, {0, 1, 3, 7, 8, 12}},
    {"Maqam Nirikriz", 8, {0, 2, 3, 6, 7, 9, 10, 12}},
    {"Maqam Saba", 8, {0, 1, 3, 4, 7, 8, 10, 12}},
    {"Hirajoshi", 6, {0, 2, 3, 7, 8, 12}},
    {"In Sen", 6, {0, 1, 5, 7, 10, 12}},
    {"Iwato", 6, {0, 1, 5, 6, 10, 12}},
    {"Yo Scale", 6, {0, 2, 5, 7, 9, 12}},
    {"Ryukyu", 6, {0, 4, 5, 7, 11, 12}},
    {"Chinese Shang", 6, {0, 2, 5, 7, 10, 12}},
    {"Todi Thaat", 8, {0, 1, 3, 6, 7, 8, 11, 12}},
    {"Purvi Thaat", 8, {0, 1, 4, 6, 7, 8, 11, 12}},
    {"Marva Thaat", 8, {0, 1, 4, 6, 7, 9, 11, 12}},
    {"Ambassel", 6, {0, 1, 5, 7, 8, 12}},
    {"Harmonic Major", 8, {0, 2, 4, 5, 7, 8, 11, 12}},
    {"Lydian Aug", 8, {0, 2, 4, 6, 8, 9, 11, 12}},
    {"Major Triad", 3, {0, 4, 7}},
    {"Minor Triad", 3, {0, 3, 7}},
    {"Dim Triad", 3, {0, 3, 6}},
    {"Aug Triad", 3, {0, 4, 8}},
    {"Power Chord", 3, {0, 7, 12}},
    {"Sus2", 3, {0, 2, 7}},
    {"Sus4", 3, {0, 5, 7}},
    {"Major 7th", 4, {0, 4, 7, 11}},
    {"Dominant 7th", 4, {0, 4, 7, 10}},
    {"Minor 7th", 4, {0, 3, 7, 10}},
    {"MinMaj 7th", 4, {0, 3, 7, 11}},
    {"Half-Dim 7th", 4, {0, 3, 6, 10}},
    {"Dim 7th", 4, {0, 3, 6, 9}},
    {"Aug 7th", 4, {0, 4, 8, 10}},
    {"AugMaj 7th", 4, {0, 4, 8, 11}},
    {"Major 6th", 4, {0, 4, 7, 9}},
    {"Minor 6th", 4, {0, 3, 7, 9}},
    {"Add9", 4, {0, 4, 7, 14}},
    {"Minor Add9", 4, {0, 3, 7, 14}},
    {"Major 9th", 5, {0, 4, 7, 11, 14}},
    {"Dominant 9th", 5, {0, 4, 7, 10, 14}},
    {"Minor 9th", 5, {0, 3, 7, 10, 14}},
    {"Dominant 11th", 6, {0, 4, 7, 10, 14, 17}},
    {"Dominant 13th", 7, {0, 4, 7, 10, 14, 17, 21}},
    {"7#9", 5, {0, 4, 7, 10, 15}},
    {"7b9", 5, {0, 4, 7, 10, 13}},
    {"7#11", 6, {0, 4, 7, 10, 14, 18}},
};
#define ARP_NSCALES ((uint32_t)(sizeof ARP_SCALES / sizeof ARP_SCALES[0]))

/* The settings the panel edits. Ranges as in penta-midi's encoder table; the LFO page's rchaos is new here. */
typedef struct {
    uint8_t mode;                       /* ARP_UP .. ARP_RANDOM */
    uint8_t scale;                      /* index into ARP_SCALES */
    uint8_t root;                       /* MIDI note, 0..100 (60 = C4) */
    uint8_t octaves;                    /* 1..3 */
    uint8_t chaos;                      /* % of steps whose note is replaced by a random one (the mode's own meaning for WALK / TURING) */
    uint8_t vchaos;                     /* % of notes with a random velocity */
    uint8_t rchaos;                     /* % of steps whose gate is inverted */
    uint8_t steps;                      /* Euclid 1..32 */
    uint8_t pulses;                     /* 0..steps */
    uint8_t rot;                        /* 0..steps-1 */
    uint8_t rate;                       /* ARP_RATE_* */
    uint16_t bpm;                       /* 40..300 */
} arp_cfg_t;

/* The running state of one arpeggiator (ISR side) */
typedef struct {
    arp_cfg_t c;                        /* the settings in use */
    uint8_t pool[ARP_MAX_POOL], npool;
    uint8_t running, ppq, note_on, note;
    uint32_t acc;                       /* sample clock: += frames * bpm, a tick every ARP_TICK_NUM */
    uint32_t phase;                     /* Euclid steps played since the start */
    int32_t off_in;                     /* frames until the note ends */
    int32_t idx;                        /* cursor of UP / DOWN / INCLUDE / EXCLUDE / WALK, -1 before the first step */
    int8_t dir;
    uint8_t pending_repeat, first;
    uint16_t turing;
    uint32_t rng;
} arp_run_t;

/* One arpeggiator per voice. arp_p is the one the panel edits (the selected voice's); arp_run walks both and puts it back.
 * The names below are the selected one's, so the panel code and the host tests read as before. */
typedef struct {
    arp_cfg_t cfg;                      /* panel -> ISR: the settings */
    volatile uint8_t dirty;             /* panel -> ISR: cfg changed */
    volatile uint8_t play;              /* panel -> ISR: the transport */
    volatile uint8_t step_ui;           /* ISR -> panel: ring position of the step last played, +1 (0 = not running) */
    volatile uint8_t ui_mark;           /* ISR -> panel: counts steps, so the panel notices a change at any rate */
    arp_run_t run;
} arp_t;

#define ARP_CFG_DEFAULT {ARP_UP, 5, 60, 1, 0, 0, 0, 16, 16, 0, RATE_1_16, 120}   /* Maj Pentatonic, as the original's default set */
#define ARP_N 4u                        /* one per voice (ENG_VOICES) */
static arp_t AV[ARP_N] = {{ARP_CFG_DEFAULT, 1, 0, 0, 0, {.rng = 0x2545F491u}}, {ARP_CFG_DEFAULT, 1, 0, 0, 0, {.rng = 0x6C8E9CF5u}},
                          {ARP_CFG_DEFAULT, 1, 0, 0, 0, {.rng = 0x1B873593u}}, {ARP_CFG_DEFAULT, 1, 0, 0, 0, {.rng = 0x3C6EF372u}}};
static arp_t *arp_p = AV;
#define arp_c (arp_p->cfg)
#define arp_dirty (arp_p->dirty)
#define arp_play (arp_p->play)
#define arp_step_ui (arp_p->step_ui)
#define arp_ui_mark (arp_p->ui_mark)
#define AR (arp_p->run)
#define ARP_VOICE ((uint32_t)(arp_p - AV))

/* Voices 2..4 play voice 1's scale */
static void arp_sync_scale(void)
{
    uint32_t v;
    for (v = 1; v < ARP_N; v++) {
        AV[v].cfg.scale = AV[0].cfg.scale;
        AV[v].dirty = 1;
    }
}

static uint32_t euclid_gate(uint32_t steps, uint32_t pulses, uint32_t i)    /* floor((i+1)p/n) - floor(ip/n) */
{
    return ((i + 1u) * pulses / steps) - (i * pulses / steps) ? 1u : 0u;
}

/* keep the dependent settings in range after one of them changed (panel side) */
static void arp_fix(void)
{
    arp_c.steps = arp_c.steps < 1u ? 1u : arp_c.steps > ARP_MAX_STEPS ? (uint8_t)ARP_MAX_STEPS : arp_c.steps;
    if (arp_c.pulses > arp_c.steps)
        arp_c.pulses = arp_c.steps;
    if (arp_c.rot >= arp_c.steps)
        arp_c.rot = (uint8_t)(arp_c.steps - 1u);
    arp_c.octaves = arp_c.octaves < 1u ? 1u : arp_c.octaves > 3u ? 3u : arp_c.octaves;
    arp_c.mode = arp_c.mode >= ARP_NMODES ? (uint8_t)(ARP_NMODES - 1u) : arp_c.mode;
    arp_c.scale = arp_c.scale >= ARP_NSCALES ? (uint8_t)(ARP_NSCALES - 1u) : arp_c.scale;
    arp_c.rate = arp_c.rate >= ARP_NRATES ? (uint8_t)(ARP_NRATES - 1u) : arp_c.rate;
    arp_c.root = arp_c.root > 100u ? 100u : arp_c.root;
    arp_c.chaos = arp_c.chaos > 100u ? 100u : arp_c.chaos;
    arp_c.vchaos = arp_c.vchaos > 100u ? 100u : arp_c.vchaos;
    arp_c.rchaos = arp_c.rchaos > 100u ? 100u : arp_c.rchaos;
    arp_c.bpm = arp_c.bpm < 40u ? 40u : arp_c.bpm > 300u ? 300u : arp_c.bpm;
}

/* ---- ISR side */



static uint32_t arp_rnd(uint32_t n)     /* 0..n-1 (xorshift32) */
{
    AR.rng ^= AR.rng << 13;
    AR.rng ^= AR.rng >> 17;
    AR.rng ^= AR.rng << 5;
    return AR.rng % n;
}

static void arp_cursor_reset(void)
{
    AR.idx = -1;
    AR.dir = 1;
    AR.pending_repeat = 0;
    AR.first = 1;
}

static void arp_pool_build(void)
{
    const arp_scale_t *s = &ARP_SCALES[AR.c.scale];
    uint32_t oct, i, n = 0;
    for (oct = 0; oct < AR.c.octaves; oct++)
        for (i = 0; i < s->n && n < ARP_MAX_POOL; i++) {
            uint32_t note = (uint32_t)AR.c.root + s->notes[i] + oct * 12u;
            AR.pool[n++] = (uint8_t)(note > 127u ? 127u : note);
        }
    AR.npool = (uint8_t)n;
    if (AR.idx > (int32_t)n - 1)
        AR.idx = (int32_t)n - 1;
}

static void arp_note_off(void)
{
    if (AR.note_on) {
        eng_note_off_v(ARP_VOICE, AR.note);
        AR.note_on = 0;
    }
}

static void arp_apply(void)             /* the panel's settings, at a block boundary */
{
    arp_cfg_t n = arp_c;
    if (ARP_VOICE)
        n.scale = AV[0].cfg.scale;      /* the other voices follow voice 1's scale */
    uint8_t pool_changed = n.scale != AR.c.scale || n.root != AR.c.root || n.octaves != AR.c.octaves;
    if (n.mode != AR.c.mode)
        AR.c.mode = n.mode, arp_cursor_reset();
    if (n.rate != AR.c.rate)
        AR.ppq = 0;                     /* a rate change takes effect on the next tick */
    AR.c = n;
    if (pool_changed || !AR.npool)
        arp_pool_build();
}

static uint32_t arp_next_index(void)
{
    uint32_t size = AR.npool;
    uint32_t chaos = AR.c.chaos;
    int32_t next;
    switch (AR.c.mode) {
    case ARP_UP:
        AR.idx = AR.idx < 0 ? 0 : (AR.idx + 1) % (int32_t)size;
        break;
    case ARP_DOWN:
        AR.idx = AR.idx < 0 ? (int32_t)size - 1 : (AR.idx - 1 + (int32_t)size) % (int32_t)size;
        break;
    case ARP_INCLUDE:                   /* up and down, the end notes played twice */
        if (AR.first) {
            AR.first = 0;
            if (AR.idx < 0)
                AR.idx = 0;
        } else if (AR.pending_repeat) {
            AR.pending_repeat = 0;
            AR.dir = (int8_t)-AR.dir;
        } else {
            AR.idx += AR.dir;
            AR.idx = AR.idx < 0 ? 0 : AR.idx > (int32_t)size - 1 ? (int32_t)size - 1 : AR.idx;
            if (AR.idx == 0 || AR.idx == (int32_t)size - 1)
                AR.pending_repeat = 1;
        }
        break;
    case ARP_EXCLUDE:                   /* up and down, the end notes played once */
        next = AR.idx + AR.dir;
        if (next >= (int32_t)size) {
            next = (int32_t)size - 2;
            AR.dir = -1;
        } else if (next < 0) {
            next = 1;
            AR.dir = 1;
        }
        AR.idx = next;
        break;
    case ARP_WALK: {                    /* a random step of one note, sometimes two or three; bounces off the ends */
        int32_t dir = arp_rnd(2) == 0 ? -1 : 1;
        int32_t mag = arp_rnd(100) < chaos / 2u ? (int32_t)(2u + arp_rnd(2)) : 1;
        next = AR.idx < 0 ? 0 : AR.idx + dir * mag;
        if (next < 0)
            next = -next;
        if (next >= (int32_t)size)
            next = 2 * ((int32_t)size - 1) - next;
        AR.idx = next < 0 ? 0 : next > (int32_t)size - 1 ? (int32_t)size - 1 : next;
        return (uint32_t)AR.idx;
    }
    case ARP_TURING: {                  /* a 16 bit shift register, the top 5 bits pick the note; chaos mutates the fed back bit */
        uint32_t wrap = (AR.turing >> 15) & 1u, fb = wrap, top;
        if (arp_rnd(100) < chaos)
            fb = arp_rnd(2);
        AR.turing = (uint16_t)((AR.turing << 1) | fb);
        top = AR.turing >> 11;
        AR.idx = (int32_t)(top * (size - 1u) / 31u);
        return (uint32_t)AR.idx;
    }
    default:
        return arp_rnd(size);
    }
    if (chaos && arp_rnd(100) < chaos)  /* chaos: this step's note only; the cursor keeps its course */
        return arp_rnd(size);
    return (uint32_t)AR.idx;
}

static void arp_step(void)
{
    uint32_t idx = arp_next_index(), steps = AR.c.steps, gate, step_frames;
    uint8_t note = AR.pool[idx];
    gate = euclid_gate(steps, AR.c.pulses, (AR.phase + AR.c.rot) % steps);
    if (AR.c.rchaos && arp_rnd(100) < AR.c.rchaos)
        gate ^= 1u;
    arp_ui_mark++;
    arp_step_ui = (uint8_t)(AR.phase % steps + 1u);
    AR.phase++;
    if (!gate)
        return;
    arp_note_off();                     /* never two notes at once */
    {
        uint32_t v = ARP_VEL_DEFAULT;
        if (AR.c.vchaos && arp_rnd(100) < AR.c.vchaos)
            v = 1u + arp_rnd(127);
        eng_note_on_v(ARP_VOICE, note, (int32_t)(v * 65536u / 127u));
    }
    AR.note = note;
    AR.note_on = 1;
    step_frames = (uint32_t)ARP_RATE_TICKS[AR.c.rate] * ARP_TICK_NUM / AR.c.bpm;
    AR.off_in = (int32_t)(step_frames * ARP_GATE_PERCENT / 100u);
}

/* One audio block of n frames: the transport, the note end, the clock. Called by the audio ISR before eng_render. */
static void arp_run_one(uint32_t n)
{
    if (arp_dirty) {
        arp_dirty = 0;
        arp_apply();
    }
    if (!arp_play) {
        if (AR.running) {
            AR.running = 0;
            arp_note_off();
            arp_step_ui = 0;
        }
        return;
    }
    if (!AR.running) {                  /* start: the first step comes with the first block */
        arp_apply();
        AR.running = 1;
        AR.ppq = (uint8_t)(ARP_RATE_TICKS[AR.c.rate] - 1u);
        AR.acc = ARP_TICK_NUM - 1u;
        AR.phase = 0;
        AR.rng ^= fm1_ticks();          /* a different random run each time */
        AR.rng = AR.rng ? AR.rng : 0x2545F491u;
        AR.turing = (uint16_t)(AR.rng >> 7) | 1u;
        arp_cursor_reset();
        if (!AR.npool)
            arp_pool_build();
    }
    if (AR.note_on) {
        AR.off_in -= (int32_t)n;
        if (AR.off_in <= 0)
            arp_note_off();
    }
    AR.acc += n * AR.c.bpm;
    while (AR.acc >= ARP_TICK_NUM) {
        AR.acc -= ARP_TICK_NUM;
        {                               /* the clock output follows the lowest voice whose arpeggiator runs */
            uint32_t lower = 0, i;
            for (i = 0; i < ARP_VOICE; i++)
                lower |= AV[i].play;
            if (!lower)
                eng_clock_tick();
        }
        if (++AR.ppq >= ARP_RATE_TICKS[AR.c.rate]) {
            AR.ppq = 0;
            arp_step();
        }
    }
}

static void arp_run(uint32_t n)         /* every voice's arpeggiator */
{
    arp_t *keep = arp_p;
    uint32_t v;
    for (v = 0; v < ARP_N; v++) {
        arp_p = &AV[v];
        arp_run_one(n);
    }
    arp_p = keep;
}

/* ---- panel side: knob k (0..3) of page 1 (melody), 2 (rhythm) or 3 (chaos), `clicks` turned; returns 1 if a value changed */
static int arp_knob(uint32_t page, uint32_t k, int32_t clicks, int fast)
{
    arp_cfg_t o = arp_c;
    int32_t c = clicks, m = fast ? 5 : 1;
    #define BUMP(field, lo, hi, mult) do { \
        int32_t v_ = (int32_t)(field) + c * (mult); \
        field = (__typeof__(field))(v_ < (lo) ? (lo) : v_ > (hi) ? (hi) : v_); } while (0)
    if (page == 1u) {
        if (k == 0u) BUMP(arp_c.mode, 0, (int32_t)ARP_NMODES - 1, 1);
        else if (k == 1u) BUMP(arp_c.scale, 0, (int32_t)ARP_NSCALES - 1, fast ? 3 : 1);
        else if (k == 2u) BUMP(arp_c.root, 0, 100, fast ? 3 : 1);
        else BUMP(arp_c.bpm, 40, 300, m);
    } else if (page == 2u) {
        if (k == 0u) BUMP(arp_c.steps, 1, (int32_t)ARP_MAX_STEPS, 1);
        else if (k == 1u) BUMP(arp_c.pulses, 0, (int32_t)arp_c.steps, 1);
        else if (k == 2u) BUMP(arp_c.rot, 0, (int32_t)arp_c.steps - 1, 1);
        else BUMP(arp_c.rate, 0, (int32_t)ARP_NRATES - 1, 1);
    } else {
        if (k == 0u) BUMP(arp_c.chaos, 0, 100, m);
        else if (k == 1u) BUMP(arp_c.vchaos, 0, 100, m);
        else if (k == 2u) BUMP(arp_c.rchaos, 0, 100, m);
        else BUMP(arp_c.octaves, 1, 3, 1);
    }
    #undef BUMP
    arp_fix();
    if (!ARP_VOICE && o.scale != arp_c.scale)
        arp_sync_scale();
    if (o.mode == arp_c.mode && o.scale == arp_c.scale && o.root == arp_c.root && o.bpm == arp_c.bpm &&
        o.steps == arp_c.steps && o.pulses == arp_c.pulses && o.rot == arp_c.rot && o.rate == arp_c.rate &&
        o.chaos == arp_c.chaos && o.vchaos == arp_c.vchaos && o.rchaos == arp_c.rchaos && o.octaves == arp_c.octaves)
        return 0;
    arp_dirty = 1;
    return 1;
}
