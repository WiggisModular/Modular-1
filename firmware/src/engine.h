/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 morgulbrut */
/* Fixed-point patch engine: the C port of web/src/engine.ts and modules.ts. Integer only (the FM-1 has no FPU).
 * Signals and parameters are Q16.16 (1.0 = 65536); the sample rate is ENG_SR. Mono voice, as the reference. */
#pragma once
#include <stdint.h>

#define ENG_SR 44100u
#define ENG_BLOCK 128u
#define ENG_ONE 65536

/* Install a patch from its binary form (web/src/patchbin.ts). 0 on success, negative on error (the engine is then
 * silent). Resets all module state. Never call while eng_render runs (stop the audio ISR's call first). */
int eng_load(const uint8_t *p, uint32_t len);
void eng_set_param(uint32_t module, uint32_t param, int32_t q16);
void eng_note_on(uint32_t note, int32_t vel_q16);
void eng_note_off(uint32_t note);
void eng_all_notes_off(void);
/* n frames of stereo, Q16, clamped to +-1.0; any n, processed in blocks of ENG_BLOCK */
void eng_render(int32_t *l, int32_t *r, uint32_t n);
