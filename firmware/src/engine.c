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

#define ENG_MAX_MODULES 32
#define ENG_MAX_CABLES 192
#define ENG_NBUF 160u               /* signal buffers of ENG_BLOCK samples (a connected output, a summed input) */
#define ENG_NDELAY 2                /* delay modules per patch */
#define DELAY_LEN 46305u            /* ceil(ENG_SR * 1.05) samples, as the reference */
#define MAX_SUM (ENG_MAX_MODULES * 4)
#define MAX_HELD 16

#define INC_C4 25480119u            /* 261.6255653 Hz at ENG_SR, u32 phase per sample */
#define DT_MAX 1932735283u          /* 0.45 of a cycle per sample (the reference limits osc to 0.45, cutoff to 0.45 sr) */
#define TURN_PER_RAD 683565276u     /* 2^32 / 2 pi */

typedef int32_t sig_t;

static inline int32_t mulq(int32_t a, int32_t b, unsigned s) { return (int32_t)(((int64_t)a * b) >> s); }
static inline int32_t mul16(int32_t a, int32_t b) { return mulq(a, b, 16); }
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

/* ------------------------------------------------------------------ engine state */

typedef struct {
    uint8_t type, conn, sum0, nsum;
    int32_t par[MAX_PAR], eff[MAX_PAR];
    const sig_t *in[MAX_IN];
    sig_t *out[MAX_OUT];
    union {
        struct { uint32_t phase; } osc;
        struct { uint32_t phase, seed; int32_t held; } lfo;
        struct { uint32_t seed; } noise;
        struct { int32_t v; uint8_t stage, prev; } adsr;
        struct { int32_t ic1, ic2; } flt;
        struct { int16_t *buf; uint32_t w; } dly;
    } s;
} node_t;

typedef struct {
    sig_t *dst;
    uint16_t first, n;       /* sources in sum_src[] */
} sum_t;

static struct {
    uint8_t loaded, nmod, nsum, nsrc, norder, nout;
    uint8_t order[ENG_MAX_MODULES], outputs[ENG_MAX_MODULES];
    uint8_t from_m[ENG_MAX_CABLES], from_p[ENG_MAX_CABLES], to_m[ENG_MAX_CABLES], to_p[ENG_MAX_CABLES];
    uint16_t ncab, nbuf, ndly;
    node_t node[ENG_MAX_MODULES];
    sum_t sum[MAX_SUM];
    const sig_t *sum_src[ENG_MAX_CABLES];
    struct { uint8_t note; int32_t vel; } held[MAX_HELD];
    uint8_t nheld;
    struct { int32_t pitch, gate, vel; } voice;
} E;

static sig_t buf_pool[ENG_NBUF][ENG_BLOCK] ENG_POOL;
static int16_t dly_pool[ENG_NDELAY][DELAY_LEN] ENG_POOL;
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
    b = buf_pool[E.nbuf++];
    for (i = 0; i < ENG_BLOCK; i++)
        b[i] = 0;
    return b;
}

static void init_state(node_t *nd)
{
    switch (nd->type) {
    case T_LFO:
        nd->s.lfo.seed = 0x2545f491u;
        break;
    case T_NOISE:
        nd->s.noise.seed = 0x1234567u;
        break;
    default:
        break;
    }
}

int eng_load(const uint8_t *p, uint32_t len)
{
    uint32_t i, j, c, at = 8, ncab;
    uint8_t st[ENG_MAX_MODULES];
    E.loaded = 0;
    E.nheld = 0;
    E.voice.pitch = E.voice.gate = E.voice.vel = 0;
    if (len < 8u || p[0] != 0xA1 || p[1] != 1)
        return -1;
    E.nmod = p[2];
    ncab = (uint32_t)p[4] | (uint32_t)p[5] << 8;
    if (E.nmod > ENG_MAX_MODULES || ncab > ENG_MAX_CABLES)
        return -2;
    E.ncab = (uint16_t)ncab;
    E.nbuf = 0;
    E.ndly = 0;
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
        if (at + 4u * d->npar > len)
            return -3;
        for (j = 0; j < d->npar; j++, at += 4)
            nd->par[j] = (int32_t)((uint32_t)p[at] | (uint32_t)p[at + 1] << 8 | (uint32_t)p[at + 2] << 16 | (uint32_t)p[at + 3] << 24);
        init_state(nd);
        if (nd->type == T_DELAY) {
            if (E.ndly >= ENG_NDELAY)
                return -4;
            nd->s.dly.buf = dly_pool[E.ndly++];
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

void eng_set_param(uint32_t module, uint32_t param, int32_t q16)
{
    if (module < E.nmod && param < DEFS[E.node[module].type].npar)
        E.node[module].par[param] = q16;
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

static void m_midi(node_t *nd, uint32_t n)
{
    fill(nd->out[MIDI_O_PITCH], E.voice.pitch, n);
    fill(nd->out[MIDI_O_GATE], E.voice.gate, n);
    fill(nd->out[MIDI_O_VEL], E.voice.vel, n);
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

static void m_lfo(node_t *nd, uint32_t n)
{
    sig_t *out = nd->out[0];
    uint32_t dt = (uint32_t)(((uint64_t)(uint32_t)EF(nd, LFO_P_RATE) << 16) / ENG_SR), phase = nd->s.lfo.phase, i;
    int32_t kind = round_q16(EF(nd, LFO_P_WAVE)), depth = EF(nd, LFO_P_DEPTH);
    for (i = 0; i < n; i++) {
        uint32_t prev = phase;
        int32_t w;
        phase += dt;
        if (phase < prev) {                 /* a new cycle: sample & hold takes a new random value */
            nd->s.lfo.seed = nd->s.lfo.seed * 1664525u + 1013904223u;
            nd->s.lfo.held = (int32_t)(nd->s.lfo.seed >> 15) - ENG_ONE;
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

/* the filter's coefficients for a cutoff given as a phase increment (cutoff / sr in u32 turns) */
static void svf_coef(svf_coef_t *c, uint32_t rinc, int32_t k24)
{
    uint32_t i, fr;
    int32_t t0, t1, g, den;
    if (rinc > DT_MAX)
        rinc = DT_MAX;
    i = rinc >> 22;
    fr = (rinc >> 6) & 0xFFFFu;
    t0 = TAN_T[i];
    t1 = TAN_T[i + 1u];
    g = t0 + (int32_t)(((int64_t)(t1 - t0) * fr) >> 16);
    den = (1 << 24) + mulq(g, g + k24, 24);
    c->g = g;
    c->a1 = recip24(den);
    c->a2 = mulq(g, c->a1, 24);
    c->a3 = mulq(g, c->a2, 24);
}

static void m_filter(node_t *nd, uint32_t n)
{
    const sig_t *in = nd->in[FILTER_I_IN], *cv = nd->in[FILTER_I_CV];
    sig_t *lp = nd->out[FILTER_O_LP], *bp = nd->out[FILTER_O_BP], *hp = nd->out[FILTER_O_HP];
    int32_t k24 = (2 << 24) - mulq(EF(nd, FILTER_P_RES), 33218888, 16);   /* 2 - 1.98 res */
    int32_t cvamt = EF(nd, FILTER_P_CVAMT), ic1 = nd->s.flt.ic1, ic2 = nd->s.flt.ic2;
    uint32_t base = (uint32_t)(((uint64_t)(uint32_t)EF(nd, FILTER_P_CUTOFF) << 16) / ENG_SR), i;
    svf_coef_t c;
    int fixed = !(nd->conn & (1u << FILTER_I_CV));
    if (fixed)
        svf_coef(&c, base, k24);
    for (i = 0; i < n; i++) {
        int32_t v0 = in[i], v3, v1, v2;
        if (!fixed)
            svf_coef(&c, exp2_scale(base, mul16(cv[i], cvamt)), k24);
        v3 = v0 - ic2;
        v1 = mulq(c.a1, ic1, 24) + mulq(c.a2, v3, 24);
        v2 = ic2 + mulq(c.a2, ic1, 24) + mulq(c.a3, v3, 24);
        ic1 = 2 * v1 - ic1;
        ic2 = 2 * v2 - ic2;
        lp[i] = v2;
        bp[i] = v1;
        hp[i] = v0 - mulq(v1, k24, 24) - v2;
    }
    nd->s.flt.ic1 = ic1;
    nd->s.flt.ic2 = ic2;
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

static void m_delay(node_t *nd, uint32_t n)
{
    const sig_t *in = nd->in[DELAY_I_IN];
    sig_t *out = nd->out[0];
    int16_t *buf = nd->s.dly.buf;
    uint32_t w = nd->s.dly.w, i, d;
    int32_t fb = EF(nd, DELAY_P_FB), mix = EF(nd, DELAY_P_MIX), dry = ENG_ONE - mix;
    int64_t smp = ((int64_t)EF(nd, DELAY_P_TIME) * ENG_SR + (int64_t)500 * 65536) / (1000 * 65536);   /* round(t_ms / 1000 * sr) */
    d = (uint32_t)(smp < 1 ? 1 : smp > (int64_t)DELAY_LEN - 1 ? (int64_t)DELAY_LEN - 1 : smp);
    for (i = 0; i < n; i++) {
        uint32_t rp = w >= d ? w - d : w + DELAY_LEN - d;
        int32_t r = (int32_t)buf[rp] * 8;
        buf[w] = dly_pack(in[i] + mul16(r, fb));
        if (++w == DELAY_LEN)
            w = 0;
        out[i] = mul16(in[i], dry) + mul16(r, mix);
    }
    nd->s.dly.w = w;
}

/* ------------------------------------------------------------------ rendering */

/* block-rate parameter modulation: effective = base + mean(cv) * (max - min), clamped (modules.ts / engine.ts applyCv) */
static void apply_cv(node_t *nd, uint32_t n)
{
    const def_t *d = &DEFS[nd->type];
    uint32_t j, i;
    for (j = 0; j < d->npar; j++) {
        int32_t v = nd->par[j];
        if (d->cvbase >= 0 && (nd->conn & (1u << (d->cvbase + j)))) {
            const sig_t *cv = nd->in[d->cvbase + j];
            int64_t sum = 0;
            for (i = 0; i < n; i++)
                sum += cv[i];
            v = clampi(v + mul16((int32_t)(sum / (int64_t)n), d->pmax[j] - d->pmin[j]), d->pmin[j], d->pmax[j]);
        }
        nd->eff[j] = v;
    }
}

static void process(node_t *nd, uint32_t n)
{
    switch (nd->type) {
    case T_MIDI: m_midi(nd, n); break;
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
    default: break;                      /* output: read by eng_render */
    }
}

void eng_render(int32_t *left, int32_t *right, uint32_t total)
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
