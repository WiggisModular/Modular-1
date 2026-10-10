/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 * Modified for Modular-1 (morgulbrut). */
/* I2S output (ALNK0 -> external codec) and the audio ISR, which renders the patch engine. Registers: hal/fm1_audio.h */
#define HALF_FRAMES 128u          /* I2S half buffer: 2.9 ms at 44.1 kHz */
#define HALF_WORDS (HALF_FRAMES * 2u)
#define FS 44100u

static int32_t abuf[2u * HALF_WORDS] __attribute__((aligned(4)));

/* the patch engine (engine.c) renders one half buffer per ISR call. eng_run = 0: silence (a patch is being loaded) */
static volatile uint8_t eng_run;
static volatile uint32_t eng_us_last, eng_us_max, eng_halves;
static int32_t eng_l[HALF_FRAMES], eng_r[HALF_FRAMES];

void fm1_alnk0_irq(void)                       /* via isr_alnk0 (hal/fm1_isr.S) */
{
    uint8_t p = fm1_audio_pending();
    fm1_audio_ack_aux(p);
    if (p & FM1_AUDIO_HALF) {
        uint32_t half = fm1_audio_free_half(), i, t0 = fm1_ticks(), us;
        int32_t *o = &abuf[half * HALF_WORDS];
        if (eng_run) {
            eng_render(eng_l, eng_r, HALF_FRAMES);
            for (i = 0; i < HALF_FRAMES; i++) {         /* Q16 (+-1.0) -> 24 bit left justified, -6 dBFS ceiling */
                o[2u * i] = eng_l[i] << 14;
                o[2u * i + 1u] = eng_r[i] << 14;
            }
        } else {
            for (i = 0; i < HALF_WORDS; i++)
                o[i] = 0;
        }
        fm1_audio_ack_half();
        us = (fm1_ticks() - t0) / FM1_TICKS_PER_US;     /* includes the key scan and USB poll nested in the ISR */
        eng_us_last = us;
        if (us > eng_us_max)
            eng_us_max = us;
        eng_halves++;
    }
}
extern void isr_alnk0(void);

static void audio_init(void)                   /* codec and ALNK0 bring-up (fm1_audio.h) */
{
    uint32_t i;
    for (i = 0; i < 2u * HALF_WORDS; i++)
        abuf[i] = 0;
    fm1_audio_init(abuf, HALF_WORDS, isr_alnk0, 3);
}
