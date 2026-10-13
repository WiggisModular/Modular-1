/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 morgulbrut */
/* Fixed-point patch engine: the C port of web/src/engine.ts and modules.ts. Integer only (the FM-1 has no FPU).
 * Signals and parameters are Q16.16 (1.0 = 65536); the sample rate is ENG_SR. Mono voice, as the reference. */
#pragma once
#include <stdint.h>

#define ENG_SR 44100u
#define ENG_BLOCK 128u
#define ENG_ONE 65536
#define ENG_PATCH_VERSION 8

/* Install a patch from its binary form (web/src/patchbin.ts). 0 on success, negative on error (the engine is then
 * silent). Resets all module state. Never call while eng_render runs (stop the audio ISR's call first). */
int eng_load(const uint8_t *p, uint32_t len);
void eng_set_param(uint32_t module, uint32_t param, int32_t q16);
uint32_t delay_div(int32_t time_q16);                               /* the note length (DELAY_DIV_*) a synced delay's time value selects */
int32_t eng_get_param(uint32_t module, uint32_t param);
int32_t eng_get_eff(uint32_t module, uint32_t param);               /* what the last block used: base + modulation */
void eng_set_att(uint32_t module, uint32_t input, int32_t q16);      /* attenuverter of p1 / p2, Q16 */
int32_t eng_att(uint32_t module, uint32_t input);
int eng_nmodules(void);
int eng_ncables(void);
void eng_cable(uint32_t i, uint32_t *from, uint32_t *from_port, uint32_t *to, uint32_t *to_port);   /* cable i of the patch */
int eng_type(uint32_t module);                                       /* defs.h T_*, -1 if no such module */
/* the matrix: bit mask of the parameters source s (0 p1, 1 p2, 2..5 knobs k1..k4) controls; 0 on modules without a matrix */
uint32_t eng_map(uint32_t module, uint32_t source);
/* the parameters knob k (0..3) changes while the module is selected: its matrix column, and on the modules without a
 * matrix (fm1, output) the k-th parameter */
uint32_t eng_knob_mask(uint32_t module, uint32_t knob);
/* the FM-1 panel module whose knob k drives parameter j of `module` through a cable (then the knob, not the parameter, is turned), else -1 */
int eng_knob_panel(uint32_t module, uint32_t knob, uint32_t j);
/* the value of a parameter after `clicks` knob clicks of `unit_per_click` (Q16 fraction of the range; octaves for the cutoff) */
int32_t eng_step_param(uint32_t module, uint32_t param, int32_t clicks, int32_t unit_per_click);
int eng_param_is_log(uint32_t module, uint32_t param);
/* index of the first module of a type (defs.h T_*), -1 if the patch has none */
int eng_find(uint32_t type);
void eng_note_on(uint32_t note, int32_t vel_q16);
void eng_note_off(uint32_t note);
void eng_all_notes_off(void);
/* the voices (patches that play together): the calls above and the patch / parameter calls act on the selected one; the
 * _v calls name the voice (the arpeggiator plays its own). eng_render mixes all voices. */
#define ENG_VOICES 4                  /* patches that play at once, voices 1..4 */
void eng_voice_select(uint32_t v);
void eng_voice_gain(uint32_t v, int32_t q16);        /* the mixer: a voice's level, Q16 (0 = muted) */
uint32_t eng_voice(void);
void eng_note_on_v(uint32_t v, uint32_t note, int32_t vel_q16);
void eng_note_off_v(uint32_t v, uint32_t note);
void eng_all_notes_off_v(uint32_t v);
/* one clock tick (24 per quarter note); the Main module's clock output pulses once per block that saw one */
void eng_clock_tick(void);
/* n frames of stereo, Q16, clamped to +-1.0; any n, processed in blocks of ENG_BLOCK */
void eng_render(int32_t *l, int32_t *r, uint32_t n);
