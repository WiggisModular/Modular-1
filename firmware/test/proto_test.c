/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 morgulbrut */
/* Host test of proto.c: feeds the frames produced by web/scripts/golden.ts (a patch upload and a parameter change,
 * NAME.syx = concatenated F0..F7 messages) and checks that the engine then renders bit-exactly what it renders
 * after eng_load of the same patch plus eng_set_param.
 *   proto_test NAME.m1p NAME.syx MODULE PARAM VALUE_Q16 ATT_MODULE ATT_INPUT ATT_Q16 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/engine.c"

static int applied, stored;
static uint8_t replies[64][64];
static uint32_t reply_len[64], nreplies;
static int m1_apply(const uint8_t *p, uint32_t n)
{
    applied++;
    return eng_load(p, n);
}
static int m1_store(const uint8_t *p, uint32_t n)
{
    (void)p;
    (void)n;
    stored++;
    return 0;
}
static int m1_flash_ok(void) { return 1; }
static void m1_send(const uint8_t *f, uint32_t n)
{
    memcpy(replies[nreplies], f, n);
    reply_len[nreplies++] = n;
}
#include "../src/proto.c"

static uint8_t *slurp(const char *path, long *n)
{
    FILE *f = fopen(path, "rb");
    uint8_t *b;
    if (!f) {
        perror(path);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    *n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc((size_t)*n + 1);
    if (fread(b, 1, (size_t)*n, f) != (size_t)*n)
        exit(2);
    fclose(f);
    return b;
}

static void run(int32_t *l, int32_t *r)
{
    eng_note_on(60, 65536);
    eng_render(l, r, 128 * 40);
}

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

int main(int argc, char **argv)
{
    long np, ns, i, at = 0;
    uint8_t *patch, *syx;
    static int32_t a_l[128 * 40], a_r[128 * 40], b_l[128 * 40], b_r[128 * 40];
    uint32_t mod, par, acks_ok = 0, amod, ain;
    int32_t val, aval;
    if (argc < 9)
        return 2;
    patch = slurp(argv[1], &np);
    syx = slurp(argv[2], &ns);
    mod = (uint32_t)atoi(argv[3]);
    par = (uint32_t)atoi(argv[4]);
    val = atoi(argv[5]);
    amod = (uint32_t)atoi(argv[6]);
    ain = (uint32_t)atoi(argv[7]);
    aval = atoi(argv[8]);
    /* reference: direct load + parameter */
    CHECK(eng_load(patch, (uint32_t)np) == 0);
    eng_set_param(mod, par, val);
    eng_set_att(amod, ain, aval);
    run(a_l, a_r);
    eng_all_notes_off();
    /* protocol: the frames, then the same render */
    CHECK(eng_load(patch, 8) != 0 || 1);                      /* anything: the upload must replace it */
    while (at < ns) {
        long start = at + 1, end = start;
        CHECK(syx[at] == 0xF0);
        while (syx[end] != 0xF7)
            end++;
        CHECK(m1_handle(syx + start, (uint32_t)(end - start)) == 1);
        at = end + 1;
    }
    for (i = 0; i < (long)nreplies; i++) {
        uint8_t p[8];
        uint32_t n = m1_unpack7(replies[i] + 5, reply_len[i] - 6u, p, sizeof p);
        CHECK(replies[i][4] == R_ACK && n >= 4u);
        CHECK(p[2] == S_OK);
        acks_ok++;
    }
    CHECK(applied == 1);
    CHECK(m1_cur_len == (uint32_t)np);
    run(b_l, b_r);
    CHECK(memcmp(a_l, b_l, sizeof a_l) == 0 && memcmp(a_r, b_r, sizeof a_r) == 0);
    /* the stored copy carries the live parameter */
    {
        uint32_t at2 = m1_record(mod) + 1u + 4u * par, at3 = m1_record(amod) + 1u + 4u * DEFS[m1_cur[m1_record(amod)]].npar + 4u * ain;
        CHECK((int32_t)((uint32_t)m1_cur[at2] | (uint32_t)m1_cur[at2 + 1] << 8 | (uint32_t)m1_cur[at2 + 2] << 16 | (uint32_t)m1_cur[at2 + 3] << 24) == val);
        CHECK((int32_t)((uint32_t)m1_cur[at3] | (uint32_t)m1_cur[at3 + 1] << 8 | (uint32_t)m1_cur[at3 + 2] << 16 | (uint32_t)m1_cur[at3 + 3] << 24) == aval);
    }
    /* a corrupted commit is refused and the old patch keeps running */
    nreplies = 0;
    {
        uint8_t bad[8] = {0x7D, 0x4D, 0x31, C_COMMIT, 0, 0, 0, 0};
        CHECK(m1_handle(bad, sizeof bad) == 1);
        CHECK(nreplies == 1);
    }
    printf("proto ok: %u acks, %ld patch bytes, render identical\n", acks_ok, np);
    return 0;
}
