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
    dr.pat[0] = dr.pat[1] = dr.pat[2] = dr.pat[3] = 0xFFFF;
    dr_play = 1;
    run(43 * 4, &pk);
    CHECK(pk <= ENG_ONE, "peak %d", pk);

    /* panel side: presets and clamping */
    reset();
    dr_load_preset(3);
    CHECK(dr.preset == 3 && dr.pat[DR_KICK] == 0x1111 && dr.bpm == 122, "preset 3");
    dr.steps = 99;
    dr.bpm = 5;
    dr.level[1] = 200;
    dr_fix();
    CHECK(dr.steps == 16 && dr.bpm == 40 && dr.level[1] == 100, "dr_fix");

    printf(fails ? "drum tests FAILED (%d)\n" : "drum tests passed\n", fails);
    return fails != 0;
}
