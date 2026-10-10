/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 morgulbrut */
/* CPU benchmark: how much one 128-frame block (2902 us at 44.1 kHz) costs per DSP kernel, fixed point against
 * float, so the engine port can be sized. Runs once at boot with interrupts off (the WDT is fed between runs),
 * results go to the LCD. A block is BENCH_N frames; every kernel runs BENCH_REP blocks, the time is per block. */
#define BENCH_N 128u
#define BENCH_REP 16u
#define BLOCK_US 2902u

static volatile int32_t bench_sink_i;
static volatile float bench_sink_f;

static uint32_t bench_run(void (*fn)(void))
{
    uint32_t i, t0, f;
    fm1_wdt_feed();
    f = irq_save();
    t0 = fm1_ticks();
    for (i = 0; i < BENCH_REP; i++)
        fn();
    t0 = fm1_ticks() - t0;
    irq_restore(f);
    return t0 / BENCH_REP / FM1_TICKS_PER_US;             /* us per block, rounded down */
}

static void k_loop(void)                                  /* the loop itself */
{
    int32_t s = 0;
    uint32_t i;
    for (i = 0; i < BENCH_N; i++)
        s += (int32_t)i;
    bench_sink_i = s;
}

static void k_osc_q(void)                                 /* phase accumulator + parabolic sine, Q15 */
{
    uint32_t i, ph = 12345u, inc = 1787529u;
    int32_t s = 0;
    for (i = 0; i < BENCH_N; i++) {
        ph += inc;
        s += sine_q15(ph);
    }
    bench_sink_i = s;
}

static void k_mul_q(void)                                 /* 4 Q15 multiply-adds per frame (gain, mix, ...) */
{
    uint32_t i;
    int32_t a = 12345, b = 23456, c = 0;
    for (i = 0; i < BENCH_N; i++) {
        c = (c * a >> 15) + b;
        c = (c * a >> 15) + b;
        c = (c * a >> 15) + b;
        c = (c * a >> 15) + b;
        c &= 0x7FFF;
    }
    bench_sink_i = c;
}

static void k_svf_q(void)                                 /* state variable filter, Q15 */
{
    uint32_t i;
    int32_t low = 0, band = 0, f = 6000, q = 20000, in = 10000;
    for (i = 0; i < BENCH_N; i++) {
        int32_t high;
        low += (f * band) >> 15;
        high = in - low - ((q * band) >> 15);
        band += (f * high) >> 15;
        in = -in;
    }
    bench_sink_i = low + band;
}

static void k_osc_f(void)                                 /* the same oscillator in float */
{
    uint32_t i;
    float ph = 0.1f, inc = 0.0123f, s = 0.0f;
    for (i = 0; i < BENCH_N; i++) {
        float x, a;
        ph += inc;
        if (ph >= 1.0f)
            ph -= 1.0f;
        x = ph * 2.0f - 1.0f;
        a = x < 0.0f ? -x : x;
        s += 4.0f * x * (1.0f - a);
    }
    bench_sink_f = s;
}

static void k_mul_f(void)
{
    uint32_t i;
    float a = 0.37f, b = 0.71f, c = 0.0f;
    for (i = 0; i < BENCH_N; i++) {
        c = c * a + b;
        c = c * a + b;
        c = c * a + b;
        c = c * a + b;
        if (c > 1.0f)
            c -= 1.0f;
    }
    bench_sink_f = c;
}

static void k_svf_f(void)
{
    uint32_t i;
    float low = 0.0f, band = 0.0f, f = 0.18f, q = 0.6f, in = 0.3f;
    for (i = 0; i < BENCH_N; i++) {
        float high;
        low += f * band;
        high = in - low - q * band;
        band += f * high;
        in = -in;
    }
    bench_sink_f = low + band;
}

static void k_f2i(void)                                   /* float -> int16 at the output, per frame */
{
    uint32_t i;
    float x = -0.9f;
    int32_t s = 0;
    for (i = 0; i < BENCH_N; i++) {
        s += (int32_t)(x * 32767.0f);
        x += 0.0141f;
    }
    bench_sink_i = s;
}

typedef struct {
    const char *name;
    void (*fn)(void);
} bench_t;
static const bench_t BENCH[] = {
    {"LOOP", k_loop}, {"OSC Q", k_osc_q}, {"MAC4 Q", k_mul_q}, {"SVF Q", k_svf_q},
    {"OSC F", k_osc_f}, {"MAC4 F", k_mul_f}, {"SVF F", k_svf_f}, {"F2I", k_f2i},
};
static uint32_t bench_us[sizeof BENCH / sizeof BENCH[0]];

static void bench_all(void)
{
    uint32_t k;
    for (k = 0; k < sizeof BENCH / sizeof BENCH[0]; k++)
        bench_us[k] = bench_run(BENCH[k].fn);
}

static char *bench_put(char *d, const char *s)
{
    while (*s)
        *d++ = *s++;
    return d;
}

static void bench_show(void)                              /* per kernel: us per block, instances per block budget */
{
    char line[24], num[12];
    uint32_t k;
    lcd_fill(0, 0, 240, 240, 0);
    text5x7(8, 6, "US/BLOCK   X@70%", 2, 0xFD20u, 0);
    for (k = 0; k < sizeof BENCH / sizeof BENCH[0]; k++) {
        char *p = line;
        uint32_t us = bench_us[k];
        p = bench_put(p, BENCH[k].name);
        while (p < line + 7)
            *p++ = ' ';
        fmt_int(num, (int32_t)us);
        p = bench_put(p, num);
        while (p < line + 13)
            *p++ = ' ';
        if (k) {
            fmt_int(num, us ? (int32_t)(BLOCK_US * 7u / 10u / us) : 9999);
            p = bench_put(p, num);
        }
        *p = 0;
        text5x7(8, 34u + 24u * k, line, 2, 0xFFFFu, 0);
    }
    text5x7(8, 34u + 24u * k + 8u, "MIDI NOTE: SINE", 2, 0x07E0u, 0);
}
