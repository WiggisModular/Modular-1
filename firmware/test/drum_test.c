/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 morgulbrut */
/* Host test of firmware/src/drums.c: the parts' sound, the sequencer, the clock. Build and run: tools/arp_test.sh */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/engine.c"
#include "../src/drums.c"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int32_t L[128], R[128];
static int step_log[1024], nsteps;

/* run n blocks; the playhead is polled after each block (a step is 43 blocks at 120 bpm, so none is missed) */
static int32_t run(int blocks, int32_t *peak_out)
{
    int32_t peak = 0;
    int i, j;
    static uint8_t mark;
    for (i = 0; i < blocks; i++) {
        memset(L, 0, sizeof L);
        memset(R, 0, sizeof R);
        dr_run(128);
        dr_render(L, R, 128);
        for (j = 0; j < 128; j++) {
            int32_t a = L[j] < 0 ? -L[j] : L[j];
            peak = a > peak ? a : peak;
            if (L[j] != R[j])
                fails++;
        }
        if (mark != dr_ui_mark) {
            mark = dr_ui_mark;
            if (nsteps < 1024)
                step_log[nsteps++] = dr_step_ui;
        }
    }
    if (peak_out)
        *peak_out = peak;
    return peak;
}

static void reset(void)
{
    memset(DV, 0, sizeof DV);
    memset(&DR, 0, sizeof DR);
    memset((void *)dr_req, 0, sizeof dr_req);
    memset((void *)dr_hits, 0, sizeof dr_hits);
    DR.seed = 0x2545F491u;
    dr_defaults();
    dr_play = 0;
    nsteps = 0;
}

int main(void)
{
    int32_t pk, tail;
    uint32_t p;
    int zc, prev, j, b;

    /* every part makes sound that dies away */
    for (p = 0; p < DR_N; p++) {
        reset();
        dr_hit(p, 127);
        run(2, &pk);
        CHECK(pk > 6000 && pk <= ENG_ONE, "%s peak %d", DR_NAME[p], pk);
        run(1100, 0);                                   /* 3 s */
        run(1, &tail);
        CHECK(tail < 200, "%s tail %d after 3 s", DR_NAME[p], tail);
    }

    /* the kick is low: its first 40 ms hold few zero crossings (150 Hz sweeping to 52 Hz: at most ~8 in 40 ms) */
    reset();
    dr_hit(DR_KICK, 127);
    zc = 0;
    prev = 0;
    for (b = 0; b < 14; b++) {
        memset(L, 0, sizeof L);
        memset(R, 0, sizeof R);
        dr_run(128);
        dr_render(L, R, 128);
        for (j = 0; j < 128; j++) {
            int s = L[j] > 0;
            if (b + j && s != prev)
                zc++;
            prev = s;
        }
    }
    CHECK(zc >= 2 && zc <= 14, "kick zero crossings in 40 ms: %d", zc);

    /* level 0 is silent, decay changes the tail */
    reset();
    dr.level[DR_SNARE] = 0;
    dr_hit(DR_SNARE, 127);
    run(3, &pk);
    CHECK(pk == 0, "level 0 gives %d", pk);
    reset();
    dr.decay[DR_OHAT] = 0;
    dr_hit(DR_OHAT, 127);
    run(40, 0);
    run(1, &tail);
    {
        int32_t tail_long;
        reset();
        dr.decay[DR_OHAT] = 100;
        dr_hit(DR_OHAT, 127);
        run(40, 0);
        run(1, &tail_long);
        CHECK(tail_long > tail * 3, "decay 100 tail %d vs decay 0 tail %d", tail_long, tail);
    }

    /* the closed hat chokes the open one */
    reset();
    dr.decay[DR_OHAT] = 100;
    dr_hit(DR_OHAT, 127);
    run(10, 0);
    dr_hit(DR_CHAT, 127);
    dr.level[DR_CHAT] = 0;                              /* (so only the choke is heard) */
    run(4, 0);
    run(1, &tail);
    CHECK(tail < 300, "open hat after a closed hat: %d", tail);

    /* the sequencer: 4 on the floor at 120 bpm, 16ths = 43.06 blocks; kick on steps 1 5 9 13 */
    reset();
    dr_load_preset(0);
    dr.bpm = 120;
    dr_play = 1;
    run(43 * 16 + 5, 0);
    CHECK(nsteps >= 16, "steps seen %d", nsteps);
    for (b = 0; b < 16 && b < nsteps; b++)
        CHECK(step_log[b] == b + 1, "step %d is %d", b, step_log[b]);
    dr_play = 0;
    run(2, 0);
    CHECK(dr_step_ui == 0, "stop clears the playhead");

    /* pattern length 5 wraps after 5 steps */
    reset();
    dr.steps = 5;
    dr_play = 1;
    run(43 * 12 + 5, 0);
    CHECK(nsteps >= 11 && step_log[4] == 5 && step_log[5] == 1 && step_log[10] == 1, "length 5: %d %d %d", step_log[4], step_log[5], step_log[10]);

    /* external clock: 24 ppqn ticks at 60 bpm (the internal 240 bpm would be four times faster) */
    reset();
    dr.bpm = 240;
    dr_play = 1;
    nsteps = 0;
    for (b = 0; b < 400; b++) {                         /* 1.16 s; a tick every 1/24 s = 8.6 blocks */
        if (b % 9 == 0)
            dr_ext_ticks++;
        run(1, 0);
    }
    CHECK(nsteps >= 3 && nsteps <= 8, "external clock gave %d steps in 1.16 s (internal would be 20)", nsteps);

    /* loud pattern stays inside the range */
    reset();
    dr.level[0] = dr.level[1] = dr.level[2] = dr.level[3] = 100;
    {
        int q, k;
        for (q = 0; q < 4; q++)
            for (k = 0; k < 16; k++)
                dr.st[q][k].mode = DM_ON;
    }
    dr_play = 1;
    run(43 * 4, &pk);
    CHECK(pk <= ENG_ONE, "peak %d", pk);

    /* the step matrix: only the kick's first step is on, steps = 16 so one pass is 16 * 43 blocks */
#define ONLY(mode_, cond_, vel_) do { int q_, k_; reset(); dr.bpm = 120; for (q_ = 0; q_ < 4; q_++) for (k_ = 0; k_ < 16; k_++) dr.st[q_][k_].mode = DM_OFF; \
    dr.st[DR_KICK][0].mode = (mode_); dr.st[DR_KICK][0].cond = (cond_); dr.st[DR_KICK][0].vel = (vel_); } while (0)
    ONLY(DM_ON, 0, DV_NORMAL);
    dr_play = 1;
    run(43 * 16 - 3, 0);
    CHECK(dr_hits[DR_KICK] == 1, "ON: %d hits", (int)dr_hits[DR_KICK]);
    ONLY(DM_R2, 0, DV_NORMAL);
    dr_play = 1;
    run(43 * 16 - 3, 0);
    CHECK(dr_hits[DR_KICK] == 2, "RATCHET 2: %d hits", (int)dr_hits[DR_KICK]);
    ONLY(DM_R3, 0, DV_NORMAL);
    dr_play = 1;
    run(43 * 16 - 3, 0);
    CHECK(dr_hits[DR_KICK] == 3, "RATCHET 3: %d hits", (int)dr_hits[DR_KICK]);
    ONLY(DM_R4, 0, DV_NORMAL);
    dr_play = 1;
    run(43 * 16 - 3, 0);
    CHECK(dr_hits[DR_KICK] == 4, "RATCHET 4: %d hits", (int)dr_hits[DR_KICK]);
    ONLY(DM_FLAM, 0, DV_NORMAL);
    dr_play = 1;
    run(3, 0);
    CHECK(dr_hits[DR_KICK] == 1, "FLAM: the grace note first, %d hits", (int)dr_hits[DR_KICK]);
    run(10, 0);
    CHECK(dr_hits[DR_KICK] == 2, "FLAM: the hit 20 ms later, %d hits", (int)dr_hits[DR_KICK]);
    /* ratchet hits are spread over the step: the second hit of a ratchet 2 is half a step in (21 blocks) */
    ONLY(DM_R2, 0, DV_NORMAL);
    dr_play = 1;
    run(15, 0);
    CHECK(dr_hits[DR_KICK] == 1, "RATCHET 2 at 15 blocks: %d", (int)dr_hits[DR_KICK]);
    run(12, 0);
    CHECK(dr_hits[DR_KICK] == 2, "RATCHET 2 at 27 blocks: %d", (int)dr_hits[DR_KICK]);

    /* volume: ghost < normal < hard */
    {
        int32_t g, n, h;
        ONLY(DM_ON, 0, DV_GHOST);
        dr_play = 1;
        run(3, &g);
        ONLY(DM_ON, 0, DV_NORMAL);
        dr_play = 1;
        run(3, &n);
        ONLY(DM_ON, 0, DV_HARD);
        dr_play = 1;
        run(3, &h);
        CHECK(g * 2 < n && n < h, "volumes ghost %d normal %d hard %d", g, n, h);
    }

    /* conditions, with a pattern length of 1 so every step is a pass: 1:1 plays every other pass, 2:1 two of three, 1:2 one of three */
    ONLY(DM_ON, DC_PROB + 0, DV_NORMAL);                /* 1:1 */
    dr.steps = 1;
    dr_play = 1;
    run(43 * 20 + 5, 0);
    CHECK(dr_hits[DR_KICK] == 10, "1:1 over 20 passes: %d", (int)dr_hits[DR_KICK]);
    ONLY(DM_ON, DC_PROB + 4, DV_NORMAL);                /* 2:1 */
    dr.steps = 1;
    dr_play = 1;
    run(43 * 18 + 5, 0);
    CHECK(dr_hits[DR_KICK] == 12, "2:1 over 18 passes: %d", (int)dr_hits[DR_KICK]);
    ONLY(DM_ON, DC_PROB + 1, DV_NORMAL);                /* 1:2 */
    dr.steps = 1;
    dr_play = 1;
    run(43 * 18 + 5, 0);
    CHECK(dr_hits[DR_KICK] == 6, "1:2 over 18 passes: %d", (int)dr_hits[DR_KICK]);
    ONLY(DM_ON, DC_PROB + 3 * 4 + 3, DV_NORMAL);        /* 4:4 */
    dr.steps = 1;
    dr_play = 1;
    run(43 * 16 + 5, 0);
    CHECK(dr_hits[DR_KICK] == 8, "4:4 over 16 passes: %d", (int)dr_hits[DR_KICK]);
    ONLY(DM_ON, 5, DV_NORMAL);                          /* 10 % */
    dr.steps = 1;
    dr_play = 1;
    run(43 * 400 + 5, 0);
    CHECK(dr_hits[DR_KICK] > 15 && dr_hits[DR_KICK] < 70, "10%% over 400 passes: %d", (int)dr_hits[DR_KICK]);
    ONLY(DM_ON, 3, DV_NORMAL);                          /* 50 % */
    dr.steps = 1;
    dr_play = 1;
    run(43 * 400 + 5, 0);
    CHECK(dr_hits[DR_KICK] > 150 && dr_hits[DR_KICK] < 250, "50%% over 400 passes: %d", (int)dr_hits[DR_KICK]);
    ONLY(DM_ON, 0, DV_NORMAL);                          /* always */
    dr.steps = 1;
    dr_play = 1;
    run(43 * 50 + 5, 0);
    CHECK(dr_hits[DR_KICK] == 50 + 0 || dr_hits[DR_KICK] == 51, "always over 50 passes: %d", (int)dr_hits[DR_KICK]);

    /* every factory pattern has ghost notes and ratchets, the tweaks stay on the grid, none of them silences or doubles a part's base */
    {
        uint32_t n, t;
        for (t = 0; t < DR_NTWEAKS; t++)
            CHECK(DR_TWEAKS[t].preset < DR_NPRESETS && DR_TWEAKS[t].part < DR_N && DR_TWEAKS[t].step < DR_STEPS &&
                  DR_TWEAKS[t].mode >= DM_ON && DR_TWEAKS[t].mode <= DM_R4 && DR_TWEAKS[t].vel < DV_N, "tweak %u out of range", t);
        for (n = 0; n < DR_NPRESETS; n++) {
            int ghost = 0, ratchet = 0, hard = 0, q, k;
            reset();
            dr_load_preset(n);
            for (q = 0; q < 4; q++)
                for (k = 0; k < 16; k++) {
                    const dr_step_t *s = &dr.st[q][k];
                    ghost += s->mode && s->vel == DV_GHOST;
                    ratchet += s->mode >= DM_R2 && s->mode <= DM_R4;
                    hard += s->mode && s->vel == DV_HARD;
                }
            CHECK(ghost >= 2 && ratchet >= 2, "%s: %d ghost notes, %d ratchets", DR_PRESETS[n].name, ghost, ratchet);
            CHECK(dr.st[DR_KICK][0].mode == DM_ON && dr.st[DR_KICK][0].vel == DV_NORMAL, "%s: the kick on 1 stays plain", DR_PRESETS[n].name);
            CHECK(dr.st[DR_SNARE][4].mode == DM_ON || n >= 6, "%s: the backbeat stays", DR_PRESETS[n].name);
        }
        reset();
        dr_load_preset(0);
        dr.bpm = 120;
        dr_play = 1;
        run(43 * 16 + 5, 0);                            /* a whole pass of a tweaked pattern runs and stays in range */
        run(1, &pk);
        CHECK(pk <= ENG_ONE, "peak %d", pk);
    }

    /* panel side: presets and clamping */
    reset();
    memset((void *)dr_hits, 0, sizeof dr_hits);
    dr_load_preset(3);
    CHECK(dr.preset == 3 && dr.st[DR_KICK][4].mode == DM_ON && dr.st[DR_KICK][1].mode == DM_OFF && dr.bpm == 122, "preset 3");
    dr.steps = 99;
    dr.bpm = 5;
    dr.level[1] = 200;
    dr.st[1][2].mode = 99;
    dr.st[1][2].cond = 99;
    dr.st[1][2].vel = 9;
    dr.cur = 40;
    dr_fix();
    CHECK(dr.steps == 16 && dr.bpm == 40 && dr.level[1] == 100, "dr_fix");
    CHECK(dr.st[1][2].mode == DM_OFF && dr.st[1][2].cond == 0 && dr.st[1][2].vel == DV_NORMAL && dr.cur == 0, "dr_fix of the steps");

    printf(fails ? "drum tests FAILED (%d)\n" : "drum tests passed\n", fails);
    return fails != 0;
}
