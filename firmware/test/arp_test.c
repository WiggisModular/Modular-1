/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 morgulbrut */
/* Host test of firmware/src/arp.c: modes, Euclid rhythm, clock, transport. Build and run: tools/arp_test.sh */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int on[128], events, last_note = -1, last_vel;
static void eng_note_on_v(uint32_t vc, uint32_t n, int32_t v) { (void)vc; on[n] = 1; events++; last_note = (int)n; last_vel = v; }
static void eng_note_off_v(uint32_t vc, uint32_t n) { (void)vc; on[n] = 0; }
static void eng_clock_tick(void) {}
static uint32_t fm1_ticks(void) { return 12345; }
#include "../src/arp.c"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int notes[256], nn;
static int vels[256];
/* record every note-on: run n blocks of 128 frames */
static void run_blocks(int blocks)
{
    int i;
    for (i = 0; i < blocks; i++) {
        int before = events;
        arp_run(128);
        if (events != before && nn < 256) {
            vels[nn] = last_vel;
            notes[nn++] = last_note;
        }
    }
}
static void reset(void)
{
    int i;
    arp_play = 0;
    arp_run(128);
    memset(on, 0, sizeof on);
    nn = 0;
    events = 0;
    arp_c = (arp_cfg_t){ARP_UP, 1, 60, 1, 0, 0, 0, 16, 16, 0, RATE_1_16, 120};   /* C major, 1/16 at 120 bpm */
    arp_dirty = 1;
    (void)i;
}

int main(void)
{
    int i, held;
    /* 1/16 at 120 bpm = 125 ms = 5512.5 frames = 43 blocks */
    reset();
    arp_play = 1;
    run_blocks(43 * 10 + 5);
    CHECK(nn == 10 || nn == 11, "10 steps in 10 step times, got %d", nn);
    for (i = 0; i < 8 && i < nn; i++)
        CHECK(notes[i] == 60 + ARP_SCALES[1].notes[i % 8], "UP step %d = %d", i, notes[i]);
    CHECK(notes[8] == 60, "wraps to the root, got %d", notes[8]);
    CHECK(vels[0] == 100 * 65536 / 127, "default velocity");

    /* the note ends before the next step (gate 65 %), and stopping silences it */
    held = 0;
    for (i = 0; i < 128; i++)
        held += on[i];
    CHECK(held <= 1, "at most one note sounding, got %d", held);
    arp_play = 0;
    arp_run(128);
    held = 0;
    for (i = 0; i < 128; i++)
        held += on[i];
    CHECK(held == 0, "stop ends the note");

    /* DOWN */
    reset();
    arp_c.mode = ARP_DOWN;
    arp_play = 1;
    run_blocks(43 * 3 + 5);
    CHECK(notes[0] == 72 && notes[1] == 71 && notes[2] == 69, "DOWN: %d %d %d", notes[0], notes[1], notes[2]);

    /* INCLUDE plays the ends twice, EXCLUDE once */
    reset();
    arp_c.mode = ARP_INCLUDE;
    arp_c.scale = 40;                   /* major triad 0 4 7 */
    arp_play = 1;
    run_blocks(43 * 10 + 5);
    {
        static const int want[] = {60, 64, 67, 67, 64, 60, 60, 64, 67, 67};
        for (i = 0; i < 10; i++)
            CHECK(notes[i] == want[i], "INCLUDE %d = %d want %d", i, notes[i], want[i]);
    }
    reset();
    arp_c.mode = ARP_EXCLUDE;
    arp_c.scale = 40;
    arp_play = 1;
    run_blocks(43 * 8 + 5);
    {
        CHECK(notes[0] == 60 && notes[1] == 64 && notes[2] == 67 && notes[3] == 64 && notes[4] == 60, "EXCLUDE %d %d %d %d %d",
              notes[0], notes[1], notes[2], notes[3], notes[4]);
    }

    /* octaves: 3 octaves of a triad, UP */
    reset();
    arp_c.scale = 40;
    arp_c.octaves = 3;
    arp_play = 1;
    run_blocks(43 * 9 + 5);
    CHECK(AR.npool == 9 && notes[3] == 72 && notes[8] == 91, "3 octaves: pool %d, %d %d", AR.npool, notes[3], notes[8]);

    /* Euclid 3 of 8 = x..x..x. (floor construction); rotation shifts it */
    reset();
    arp_c.steps = 8;
    arp_c.pulses = 3;
    arp_play = 1;
    run_blocks(43 * 16 + 5);
    CHECK(nn == 6 || nn == 5 || nn == 7, "3 of 8 over two cycles: %d notes", nn);
    {
        int g[8], total = 0;
        for (i = 0; i < 8; i++) {
            g[i] = (int)euclid_gate(8, 3, (uint32_t)i);
            total += g[i];
        }
        CHECK(total == 3, "3 pulses, got %d", total);
        CHECK(!g[0] && !g[1] && g[2] && !g[3] && !g[4] && g[5] && !g[6] && g[7], "pattern .. x..x.x");
    }
    for (i = 1; i <= 32; i++) {         /* every pulse count gives that many onsets */
        int p, total;
        for (p = 0; p <= i; p++) {
            int j;
            total = 0;
            for (j = 0; j < i; j++)
                total += (int)euclid_gate((uint32_t)i, (uint32_t)p, (uint32_t)j);
            CHECK(total == p, "euclid %d/%d = %d", p, i, total);
        }
    }

    /* rhythm chaos 100 % inverts every gate: with all pulses open nothing plays */
    reset();
    arp_c.rchaos = 100;
    arp_play = 1;
    run_blocks(43 * 8);
    CHECK(nn == 0, "rhythm chaos 100%% on a full pattern: %d notes", nn);

    /* melody chaos 100 % keeps all notes inside the pool, velocity chaos varies the velocity */
    reset();
    arp_c.chaos = 100;
    arp_c.vchaos = 100;
    arp_play = 1;
    run_blocks(43 * 40);
    {
        int out = 0, varied = 0;
        for (i = 0; i < nn; i++) {
            int inpool = 0, j;
            for (j = 0; j < AR.npool; j++)
                inpool |= AR.pool[j] == notes[i];
            out += !inpool;
            varied += vels[i] != vels[0];
        }
        CHECK(out == 0, "%d notes outside the pool", out);
        CHECK(varied > nn / 2, "velocity varies: %d of %d", varied, nn);
    }

    /* every mode stays inside the pool, at every octave count and scale size */
    for (i = 0; i < ARP_NMODES; i++) {
        uint32_t sc;
        for (sc = 0; sc < ARP_NSCALES; sc++) {
            int j, bad = 0;
            reset();
            arp_c.mode = (uint8_t)i;
            arp_c.scale = (uint8_t)sc;
            arp_c.octaves = (uint8_t)(1 + sc % 3);
            arp_c.chaos = (uint8_t)(sc * 7 % 101);
            arp_play = 1;
            run_blocks(43 * 40);
            for (j = 0; j < nn; j++) {
                int inpool = 0, k;
                for (k = 0; k < AR.npool; k++)
                    inpool |= AR.pool[k] == notes[j];
                bad += !inpool;
            }
            CHECK(nn > 30 && !bad, "mode %d scale %u: %d notes, %d outside", i, sc, nn, bad);
        }
    }

    /* speed: 240 bpm doubles the step rate; 1/8 halves it */
    reset();
    arp_c.bpm = 240;
    arp_play = 1;
    run_blocks(43 * 10 + 5);
    CHECK(nn >= 19 && nn <= 21, "240 bpm: %d notes in 10 steps of 120 bpm", nn);
    reset();
    arp_c.rate = RATE_1_8;
    arp_play = 1;
    run_blocks(43 * 10 + 5);
    CHECK(nn >= 4 && nn <= 6, "1/8: %d notes", nn);

    /* the settings the knobs reach */
    reset();
    arp_c.steps = 16;
    arp_c.pulses = 16;
    arp_c.rot = 15;
    arp_knob(2, 0, -10, 0);             /* steps 16 -> 6: pulses and rotation follow */
    CHECK(arp_c.steps == 6 && arp_c.pulses == 6 && arp_c.rot == 5, "steps %d pulses %d rot %d", arp_c.steps, arp_c.pulses, arp_c.rot);
    arp_knob(1, 3, -1000, 1);
    CHECK(arp_c.bpm == 40, "bpm floor");
    arp_knob(3, 3, 5, 0);
    CHECK(arp_c.octaves == 3, "octaves ceiling");
    CHECK(arp_knob(3, 3, 1, 0) == 0, "no change reported at the limit");

    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
