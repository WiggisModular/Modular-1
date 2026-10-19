/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 morgulbrut */
/* Fixed-point patch engine, see engine.h. The reference is web/src/engine.ts / modules.ts; formulas are kept
 * line by line where possible. Number formats: signals, parameters Q16.16; oscillator phase u32 (2^32 = 1 cycle);
 * filter coefficients Q24; envelope level and rates Q30. Build: -DENG_POOL=... places the big buffers (device:
 * the .pool section). Needs 64-bit multiply (hardware) and, at block rate and on waveform edges, 64/32 division. */
#include "engine.h"
#include "defs.h"
#include "dsp_tables.h"

#ifndef ENG_POOL
#define ENG_POOL
#endif

#define ENG_MAX_MODULES 24          /* per voice */
#define ENG_MAX_CABLES 128
#define ENG_NBUF 40u                /* signal buffers of ENG_BLOCK samples per voice (a connected output, a summed input) */
#define ENG_NDELAY 1                /* delay modules per patch */
#define ENG_NREV 1                  /* reverb modules per patch */
#define RV_LEN 5934u                /* the four combs (1116 1188 1277 1356) and two allpasses (556 441), int16 Q13 */
#define DELAY_LEN 23000u            /* 521 ms (the reference has 1.05 s: four voices do not fit in the pool with it); web/src/modules.ts caps the delay the same */
#define MAX_SUM (ENG_MAX_MODULES * 4)
#define MAX_HELD 16

#define INC_C4 25480119u            /* 261.6255653 Hz at ENG_SR, u32 phase per sample */
#define DT_MAX 1932735283u          /* 0.45 of a cycle per sample (the reference limits osc to 0.45, cutoff to 0.45 sr) */
#define TURN_PER_RAD 683565276u     /* 2^32 / 2 pi */

typedef int32_t sig_t;

static inline int32_t mulq(int32_t a, int32_t b, unsigned s) { return (int32_t)(((int64_t)a * b) >> s); }
static inline int32_t mul16(int32_t a, int32_t b) { return mulq(a, b, 16); }
/* rounded to nearest: no bias, which the filter's integrators would turn into a DC offset at low cutoffs */
static inline int32_t mulr(int32_t a, int32_t b, unsigned s) { return (int32_t)(((int64_t)a * b + ((int64_t)1 << (s - 1))) >> s); }
static inline int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ------------------------------------------------------------------ number helpers */

/* base * 2^(x / 65536), saturating at 2^32 - 1. base: a phase increment, x: octaves Q16.16 */
static uint32_t exp2_scale(uint32_t base, int32_t x)
{
    int32_t n = x >> 16;
    uint32_t fr = (uint32_t)x & 0xFFFFu, i = fr >> 8, r = fr & 255u;
    uint32_t m = EXP2_T[i] + (uint32_t)(((uint64_t)(EXP2_T[i + 1u] - EXP2_T[i]) * r) >> 8);   /* Q28, 1..2 */
    uint64_t v = ((uint64_t)base * m) >> 28;
    if (n >= 0) {
        if (n >= 32 || v >= (1ull << (32 - n)))
            return 0xFFFFFFFFu;
        return (uint32_t)(v << n);
    }
    return -n >= 64 ? 0u : (uint32_t)(v >> -n);
}

/* sin(2 pi phase), phase u32 turns -> Q16 */
static int32_t sin_turn(uint32_t ph)
{
    uint32_t i = ph >> 22, fr = (ph >> 6) & 0xFFFFu;
    int32_t s0 = SIN_T[i], s1 = SIN_T[i + 1u];
    return (s0 + (int32_t)(((int64_t)(s1 - s0) * fr) >> 16)) >> 14;
}

/* 1 / (d / 2^24) in Q24 for d (Q24) in [2^24, 2^30): 32-bit divide for a first estimate, one Newton step */
static int32_t recip24(int32_t d)
{
    int32_t x = (int32_t)(0xFFFFFFFFu / ((uint32_t)d >> 8)) << 8;
    int32_t t = (int32_t)(((int64_t)d * x) >> 24);
    return (int32_t)(((int64_t)x * ((2 << 24) - t)) >> 24);
}

static inline int32_t round_q16(int32_t v) { return (v + 32768) >> 16; }   /* Math.round for the integer params */

/* poly-BLEP residual, phase t and increment dt in u32 turns -> Q16 */
static int32_t poly_blep(uint32_t t, uint32_t dt)
{
    if (dt == 0u)
        return 0;
    if (t < dt) {
        int32_t u = (int32_t)(((uint64_t)t << 16) / dt);
        return 2 * u - (int32_t)(((int64_t)u * u) >> 16) - ENG_ONE;
    }
    if (t > 0u - dt) {
        int32_t u = (int32_t)(((uint64_t)(0u - t) << 16) / dt);      /* -(t - 1) / dt */
        return (int32_t)(((int64_t)u * u) >> 16) - 2 * u + ENG_ONE;
    }
    return 0;
}

/* the waveforms of modules.ts wave(): 0 sine, 1 triangle, 2 saw, 3 square. phase and pw in u32 turns */
static int32_t wave(int32_t kind, uint32_t ph, uint32_t dt, uint32_t pw)
{
    switch (kind) {
    case 0:
        return sin_turn(ph);
    case 1: {
        int32_t s = (int32_t)ph;
        uint32_t a = s < 0 ? 0u - (uint32_t)s : (uint32_t)s;     /* |phase - round(phase)| in 2^32 */
        return (int32_t)(a >> 14) - ENG_ONE;
    }
    case 2:
        return (int32_t)(ph >> 15) - ENG_ONE - poly_blep(ph, dt);
    default: {
        int32_t v = ph < pw ? ENG_ONE : -ENG_ONE;
        v += poly_blep(ph, dt);
        v -= poly_blep(ph - pw, dt);
        return v;
    }
    }
}


/* log2 of a Q16 value (or of any positive integer: the offset cancels in ratios), Q16 */
static int32_t log2_q16(uint32_t x)
{
    uint32_t n, m, i, r;
    if (!x)
        return -(32 << 16);
    n = 31u - (uint32_t)__builtin_clz(x);
    m = (x << (31u - n)) & 0x7FFFFFFFu;                  /* the mantissa's fraction, 31 bits */
    i = m >> 23;
    r = (m >> 7) & 0xFFFFu;
    return (int32_t)(n << 16) + (int32_t)(LOG2_T[i] + (uint32_t)(((uint64_t)(LOG2_T[i + 1u] - LOG2_T[i]) * r) >> 16));
}

/* 2^(e / 65536) as an integer (a Q16 value), saturating */
static uint32_t exp2_q16(int32_t e)
{
    int32_t n = e >> 16;
    uint32_t fr = (uint32_t)e & 0xFFFFu, i = fr >> 8, r = fr & 255u;
    uint64_t m = EXP2_T[i] + (uint32_t)(((uint64_t)(EXP2_T[i + 1u] - EXP2_T[i]) * r) >> 8);   /* Q28 */
    if (n < 0)
        return 0;
    if (n > 31)
        return 0x7FFFFFFFu;
    m = (m << n) >> 28;
    return m > 0x7FFFFFFFu ? 0x7FFFFFFFu : (uint32_t)m;
}

static uint32_t isqrt32(uint32_t x)
{
    uint32_t r = 0, b = 1u << 30;
    while (b > x)
        b >>= 2;
    for (; b; b >>= 2) {
        if (x >= r + b) {
            x -= r + b;
            r = (r >> 1) + b;
        } else
            r >>= 1;
    }
    return r;
}

/* a parameter's position in its range as Q16 0..1: linear, or logarithmic for the cutoff (plog 2: bent, the position is
 * the square root of the octave fraction, so the low end gets more of the knob) */
static int32_t unit_of(const def_t *d, uint32_t j, int32_t v)
{
    int32_t lo = d->pmin[j], hi = d->pmax[j];
    v = clampi(v, lo, hi);
    if (d->plog && d->plog[j]) {
        int32_t l0 = log2_q16((uint32_t)lo);
        int32_t t = (int32_t)(((int64_t)(log2_q16((uint32_t)v) - l0) << 16) / (log2_q16((uint32_t)hi) - l0));
        return d->plog[j] == 2 ? (int32_t)isqrt32((uint32_t)t << 16) : t;
    }
    return (int32_t)(((int64_t)(v - lo) << 16) / (hi - lo));
}

static int32_t value_of(const def_t *d, uint32_t j, int32_t u)
{
    int32_t lo = d->pmin[j], hi = d->pmax[j];
    u = clampi(u, 0, ENG_ONE);
    if (d->plog && d->plog[j]) {
        int32_t l0 = log2_q16((uint32_t)lo), t = d->plog[j] == 2 ? (int32_t)(((int64_t)u * u) >> 16) : u;
        return clampi((int32_t)exp2_q16(l0 + (int32_t)(((int64_t)t * (log2_q16((uint32_t)hi) - l0)) >> 16)), lo, hi);
    }
    return lo + (int32_t)(((int64_t)u * (hi - lo)) >> 16);
}

/* ------------------------------------------------------------------ engine state */

typedef struct {
    uint8_t type, conn, sum0, nsum;
    uint8_t map[6];                  /* the matrix: bit j of map[s] = source s controls parameter j. s: p1 p2 k1 k2 k3 k4 */
    int32_t att[2];                  /* attenuverters of p1, p2: Q16, -1..1 */
    int32_t par[MAX_PAR], eff[MAX_PAR];
    const sig_t *in[MAX_IN];
    sig_t *out[MAX_OUT];
    union {
        struct { uint32_t phase; } osc;
        struct { uint32_t phase, seed; int32_t held; uint32_t since; int32_t interval; uint16_t ticks; uint8_t clk_hi, rst_hi; } lfo;   /* interval: clock edge distance in samples, Q8 */
        struct { uint32_t seed; } noise;
        struct { uint32_t phase, noise_clock, arp; uint16_t lfsr; uint8_t step; } chip;
        struct { int32_t v; uint8_t stage, prev; } adsr;
        struct { int32_t ic1, ic2, jc1, jc2; } flt;   /* stage 1 and stage 2 integrators */
        struct { int16_t *buf; uint32_t w, t, seen, beat; uint32_t ring[DELAY_CLK_PPQN]; uint8_t prev; } dly;
        struct { int16_t *buf; uint16_t ci[4], ai[2]; int32_t lp[4]; } rev;
    } s;
} node_t;

typedef struct {
    sig_t *dst;
    uint16_t first, n;       /* sources in sum_src[] */
} sum_t;

typedef struct {
    uint8_t loaded, nmod, nsum, nsrc, norder, nout;
    uint8_t order[ENG_MAX_MODULES], outputs[ENG_MAX_MODULES];
    uint8_t from_m[ENG_MAX_CABLES], from_p[ENG_MAX_CABLES], to_m[ENG_MAX_CABLES], to_p[ENG_MAX_CABLES];
    uint16_t ncab, nbuf, ndly, nrev;
    node_t node[ENG_MAX_MODULES];
    sum_t sum[MAX_SUM];
    const sig_t *sum_src[ENG_MAX_CABLES];
    struct { uint8_t note; int32_t vel; } held[MAX_HELD];
    uint8_t nheld;
    struct { int32_t pitch, gate, vel; } voice;
    uint32_t clk_seen;
} eng_t;

/* the voices. E is the one the API calls act on (eng_voice_select; the panel code), the audio ISR walks all of them and puts the
 * pointer back. The clock is shared: ticks (24 ppqn) come from the arpeggiator or MIDI in (eng_clock_tick). */
static eng_t EV[ENG_VOICES] ENG_POOL;
static eng_t *eng_p = EV;
#define E (*eng_p)
static volatile uint32_t clk_ticks;

static sig_t buf_pool[ENG_VOICES][ENG_NBUF][ENG_BLOCK] ENG_POOL;
static int16_t dly_pool[ENG_VOICES][ENG_NDELAY][DELAY_LEN] ENG_POOL;
static int16_t rev_pool[ENG_VOICES][ENG_NREV][RV_LEN] ENG_POOL;
#define VOICE_NO ((uint32_t)(eng_p - EV))
static const sig_t zero_buf[ENG_BLOCK];
static sig_t dummy_out[ENG_BLOCK];

/* ------------------------------------------------------------------ patch loading */

static void visit(uint32_t id, uint8_t *st)
{
    uint32_t c;
    st[id] = 1;
    for (c = 0; c < E.ncab; c++) {
        if (E.to_m[c] != id)
            continue;
        if (st[E.from_m[c]] == 0)
            visit(E.from_m[c], st);
    }
    st[id] = 2;
    E.order[E.norder++] = (uint8_t)id;
}

static sig_t *buf_alloc(void)
{
    sig_t *b;
    uint32_t i;
    if (E.nbuf >= ENG_NBUF)
        return 0;
    b = buf_pool[VOICE_NO][E.nbuf++];
    for (i = 0; i < ENG_BLOCK; i++)
        b[i] = 0;
    return b;
}

static void init_state(node_t *nd)
{
    switch (nd->type) {
    case T_LFO:
        nd->s.lfo.seed = 0x2545f491u;
        nd->s.lfo.since = ENG_SR;
        break;
    case T_NOISE:
        nd->s.noise.seed = 0x1234567u;
        break;
    case T_CHIP:
        nd->s.chip.lfsr = 0x7fff;
        break;
    default:
        break;
    }
}

static inline int32_t rd32(const uint8_t *p)
{
    return (int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
}

/* bytes a module record has beyond its type byte and parameters: the attenuverters and the matrix */
static inline uint32_t eng_record_extra(const def_t *d) { return d->cvbase >= 0 ? 14u : 0u; }

int eng_load(const uint8_t *p, uint32_t len)
{
    uint32_t i, j, c, at = 8, ncab;
    uint8_t st[ENG_MAX_MODULES];
    E.loaded = 0;
    E.nheld = 0;
    E.voice.pitch = E.voice.gate = E.voice.vel = 0;
    E.clk_seen = clk_ticks;
    if (len < 8u || p[0] != 0xA1 || p[1] != ENG_PATCH_VERSION)
        return -1;
    E.nmod = p[2];
    ncab = (uint32_t)p[4] | (uint32_t)p[5] << 8;
    if (E.nmod > ENG_MAX_MODULES || ncab > ENG_MAX_CABLES)
        return -2;
    E.ncab = (uint16_t)ncab;
    E.nbuf = 0;
    E.ndly = 0;
    E.nrev = 0;
    for (i = 0; i < E.nmod; i++) {
        node_t *nd = &E.node[i];
        const def_t *d;
        uint8_t *z = (uint8_t *)nd;
        for (j = 0; j < sizeof *nd; j++)
            z[j] = 0;
        if (at >= len || p[at] >= N_TYPES)
            return -3;
        nd->type = p[at++];
        d = &DEFS[nd->type];
        if (at + eng_record_extra(d) + 4u * d->npar > len)
            return -3;
        for (j = 0; j < d->npar; j++, at += 4)
            nd->par[j] = rd32(p + at);
        if (d->cvbase >= 0) {
            nd->att[0] = rd32(p + at);
            nd->att[1] = rd32(p + at + 4);
            at += 8;
            for (j = 0; j < 6u; j++)
                nd->map[j] = p[at++];
        }
        init_state(nd);
        if (nd->type == T_REVERB) {
            if (E.nrev >= ENG_NREV)
                return -4;
            nd->s.rev.buf = rev_pool[VOICE_NO][E.nrev++];
            for (j = 0; j < RV_LEN; j++)
                nd->s.rev.buf[j] = 0;
        }
        if (nd->type == T_DELAY) {
            if (E.ndly >= ENG_NDELAY)
                return -4;
            nd->s.dly.buf = dly_pool[VOICE_NO][E.ndly++];
            nd->s.dly.t = nd->s.dly.seen = nd->s.dly.prev = 0;
            nd->s.dly.beat = ENG_SR / 2u;             /* 120 bpm until a clock has come in */
            for (j = 0; j < DELAY_LEN; j++)
                nd->s.dly.buf[j] = 0;
        }
    }
    if (at + 4u * ncab > len)
        return -3;
    for (c = 0; c < ncab; c++, at += 4) {
        uint8_t fm = p[at], fp = p[at + 1], tm = p[at + 2], tp = p[at + 3];
        if (fm >= E.nmod || tm >= E.nmod || fm == tm)
            return -5;
        if (fp >= DEFS[E.node[fm].type].nout || tp >= DEFS[E.node[tm].type].nin)
            return -5;
        E.from_m[c] = fm;
        E.from_p[c] = fp;
        E.to_m[c] = tm;
        E.to_p[c] = tp;
        E.node[tm].conn |= (uint8_t)(1u << tp);
    }
    /* output buffers: one per connected output port, the dummy for the others */
    for (i = 0; i < E.nmod; i++)
        for (j = 0; j < DEFS[E.node[i].type].nout; j++)
            E.node[i].out[j] = dummy_out;
    for (c = 0; c < ncab; c++) {
        sig_t **o = &E.node[E.from_m[c]].out[E.from_p[c]];
        if (*o == dummy_out && !(*o = buf_alloc()))
            return -6;
    }
    /* inputs: none -> zeros, one cable -> the source buffer itself, several -> a summing buffer */
    E.nsum = 0;
    E.nsrc = 0;
    for (i = 0; i < E.nmod; i++) {
        node_t *nd = &E.node[i];
        nd->sum0 = E.nsum;
        for (j = 0; j < DEFS[nd->type].nin; j++) {
            uint32_t k, n = 0, first = 0;
            for (c = 0; c < ncab; c++)
                if (E.to_m[c] == i && E.to_p[c] == j)
                    n++;
            if (n == 0) {
                nd->in[j] = zero_buf;
            } else if (n == 1) {
                for (c = 0; E.to_m[c] != i || E.to_p[c] != j; c++)
                    ;
                nd->in[j] = E.node[E.from_m[c]].out[E.from_p[c]];
            } else {
                sig_t *b = buf_alloc();
                if (!b || E.nsum >= MAX_SUM)
                    return -6;
                nd->in[j] = b;
                first = E.nsrc;
                for (c = 0, k = 0; c < ncab; c++)
                    if (E.to_m[c] == i && E.to_p[c] == j)
                        E.sum_src[E.nsrc++] = E.node[E.from_m[c]].out[E.from_p[c]], k++;
                E.sum[E.nsum].dst = b;
                E.sum[E.nsum].first = (uint16_t)first;
                E.sum[E.nsum].n = (uint16_t)k;
                E.nsum++;
            }
        }
        nd->nsum = (uint8_t)(E.nsum - nd->sum0);
    }
    for (i = 0; i < E.nmod; i++)
        st[i] = 0;
    E.norder = 0;
    for (i = 0; i < E.nmod; i++)
        if (st[i] == 0)
            visit(i, st);
    E.nout = 0;
    for (i = 0; i < E.norder; i++)
        if (E.node[E.order[i]].type == T_OUTPUT)
            E.outputs[E.nout++] = E.order[i];
    E.loaded = 1;
    return 0;
}

void eng_set_att(uint32_t module, uint32_t input, int32_t q16)
{
    if (module < E.nmod && input < 2u)
        E.node[module].att[input] = q16;
}

/* what turning knob k (0..3) changes on a module: the matrix column of k1..k4, and on the modules that have no matrix
 * (fm1, output) the n-th parameter */
uint32_t eng_knob_mask(uint32_t module, uint32_t knob)
{
    const def_t *d;
    if (module >= E.nmod || knob >= 4u)
        return 0;
    d = &DEFS[E.node[module].type];
    if (d->cvbase >= 0)
        return E.node[module].map[2u + knob];
    return knob < d->npar ? 1u << knob : 0u;
}

/* Is parameter j of `module` driven by a cable from the FM-1 panel's knob k (a K1..K4 output into p1 or p2, whose matrix column
 * has j)? Then turning that knob while `module` is selected must move the panel's knob, not the base value: the two would add up
 * (and clamp), and the knob would seem to jam at the end of the range. Returns the panel's module index or -1. */
int eng_knob_panel(uint32_t module, uint32_t knob, uint32_t j)
{
    const def_t *d;
    uint32_t c;
    if (module >= E.nmod || knob >= 4u)
        return -1;
    d = &DEFS[E.node[module].type];
    if (d->cvbase < 0)
        return -1;
    for (c = 0; c < E.ncab; c++) {
        uint32_t pin;
        if (E.to_m[c] != module || E.to_p[c] < (uint32_t)d->cvbase || E.to_p[c] > (uint32_t)d->cvbase + 1u)
            continue;
        if (E.node[E.from_m[c]].type != T_FM1 || E.from_p[c] != FM1_O_K1 + knob)
            continue;
        pin = E.to_p[c] - (uint32_t)d->cvbase;
        if (E.node[module].map[pin] >> j & 1u)
            return (int)E.from_m[c];
    }
    return -1;
}

int eng_ncables(void) { return E.loaded ? E.ncab : 0; }
void eng_cable(uint32_t i, uint32_t *from, uint32_t *from_port, uint32_t *to, uint32_t *to_port)
{
    if (i >= E.ncab)
        return;
    *from = E.from_m[i];
    *from_port = E.from_p[i];
    *to = E.to_m[i];
    *to_port = E.to_p[i];
}

int eng_nmodules(void) { return E.loaded ? E.nmod : 0; }
int eng_type(uint32_t module) { return module < E.nmod ? E.node[module].type : -1; }
uint32_t eng_map(uint32_t module, uint32_t source)
{
    return module < E.nmod && source < 6u && DEFS[E.node[module].type].cvbase >= 0 ? E.node[module].map[source] : 0u;
}
int32_t eng_att(uint32_t module, uint32_t input)
{
    return module < E.nmod && input < 2u ? E.node[module].att[input] : 0;
}

void eng_set_param(uint32_t module, uint32_t param, int32_t q16)
{
    if (module < E.nmod && param < DEFS[E.node[module].type].npar)
        E.node[module].par[param] = q16;
}

/* for the knobs: a parameter moved by `clicks` steps of 1/100 of its range (octaves for the cutoff) */
int32_t eng_step_param(uint32_t module, uint32_t j, int32_t clicks, int32_t unit_per_click)
{
    const def_t *d;
    if (module >= E.nmod || j >= DEFS[E.node[module].type].npar)
        return 0;
    d = &DEFS[E.node[module].type];
    return value_of(d, j, unit_of(d, j, E.node[module].par[j]) + clicks * unit_per_click);
}

int eng_param_is_log(uint32_t module, uint32_t j)
{
    return module < E.nmod && j < DEFS[E.node[module].type].npar && DEFS[E.node[module].type].plog && DEFS[E.node[module].type].plog[j];
}

/* the value the module used in the last block: the base plus what the parameter inputs added */
int32_t eng_get_eff(uint32_t module, uint32_t param)
{
    return module < E.nmod && param < DEFS[E.node[module].type].npar ? E.node[module].eff[param] : 0;
}

int32_t eng_get_param(uint32_t module, uint32_t param)
{
    return module < E.nmod && param < DEFS[E.node[module].type].npar ? E.node[module].par[param] : 0;
}

int eng_find(uint32_t type)
{
    uint32_t i;
    if (!E.loaded)
        return -1;
    for (i = 0; i < E.nmod; i++)
        if (E.node[i].type == type)
            return (int)i;
    return -1;
}

/* ------------------------------------------------------------------ voice (mono, last note priority) */

static void update_voice(void)
{
    if (E.nheld) {
        int32_t v = ((int32_t)E.held[E.nheld - 1].note - 60) * 32768;      /* (note - 60) / 12 octaves = k * 65536 / 12 */
        E.voice.pitch = (v >= 0 ? v + 3 : v - 3) / 6;
        E.voice.vel = E.held[E.nheld - 1].vel;
        E.voice.gate = ENG_ONE;
    } else {
        E.voice.gate = 0;
    }
}

void eng_note_off(uint32_t note)
{
    uint32_t i, k = 0;
    for (i = 0; i < E.nheld; i++)
        if (E.held[i].note != note)
            E.held[k++] = E.held[i];
    E.nheld = (uint8_t)k;
    update_voice();
}

void eng_note_on(uint32_t note, int32_t vel_q16)
{
    eng_note_off(note);
    if (E.nheld == MAX_HELD) {
        uint32_t i;
        for (i = 1; i < MAX_HELD; i++)
            E.held[i - 1] = E.held[i];
        E.nheld--;
    }
    E.held[E.nheld].note = (uint8_t)note;
    E.held[E.nheld].vel = vel_q16;
    E.nheld++;
    update_voice();
}

void eng_clock_tick(void)
{
    clk_ticks++;
}

void eng_all_notes_off(void)
{
    E.nheld = 0;
    E.voice.gate = 0;
}

/* ------------------------------------------------------------------ modules */

#define EF(nd, name) ((nd)->eff[name])

static void fill(sig_t *o, int32_t v, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        o[i] = v;
}

static void m_fm1(node_t *nd, uint32_t n)
{
    fill(nd->out[FM1_O_PITCH], E.voice.pitch, n);
    fill(nd->out[FM1_O_GATE], E.voice.gate, n);
    fill(nd->out[FM1_O_VEL], E.voice.vel, n);
    fill(nd->out[FM1_O_K1], EF(nd, FM1_P_K1), n);
    fill(nd->out[FM1_O_K2], EF(nd, FM1_P_K2), n);
    fill(nd->out[FM1_O_K3], EF(nd, FM1_P_K3), n);
    fill(nd->out[FM1_O_K4], EF(nd, FM1_P_K4), n);
    {                                   /* clock: a pulse of half a block per block that saw a tick; nothing without a source */
        uint32_t hi = E.clk_seen != clk_ticks ? (n + 1u) / 2u : 0u, i;
        E.clk_seen = clk_ticks;
        for (i = 0; i < n; i++)
            nd->out[FM1_O_CLOCK][i] = i < hi ? ENG_ONE : 0;
    }
}

static void m_osc(node_t *nd, uint32_t n)
{
    const sig_t *pitch = nd->in[OSC_I_PITCH], *pm = nd->in[OSC_I_PM];
    sig_t *out = nd->out[OSC_O_OUT];
    int32_t pw_q = clampi(EF(nd, OSC_P_PW), 3277, 62259);                  /* 0.05 .. 0.95 */
    uint32_t pw = (uint32_t)pw_q << 16, phase = nd->s.osc.phase, i;
    int32_t base = EF(nd, OSC_P_COARSE) / 12 + EF(nd, OSC_P_FINE) / 1200;
    int32_t kind = round_q16(EF(nd, OSC_P_WAVE)), idx = EF(nd, OSC_P_PM), lvl = EF(nd, OSC_P_LEVEL);
    for (i = 0; i < n; i++) {
        uint32_t dt = exp2_scale(INC_C4, pitch[i] + base), ph;
        if (dt > DT_MAX)
            dt = DT_MAX;
        phase += dt;
        ph = phase;
        if (idx > 0)
            ph += (uint32_t)(((int64_t)mul16(idx, pm[i]) * TURN_PER_RAD) >> 16);
        out[i] = mul16(wave(kind, ph, dt, pw), lvl);
    }
    nd->s.osc.phase = phase;
}

static const uint32_t CHIP_DUTY[4] = {0x20000000u, 0x40000000u, 0x80000000u, 0xC0000000u};   /* pulse 12.5, 25, 50, 75 % */

/* a 4 bit level 0..15 as a signal -1..1 (the same integer steps as chipLevel in modules.ts) */
static inline int32_t chip_level(uint32_t v) { return ((int32_t)(2u * v) - 15) * 65536 / 15; }

/* The chip oscillator (modules.ts chip): pulse widths, the NES triangle / the Game Boy's wave, the noise LFSR; a fast arpeggio over a chord. */
static void m_chip(node_t *nd, uint32_t n)
{
    const sig_t *pitch = nd->in[CHIP_I_PITCH];
    sig_t *out = nd->out[CHIP_O_OUT];
    int gb = round_q16(EF(nd, CHIP_P_CHIP)) == 1, arp = EF(nd, CHIP_P_ARP) >= 32768;
    int32_t shape = clampi(round_q16(EF(nd, CHIP_P_SHAPE)), 0, 6), chord = clampi(round_q16(EF(nd, CHIP_P_CHORD)), 0, CHIP_NCHORD - 1);
    int32_t base = EF(nd, CHIP_P_COARSE) / 12 + EF(nd, CHIP_P_FINE) / 1200, lvl = EF(nd, CHIP_P_LEVEL);
    uint32_t arp_inc = (uint32_t)(((uint64_t)(uint32_t)EF(nd, CHIP_P_SPEED) << 16) / ENG_SR), len = CHIP_CHORD_LEN[chord];
    uint32_t phase = nd->s.chip.phase, nclk = nd->s.chip.noise_clock, aph = nd->s.chip.arp, lfsr = nd->s.chip.lfsr, step = nd->s.chip.step, i;
    for (i = 0; i < n; i++) {
        uint32_t dt, semis = 0;
        int32_t v;
        if (arp) {
            uint32_t a = aph + arp_inc;
            if (a < aph)
                step = (step + 1u) % len;
            aph = a;
            semis = CHIP_CHORD_NOTE[chord][step % len];
        }
        dt = exp2_scale(INC_C4, pitch[i] + base + (int32_t)((semis << 16) / 12u));
        if (dt > DT_MAX)
            dt = DT_MAX;
        phase += dt;
        if (shape <= 3) {
            v = phase < CHIP_DUTY[shape] ? ENG_ONE : -ENG_ONE;
        } else if (shape == 4) {
            uint32_t idx = phase >> 27;
            v = gb ? chip_level(CHIP_GB_WAVE[idx]) : chip_level(idx < 16u ? 15u - idx : idx - 16u);
        } else {
            uint64_t t = (uint64_t)nclk + ((uint64_t)dt * CHIP_NOISE_MUL);
            uint32_t k = (uint32_t)(t >> 32);
            nclk = (uint32_t)t;
            if (k > 8u)
                k = 8u;
            while (k--) {
                int sh = shape == 6;
                uint32_t fb = (lfsr ^ (lfsr >> (sh && !gb ? 6 : 1))) & 1u;
                lfsr = (lfsr >> 1) | (fb << 14);
                if (gb && sh)
                    lfsr = (lfsr & ~(1u << 6)) | (fb << 6);
            }
            v = (lfsr & 1u) ? -ENG_ONE : ENG_ONE;
        }
        out[i] = mul16(v, lvl);
    }
    nd->s.chip.phase = phase;
    nd->s.chip.noise_clock = nclk;
    nd->s.chip.arp = aph;
    nd->s.chip.lfsr = (uint16_t)lfsr;
    nd->s.chip.step = (uint8_t)step;
}

static void lfo_cycle(node_t *nd)
{
    nd->s.lfo.seed = nd->s.lfo.seed * 1664525u + 1013904223u;
    nd->s.lfo.held = (int32_t)(nd->s.lfo.seed >> 15) - ENG_ONE;
}

/* free: rate in Hz. sync: one cycle per note length (DELAY_DIV_NUM / DELAY_DIV_DEN, as the delay) that snaps to every n-th edge of clk
 * (24 ppqn, n = ticks per cycle), runs at the measured clock speed in between and at bpm when there is no clock. */
static void m_lfo(node_t *nd, uint32_t n)
{
    sig_t *out = nd->out[0];
    const sig_t *clk = nd->in[LFO_I_CLK], *rst = nd->in[LFO_I_RESET];
    uint32_t dtfree = (uint32_t)(((uint64_t)(uint32_t)EF(nd, LFO_P_RATE) << 16) / ENG_SR), phase = nd->s.lfo.phase, i, dt;
    int32_t kind = round_q16(EF(nd, LFO_P_WAVE)), depth = EF(nd, LFO_P_DEPTH);
    int sync = EF(nd, LFO_P_SYNC) >= 32768;
    int32_t div = clampi((EF(nd, LFO_P_DIV) + 32768) >> 16, 0, DELAY_NDIV - 1), bpm = EF(nd, LFO_P_BPM);
    uint32_t per = 24u * DELAY_DIV_NUM[div] / DELAY_DIV_DEN[div], dtbpm;
    if (bpm < 65536)
        bpm = 65536;
    dtbpm = (uint32_t)(((uint64_t)(uint32_t)bpm * DELAY_DIV_DEN[div] << 16) / (60u * DELAY_DIV_NUM[div] * ENG_SR));
    for (i = 0; i < n; i++) {
        uint32_t prev = phase;
        int32_t w;
        int c = clk[i] >= 32768, r = rst[i] >= 32768, live;
        if (nd->s.lfo.since < ENG_SR)
            nd->s.lfo.since++;
        if (c && !nd->s.lfo.clk_hi) {
            int32_t d8 = (int32_t)(nd->s.lfo.since << 8);
            if (nd->s.lfo.since >= ENG_SR / 2)
                nd->s.lfo.interval = 0;
            else
                nd->s.lfo.interval = nd->s.lfo.interval > 0 ? nd->s.lfo.interval + (d8 - nd->s.lfo.interval) / 8 : d8;
            nd->s.lfo.since = 0;
            if (sync && ++nd->s.lfo.ticks >= per) {
                nd->s.lfo.ticks = 0;
                phase = prev = 0;
                lfo_cycle(nd);
            }
        }
        nd->s.lfo.clk_hi = (uint8_t)c;
        if (r && !nd->s.lfo.rst_hi) {
            phase = prev = 0;
            nd->s.lfo.ticks = 0;
            lfo_cycle(nd);
        }
        nd->s.lfo.rst_hi = (uint8_t)r;
        live = sync && nd->s.lfo.interval > 0 && nd->s.lfo.since < ENG_SR / 2;
        dt = !sync ? dtfree : live ? (uint32_t)((1ull << 40) / ((uint64_t)per * (uint32_t)nd->s.lfo.interval)) : dtbpm;
        phase += dt;
        if (phase < prev) {
            if (live)
                phase = 0xFFFFFFFFu;        /* wait for the clock's next cycle start */
            else
                lfo_cycle(nd);              /* a new cycle: sample & hold takes a new random value */
        }
        w = kind == 4 ? nd->s.lfo.held : wave(kind, phase, dt, 0x80000000u);
        out[i] = mul16(w, depth);
    }
    nd->s.lfo.phase = phase;
}

static void m_noise(node_t *nd, uint32_t n)
{
    sig_t *out = nd->out[0];
    uint32_t seed = nd->s.noise.seed, i;
    int32_t lvl = EF(nd, 0);
    for (i = 0; i < n; i++) {
        seed = seed * 1664525u + 1013904223u;
        out[i] = mul16((int32_t)(seed >> 15) - ENG_ONE, lvl);
    }
    nd->s.noise.seed = seed;
}

static void m_const(node_t *nd, uint32_t n) { fill(nd->out[0], EF(nd, 0), n); }

static void m_scale(node_t *nd, uint32_t n)
{
    const sig_t *in = nd->in[SCALE_I_IN];
    sig_t *out = nd->out[0];
    int32_t g = EF(nd, SCALE_P_GAIN), o = EF(nd, SCALE_P_OFFSET);
    uint32_t i;
    for (i = 0; i < n; i++)
        out[i] = mul16(in[i], g) + o;
}

/* 1 - exp(-4.6 / max(t * sr, 1)) in Q30, t in seconds Q16 */
static int32_t env_rate(int32_t t_q16)
{
    uint64_t tsr = ((uint64_t)(uint32_t)t_q16 * ENG_SR) >> 8;        /* samples, Q8 */
    uint64_t y;
    if (tsr < 256u)
        tsr = 256u;
    y = 1264438371942ull / tsr;                                       /* 4.6 in Q30 / samples */
    if (y < (1ull << 26)) {                                           /* series: y - y^2/2 + y^3/6 - y^4/24 */
        int64_t y2 = (int64_t)((y * y) >> 30), y3 = (y2 * (int64_t)y) >> 30, y4 = (y3 * (int64_t)y) >> 30;
        return (int32_t)((int64_t)y - y2 / 2 + y3 / 6 - y4 / 24);
    } else {                                                          /* exp(-y) = 2^(-y log2 e) */
        uint64_t z = (y * 1549082005ull) >> 30;                       /* Q30 */
        uint32_t n = (uint32_t)(z >> 30), f = (uint32_t)z & 0x3FFFFFFFu, i = f >> 22, r = (f >> 14) & 255u;
        uint32_t e31 = EXPN_T[i] - (uint32_t)(((uint64_t)(EXPN_T[i] - EXPN_T[i + 1u]) * r) >> 8);
        uint32_t e30 = n >= 31u ? 0u : e31 >> (n + 1u);
        return (int32_t)(1u << 30) - (int32_t)e30;
    }
}

static void m_adsr(node_t *nd, uint32_t n)
{
    const sig_t *gate = nd->in[ADSR_I_GATE];
    sig_t *out = nd->out[0];
    int32_t ad = env_rate(EF(nd, ADSR_P_D)), ar = env_rate(EF(nd, ADSR_P_R));
    uint64_t asr = ((uint64_t)(uint32_t)EF(nd, ADSR_P_A) * ENG_SR) >> 8;
    int32_t ainc, sus = EF(nd, ADSR_P_S) << 14, v = nd->s.adsr.v;
    uint8_t stage = nd->s.adsr.stage, prev = nd->s.adsr.prev;
    uint32_t i;
    if (asr < 256u)
        asr = 256u;
    ainc = (int32_t)((1ull << 38) / asr);
    for (i = 0; i < n; i++) {
        uint8_t g = gate[i] > 32768 ? 1 : 0;
        if (g && !prev)
            stage = 1;
        else if (!g && prev)
            stage = 4;
        prev = g;
        switch (stage) {
        case 1:
            v += ainc;
            if (v >= (1 << 30)) {
                v = 1 << 30;
                stage = 2;
            }
            break;
        case 2: {
            int32_t d = sus - v;
            v += mulq(d, ad, 30);
            d = sus - v;
            if ((d < 0 ? -d : d) < 107374)
                stage = 3;
            break;
        }
        case 3:
            v = sus;
            break;
        case 4:
            v -= mulq(v, ar, 30);
            if (v < 107374) {
                v = 0;
                stage = 0;
            }
            break;
        }
        out[i] = v >> 14;
    }
    nd->s.adsr.v = v;
    nd->s.adsr.stage = stage;
    nd->s.adsr.prev = prev;
}

static void m_vca(node_t *nd, uint32_t n)
{
    const sig_t *in = nd->in[VCA_I_IN], *cv = nd->in[VCA_I_CV];
    sig_t *out = nd->out[0];
    int32_t g = EF(nd, VCA_P_GAIN);
    uint32_t i;
    if (!(nd->conn & (1u << VCA_I_CV)))
        for (i = 0; i < n; i++)
            out[i] = mul16(in[i], g);
    else
        for (i = 0; i < n; i++)
            out[i] = mul16(mul16(in[i], cv[i]), g);
}

typedef struct {
    int32_t g, a1, a2, a3;
} svf_coef_t;

/* the coefficients of one stage for the tan() warped cutoff g */
static void svf_stage(svf_coef_t *c, int32_t g, int32_t k24)
{
    int32_t den = (1 << 24) + mulq(g, g + k24, 24);
    c->g = g;
    c->a1 = recip24(den);
    c->a2 = mulq(g, c->a1, 24);
    c->a3 = mulq(g, c->a2, 24);
}

/* the warped cutoff g = tan(pi fc / sr) for a cutoff given as a phase increment (cutoff / sr in u32 turns) */
static int32_t svf_g(uint32_t rinc)
{
    uint32_t i, fr;
    int32_t t0, t1;
    if (rinc > DT_MAX)
        rinc = DT_MAX;
    i = rinc >> 22;
    fr = (rinc >> 6) & 0xFFFFu;
    t0 = TAN_T[i];
    t1 = TAN_T[i + 1u];
    return t0 + (int32_t)(((int64_t)(t1 - t0) * fr) >> 16);
}

#define SQRT2_24 23726566   /* stage 2's k: sqrt(2) in Q24 (Q 0.7), the resonance comes from stage 1 only */

/* Two stages: stage 1 (12 dB) gives lp12, bp, hp; stage 2 filters lp12 again (lp24); mix fades lp24 -> hp by morph.
 * The integrator states are Q20 (+-2048: room for the resonance peak), the signals Q16; products are rounded.
 * Key tracking moves the cutoff by the voice pitch (octaves, C4 = as set), like the host engine, once per block. */
static void m_filter(node_t *nd, uint32_t n)
{
    const sig_t *in = nd->in[FILTER_I_IN], *cv = nd->in[FILTER_I_CV], *mo = nd->in[FILTER_I_MORPH];
    sig_t *lp = nd->out[FILTER_O_LP12], *bp = nd->out[FILTER_O_BP], *hp = nd->out[FILTER_O_HP];
    sig_t *lp24 = nd->out[FILTER_O_LP24], *mix = nd->out[FILTER_O_MIX];
    int32_t k24 = (2 << 24) - mulq(EF(nd, FILTER_P_RES), 33218888, 16);   /* 2 - 1.98 res */
    int32_t cvamt = EF(nd, FILTER_P_CVAMT), ic1 = nd->s.flt.ic1, ic2 = nd->s.flt.ic2, jc1 = nd->s.flt.jc1, jc2 = nd->s.flt.jc2;
    int32_t morph = EF(nd, FILTER_P_MORPH), tr = EF(nd, FILTER_P_TRACK) >= 32768 ? E.voice.pitch : 0;
    uint32_t base = (uint32_t)(((uint64_t)(uint32_t)EF(nd, FILTER_P_CUTOFF) << 16) / ENG_SR), i;
    svf_coef_t c, d;
    int fixed = !(nd->conn & (1u << FILTER_I_CV));
    int msig = nd->conn & (1u << FILTER_I_MORPH);
    if (tr)
        base = exp2_scale(base, tr);
    if (fixed) {
        int32_t g = svf_g(base);
        svf_stage(&c, g, k24);
        svf_stage(&d, g, SQRT2_24);
    }
    for (i = 0; i < n; i++) {
        int32_t v0 = in[i] << 4, v3, v1, v2, w3, w1, w2, h, m;               /* Q20 */
        if (!fixed) {
            int32_t g = svf_g(exp2_scale(base, mul16(cv[i], cvamt)));
            svf_stage(&c, g, k24);
            svf_stage(&d, g, SQRT2_24);
        }
        v3 = v0 - ic2;
        v1 = mulr(c.a1, ic1, 24) + mulr(c.a2, v3, 24);
        v2 = ic2 + mulr(c.a2, ic1, 24) + mulr(c.a3, v3, 24);
        ic1 = clampi(2 * v1 - ic1, -(1 << 30), 1 << 30);
        ic2 = clampi(2 * v2 - ic2, -(1 << 30), 1 << 30);
        h = v0 - mulr(v1, k24, 24) - v2;
        w3 = v2 - jc2;
        w1 = mulr(d.a1, jc1, 24) + mulr(d.a2, w3, 24);
        w2 = jc2 + mulr(d.a2, jc1, 24) + mulr(d.a3, w3, 24);
        jc1 = clampi(2 * w1 - jc1, -(1 << 30), 1 << 30);
        jc2 = clampi(2 * w2 - jc2, -(1 << 30), 1 << 30);
        m = clampi(msig ? morph + mo[i] : morph, 0, 65536);
        lp[i] = (v2 + 8) >> 4;
        bp[i] = (v1 + 8) >> 4;
        hp[i] = (h + 8) >> 4;
        lp24[i] = (w2 + 8) >> 4;
        mix[i] = (w2 + mul16(h - w2, m) + 8) >> 4;
    }
    nd->s.flt.ic1 = ic1;
    nd->s.flt.ic2 = ic2;
    nd->s.flt.jc1 = jc1;
    nd->s.flt.jc2 = jc2;
}

static void m_mixer(node_t *nd, uint32_t n)
{
    const sig_t *a = nd->in[MIXER_I_IN1], *b = nd->in[MIXER_I_IN2], *c = nd->in[MIXER_I_IN3], *d = nd->in[MIXER_I_IN4];
    sig_t *out = nd->out[0];
    int32_t la = EF(nd, MIXER_P_L1), lb = EF(nd, MIXER_P_L2), lc = EF(nd, MIXER_P_L3), ld = EF(nd, MIXER_P_L4);
    uint32_t i;
    for (i = 0; i < n; i++)
        out[i] = mul16(a[i], la) + mul16(b[i], lb) + mul16(c[i], lc) + mul16(d[i], ld);
}

/* the delay line holds Q13 (+-4.0) in int16: half the memory of Q16 in int32, 1.2e-4 resolution */
static inline int16_t dly_pack(int32_t v)
{
    int32_t q = (v + 4) >> 3;
    return (int16_t)clampi(q, -32768, 32767);
}

#define DELAY_HALF (DELAY_LEN / 2u)

/* the note length the time knob selects while the delay is synced: its range split into DELAY_NDIV steps (also used by the LCD) */
uint32_t delay_div(int32_t time_q16)
{
    int64_t lo = DEFS[T_DELAY].pmin[DELAY_P_TIME], range = DEFS[T_DELAY].pmax[DELAY_P_TIME] - lo;
    int64_t i = (((int64_t)time_q16 - lo) * (DELAY_NDIV - 1) * 2 + range) / (range * 2);
    return (uint32_t)(i < 0 ? 0 : i > DELAY_NDIV - 1 ? DELAY_NDIV - 1 : i);
}

/* free or synced time (see modules.ts); ping-pong splits the buffer in two halves, A (input, left) and B (right), see modules.ts */
static void m_delay(node_t *nd, uint32_t n)
{
    const sig_t *in = nd->in[DELAY_I_IN];
    sig_t *out = nd->out[DELAY_O_OUT], *outr = nd->out[DELAY_O_R];
    int16_t *buf = nd->s.dly.buf;
    uint32_t w = nd->s.dly.w, i, d;
    int32_t fb = EF(nd, DELAY_P_FB), mix = EF(nd, DELAY_P_MIX), dry = ENG_ONE - mix;
    int pp = EF(nd, DELAY_P_PINGPONG) >= 32768;
    int64_t smp, top = pp ? (int64_t)DELAY_HALF - 1 : (int64_t)DELAY_LEN - 1;
    if (nd->conn & (1u << DELAY_I_CLK)) {   /* tempo: the time of the last 24 clock pulses, smoothed */
        const sig_t *clk = nd->in[DELAY_I_CLK];
        for (i = 0; i < n; i++) {
            int hi = clk[i] >= 32768;
            nd->s.dly.t++;
            if (hi && !nd->s.dly.prev) {
                uint32_t k = nd->s.dly.seen % DELAY_CLK_PPQN;
                if (nd->s.dly.seen >= DELAY_CLK_PPQN) {
                    uint32_t m = nd->s.dly.t - nd->s.dly.ring[k];
                    if (m >= ENG_SR / 5u && m <= ENG_SR * 3u / 2u)
                        nd->s.dly.beat = nd->s.dly.seen == DELAY_CLK_PPQN ? m : (uint32_t)((int32_t)nd->s.dly.beat + ((int32_t)m - (int32_t)nd->s.dly.beat) / 4);
                }
                nd->s.dly.ring[k] = nd->s.dly.t;
                nd->s.dly.seen++;
            }
            nd->s.dly.prev = (uint8_t)hi;
        }
    }
    if (EF(nd, DELAY_P_SYNC) >= 32768) {
        uint32_t div = delay_div(EF(nd, DELAY_P_TIME));
        smp = ((int64_t)DELAY_DIV_NUM[div] * nd->s.dly.beat * 2 + DELAY_DIV_DEN[div]) / ((int64_t)DELAY_DIV_DEN[div] * 2);
    } else
        smp = ((int64_t)EF(nd, DELAY_P_TIME) * ENG_SR + (int64_t)500 * 65536) / (1000 * 65536);   /* round(t_ms / 1000 * sr) */
    d = (uint32_t)(smp < 1 ? 1 : smp > top ? top : smp);
    if (!pp) {
        for (i = 0; i < n; i++) {
            uint32_t rp = w >= d ? w - d : w + DELAY_LEN - d;
            int32_t r = (int32_t)buf[rp] * 8;
            buf[w] = dly_pack(in[i] + mul16(r, fb));
            if (++w == DELAY_LEN)
                w = 0;
            out[i] = mul16(in[i], dry) + mul16(r, mix);
            outr[i] = out[i];
        }
    } else {
        int16_t *bb = buf + DELAY_HALF;
        if (w >= DELAY_HALF)
            w = 0;
        for (i = 0; i < n; i++) {
            uint32_t rp = w >= d ? w - d : w + DELAY_HALF - d;
            int32_t ea = (int32_t)buf[rp] * 8, eb = (int32_t)bb[rp] * 8, dr = mul16(in[i], dry);
            bb[w] = dly_pack(mul16(ea, fb));
            buf[w] = dly_pack(in[i] + mul16(eb, fb));
            if (++w == DELAY_HALF)
                w = 0;
            out[i] = dr + mul16(ea, mix);
            outr[i] = dr + mul16(eb, mix);
        }
    }
    nd->s.dly.w = w;
}

/* ---- reverb: the same Freeverb-style network as modules.ts, in Q16 with int16 (Q13) delay memory */

static const uint16_t RV_COMB[4] = {1116, 1188, 1277, 1356};
static const uint16_t RV_AP[2] = {556, 441};

static void m_reverb(node_t *nd, uint32_t n)
{
    const sig_t *in = nd->in[REVERB_I_IN];
    sig_t *out = nd->out[0];
    int16_t *base = nd->s.rev.buf, *comb[4], *ap[2];
    int32_t fb = 45875 + mul16(EF(nd, REVERB_P_SIZE), 18350);          /* 0.70 + 0.28 size */
    int32_t d1 = mul16(EF(nd, REVERB_P_DAMP), 26214), mix = EF(nd, REVERB_P_MIX), dry = ENG_ONE - mix;
    uint32_t i, k, off = 0;
    for (k = 0; k < 4u; k++) {
        comb[k] = base + off;
        off += RV_COMB[k];
    }
    for (k = 0; k < 2u; k++) {
        ap[k] = base + off;
        off += RV_AP[k];
    }
    for (i = 0; i < n; i++) {
        int32_t x = in[i], g = x >> 4, sum = 0;                        /* the input at 1/16 */
        for (k = 0; k < 4u; k++) {
            uint32_t ix = nd->s.rev.ci[k];
            int32_t o = (int32_t)comb[k][ix] * 8;
            nd->s.rev.lp[k] = mulr(o, ENG_ONE - d1, 16) + mulr(nd->s.rev.lp[k], d1, 16);
            comb[k][ix] = dly_pack(g + mulr(nd->s.rev.lp[k], fb, 16));
            if (++ix == RV_COMB[k])
                ix = 0;
            nd->s.rev.ci[k] = (uint16_t)ix;
            sum += o;
        }
        for (k = 0; k < 2u; k++) {
            uint32_t ix = nd->s.rev.ai[k];
            int32_t bo = (int32_t)ap[k][ix] * 8, y = bo - sum;
            ap[k][ix] = dly_pack(sum + (bo >> 1));
            if (++ix == RV_AP[k])
                ix = 0;
            nd->s.rev.ai[k] = (uint16_t)ix;
            sum = y;
        }
        out[i] = mulr(x, dry, 16) + mulr(sum, mix, 16);
    }
}

/* ------------------------------------------------------------------ rendering */

/* Block-rate parameter modulation (engine.ts applyCv): the mean of p1 / p2 over the block, times the attenuverter, is
 * added (times the range) to the parameters the matrix assigns that input to; clamped when something was added. */
static void apply_cv(node_t *nd, uint32_t n)
{
    const def_t *d = &DEFS[nd->type];
    int32_t mean[2] = {0, 0};
    uint32_t j, i, s, have = 0;
    if (d->cvbase >= 0)
        for (s = 0; s < 2u; s++) {
            const sig_t *cv;
            int64_t sum = 0;
            if (!(nd->conn & (1u << (d->cvbase + s))))
                continue;
            cv = nd->in[d->cvbase + s];
            for (i = 0; i < n; i++)
                sum += cv[i];
            mean[s] = mul16((int32_t)(sum / (int64_t)n), nd->att[s]);
            have |= 1u << s;
        }
    for (j = 0; j < d->npar; j++) {
        int32_t v = nd->par[j];
        uint32_t touched = 0;
        int32_t mod = 0;
        for (s = 0; s < 2u; s++)
            if ((have >> s & 1u) && (nd->map[s] >> j & 1u)) {
                mod += mean[s];
                touched = 1;
            }
        if (!touched)
            nd->eff[j] = v;
        else if (d->plog && d->plog[j])
            nd->eff[j] = value_of(d, j, unit_of(d, j, v) + mod);            /* in octaves */
        else
            nd->eff[j] = clampi(v + mul16(mod, d->pmax[j] - d->pmin[j]), d->pmin[j], d->pmax[j]);
    }
}

static void process(node_t *nd, uint32_t n)
{
    switch (nd->type) {
    case T_FM1: m_fm1(nd, n); break;
    case T_OSC: m_osc(nd, n); break;
    case T_LFO: m_lfo(nd, n); break;
    case T_NOISE: m_noise(nd, n); break;
    case T_CONST: m_const(nd, n); break;
    case T_SCALE: m_scale(nd, n); break;
    case T_ADSR: m_adsr(nd, n); break;
    case T_VCA: m_vca(nd, n); break;
    case T_FILTER: m_filter(nd, n); break;
    case T_MIXER: m_mixer(nd, n); break;
    case T_DELAY: m_delay(nd, n); break;
    case T_REVERB: m_reverb(nd, n); break;
    case T_CHIP: m_chip(nd, n); break;
    default: break;                      /* output: read by eng_render */
    }
}

static void render_voice(int32_t *left, int32_t *right, uint32_t total)   /* the current voice alone, clamped */
{
    uint32_t off, i, k, s;
    for (i = 0; i < total; i++)
        left[i] = right[i] = 0;
    if (!E.loaded)
        return;
    for (off = 0; off < total; off += ENG_BLOCK) {
        uint32_t n = total - off < ENG_BLOCK ? total - off : ENG_BLOCK;
        for (k = 0; k < E.norder; k++) {
            node_t *nd = &E.node[E.order[k]];
            for (s = nd->sum0; s < (uint32_t)nd->sum0 + nd->nsum; s++) {
                const sum_t *sm = &E.sum[s];
                uint32_t q;
                for (i = 0; i < n; i++)
                    sm->dst[i] = 0;
                for (q = 0; q < sm->n; q++) {
                    const sig_t *src = E.sum_src[sm->first + q];
                    for (i = 0; i < n; i++)
                        sm->dst[i] += src[i];
                }
            }
            apply_cv(nd, n);
            process(nd, n);
        }
        for (k = 0; k < E.nout; k++) {
            const node_t *o = &E.node[E.outputs[k]];
            int32_t lvl = o->eff[OUTPUT_P_LEVEL];
            const sig_t *l = o->in[OUTPUT_I_L], *r = (o->conn & (1u << OUTPUT_I_R)) ? o->in[OUTPUT_I_R] : o->in[OUTPUT_I_L];
            for (i = 0; i < n; i++) {
                left[off + i] += mul16(l[i], lvl);
                right[off + i] += mul16(r[i], lvl);
            }
        }
    }
    for (i = 0; i < total; i++) {
        left[i] = clampi(left[i], -ENG_ONE, ENG_ONE);
        right[i] = clampi(right[i], -ENG_ONE, ENG_ONE);
    }
}

void eng_voice_select(uint32_t v)
{
    if (v < ENG_VOICES)
        eng_p = &EV[v];
}

uint32_t eng_voice(void) { return VOICE_NO; }

static volatile int32_t vgain[ENG_VOICES] = {ENG_ONE, ENG_ONE};   /* the mixer: each voice's level, Q16 (0 = muted) */

void eng_voice_gain(uint32_t v, int32_t q16)
{
    if (v < ENG_VOICES)
        vgain[v] = q16 < 0 ? 0 : q16 > ENG_ONE ? ENG_ONE : q16;
}

/* the voices of the patch, mixed: voice 1 into left / right, the others added, then clamped */
void eng_render(int32_t *left, int32_t *right, uint32_t total)
{
    eng_t *keep = eng_p;                /* the audio ISR may run in the middle of a panel call: give the pointer back */
    uint32_t v, off, i;
    int32_t tl[ENG_BLOCK], tr[ENG_BLOCK];
    eng_p = &EV[0];
    render_voice(left, right, total);
    if (vgain[0] != ENG_ONE)
        for (i = 0; i < total; i++) {
            left[i] = mul16(left[i], vgain[0]);
            right[i] = mul16(right[i], vgain[0]);
        }
    for (v = 1; v < ENG_VOICES; v++) {
        eng_p = &EV[v];
        if (!E.loaded)
            continue;
        for (off = 0; off < total; off += ENG_BLOCK) {
            uint32_t n = total - off < ENG_BLOCK ? total - off : ENG_BLOCK;
            render_voice(tl, tr, n);
            for (i = 0; i < n; i++) {
                left[off + i] = clampi(left[off + i] + mul16(tl[i], vgain[v]), -ENG_ONE, ENG_ONE);
                right[off + i] = clampi(right[off + i] + mul16(tr[i], vgain[v]), -ENG_ONE, ENG_ONE);
            }
        }
    }
    eng_p = keep;
}

void eng_note_on_v(uint32_t v, uint32_t note, int32_t vel_q16)     /* a note on one voice, whichever the panel has selected */
{
    eng_t *keep = eng_p;
    eng_voice_select(v);
    eng_note_on(note, vel_q16);
    eng_p = keep;
}

void eng_note_off_v(uint32_t v, uint32_t note)
{
    eng_t *keep = eng_p;
    eng_voice_select(v);
    eng_note_off(note);
    eng_p = keep;
}

void eng_all_notes_off_v(uint32_t v)
{
    eng_t *keep = eng_p;
    eng_voice_select(v);
    eng_all_notes_off();
    eng_p = keep;
}
