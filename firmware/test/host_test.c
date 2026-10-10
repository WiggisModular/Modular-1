/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 morgulbrut */
/* Host test of the fixed-point engine against the TypeScript reference.
 *   host_test NAME.m1p NAME.ev NAME.f32
 * .ev: lines "frame on NOTE VEL" / "frame off NOTE" (frame a multiple of 128, ascending), last line "end FRAMES".
 * .f32: float32 stereo interleaved reference output. Prints max and rms error (full scale = 1.0). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/engine.c"

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

int main(int argc, char **argv)
{
    long np, nr, i;
    uint8_t *patch, *ref_raw;
    float *ref;
    int32_t *l, *r;
    FILE *ev;
    char kind[16];
    long frame, pos = 0, total = 0;
    unsigned note, vel;
    double maxe = 0, sum = 0, maxref = 0, refsum = 0;
    long bad = 0;
    long maxat = 0;
    if (argc < 4) {
        fprintf(stderr, "usage: host_test NAME.m1p NAME.ev NAME.f32\n");
        return 2;
    }
    patch = slurp(argv[1], &np);
    ref_raw = slurp(argv[3], &nr);
    ref = (float *)ref_raw;
    if (eng_load(patch, (uint32_t)np)) {
        fprintf(stderr, "eng_load failed\n");
        return 1;
    }
    ev = fopen(argv[2], "r");
    if (!ev) {
        perror(argv[2]);
        return 2;
    }
    {   /* first pass: total length */
        long fr;
        while (fscanf(ev, "%ld %15s", &fr, kind) == 2) {
            if (!strcmp(kind, "end"))
                total = fr;
            else if (!strcmp(kind, "on"))
                fscanf(ev, "%u %u", &note, &vel);
            else
                fscanf(ev, "%u", &note);
        }
        rewind(ev);
    }
    if (nr != total * 8) {
        fprintf(stderr, "reference has %ld bytes, expected %ld\n", nr, total * 8);
        return 2;
    }
    l = malloc((size_t)total * 4);
    r = malloc((size_t)total * 4);
    while (fscanf(ev, "%ld %15s", &frame, kind) == 2) {
        if (frame > pos) {
            eng_render(l + pos, r + pos, (uint32_t)(frame - pos));
            pos = frame;
        }
        if (!strcmp(kind, "on")) {
            fscanf(ev, "%u %u", &note, &vel);
            eng_note_on(note, (int32_t)(vel * 65536u / 127u));
        } else if (!strcmp(kind, "off")) {
            fscanf(ev, "%u", &note);
            eng_note_off(note);
        }
    }
    for (i = 0; i < total; i++) {
        double a = l[i] / 65536.0 - ref[2 * i], b = r[i] / 65536.0 - ref[2 * i + 1];
        double e = fabs(a) > fabs(b) ? fabs(a) : fabs(b);
        if (e > maxe) {
            maxe = e;
            maxat = i;
        }
        sum += a * a + b * b;
        refsum += (double)ref[2 * i] * ref[2 * i] + (double)ref[2 * i + 1] * ref[2 * i + 1];
        if (e > 0.02)
            bad++;
        if (fabs(ref[2 * i]) > maxref)
            maxref = fabs(ref[2 * i]);
    }
    if (getenv("HOST_TEST_DUMP")) {          /* the C engine's output as float32 stereo, for inspection */
        FILE *d = fopen(getenv("HOST_TEST_DUMP"), "wb");
        for (i = 0; d && i < total; i++) {
            float s[2] = {(float)(l[i] / 65536.0), (float)(r[i] / 65536.0)};
            fwrite(s, 4, 2, d);
        }
        if (d)
            fclose(d);
    }
    printf("max_err %.6f at %ld  rms_err %.6f  rel_rms %.5f  bad %.4f%%  ref_peak %.4f\n", maxe, maxat, sqrt(sum / (2.0 * (double)total)),
           refsum > 0 ? sqrt(sum / refsum) : 0.0, 100.0 * (double)bad / (double)total, maxref);
    return 0;
}
