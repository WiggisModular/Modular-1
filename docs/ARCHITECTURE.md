# Architecture and plan

## Goal

Patch a small modular synth in the browser, run it on an M-VAVE FM-1. No FM engine, sampler, sequencer or UI from Felucca: only its flashing path and its hardware layer.

## What the FM-1 is (as far as known)

From the Felucca and FM-1 reverse-engineering projects: a JieLi AC791N SoC (pi32v2 core), 240×240 colour LCD, 27 keys, 7 knobs plus volume, USB MIDI/audio, TRS MIDI in, battery. CPU clock, RAM and flash sizes are **not verified** here; Felucca's `firmware/app.ld` and `tools/build.py` hold the memory map (app slot 0x8DFBC bytes, app RAM + a pool region).

## Flash path (reused from Felucca)

1. Browser (or `tools/fm1_install.py`) talks SysEx over USB MIDI. The *device* asks for blocks of a "logical image" (the package minus 20 marker bytes) and the host answers.
2. Step 1: the running firmware verifies parts of the package and stages Felucca's update loader. Step 2: the loader reads the whole package and writes flash. Then the device restarts and the identity (`FM-1_<n>`) is checked.
3. A package is built by `tools/fm1pkg_make.py` / `tools/build.py` (app.bin + loader ota.bin + three JieLi SDK files). The browser can also patch an app.bin into the user's own official V15 (`buildFeluccaPackage`).

## Patch engine

`web/src/engine.ts` and `web/src/modules.ts` are the reference implementation: float DSP, 128-frame blocks, patch graph ordered by `processingOrder` (loops become one-block feedback). Module definitions (`ModuleDef`) are data and are shared by the editor and, later, the firmware.

## Firmware

Builds with `tools/build.py` inside `nix develop` (see CLAUDE.md), flashes with the web flasher or `tools/fm1_install.py`.

- `firmware/src/engine.c` is the fixed-point port of `web/src/engine.ts` and `modules.ts` (all 13 modules, mono voice, CV inputs, feedback cables as one block of delay). Signals and parameters are Q16.16, oscillator phase u32, filter coefficients Q24, envelope Q30; sine, exp2 and tan come from tables (`tools/gen_dsp_tables.py` -> `dsp_tables.h`). No float anywhere. The 32x32->64 multiply and the 32-bit divide are hardware; 64-bit division (a libcall) is used only at block rate and on waveform edges.
- `firmware/src/defs.h` and `presets.h` are generated from the TypeScript definitions (`npm run export-defs` in `web/`): module/port/parameter indices and the factory patches in the binary format of `web/src/patchbin.ts`. Port and type indices are append-only.
- The audio ISR (`audio.c`) renders 128 frames per half buffer. MIDI program change selects a factory patch, note on/off and CC 123 drive the voice. The LCD shows the patch name and the measured CPU load (ISR time per half buffer, key scan and USB nested in it).
- `tools/golden.sh` renders 11 patches with the TypeScript engine and the C engine (host build) and compares them. Smooth patches agree to about 0.1 % rms (-60 dB), patches with saw/square edges to 1-2 % rms: the Q16 pitch grid moves an edge by a fraction of a sample, which gives short spikes at the edges and no audible difference.
- Memory: signal buffers 82 KB and two delay lines of 93 KB (int16, Q13) sit in the 360 KB pool; limits are 32 modules, 192 cables, 2 delays.

Measured on the FM-1: the 303 Acid factory patch (8 modules, one delay) uses 11 % of the audio budget (peak 12 %).

Patch transfer (M1-0.3): `web/src/protocol.ts` defines the SysEx messages (`F0 7D 4D 31 cmd payload F7`, payload packed to 7 bit): HELLO/INFO, patch upload in 384-byte CHUNKs with an ACK each, COMMIT with a CRC-16, STORE, live PARAM. `firmware/src/proto.c` implements it (host-tested bit-exactly by `firmware/test/proto_test.c` through `tools/golden.sh`), `patchstore.c` keeps the patch in two A/B flash sectors (0x97000, 0x98000) and the firmware boots the stored patch, else factory patch 0. A failed commit leaves the previous patch running; STORE saves the live-edited parameters too. `web/src/device.ts` is the browser side (toolbar: Connect FM-1, Live, Send patch, Store on FM-1); `tools/m1_link.py` does the same from a terminal.

**Parameter inputs and the source matrix (patch format 2, M1-0.6).** Every module except the FM-1 panel and the output has exactly two parameter inputs, `p1` and `p2`, each with an attenuverter (-1..1; scales and inverts), and a matrix with the sources p1, p2, k1, k2, k3, k4 on one axis and the module's parameters on the other. A parameter's effective value is base + (block mean of p1 x att1 and of p2 x att2, for the inputs the matrix assigns to it) x (max - min), clamped. The four knobs k1..k4 are the FM-1's encoders: on the device the SELECT encoder steps through the modules of the patch, and K1..K4 change the parameters their matrix column assigns on the selected module (one click = 1/100 of the range, or the parameter's own step if larger; five times that when turned fast). The FM-1 panel module is the patch's only input and the exception that stays as it was (there is no separate MIDI module: the keys and USB MIDI are handled by the firmware, and by the browser host, and play the one voice that the panel module outputs as pitch, gate and vel; a saved `midi` module becomes the panel module on load): its parameters are k1..k4 themselves and its outputs feed the patch, so with it selected the knobs work as the panel knobs. Default matrix: p1, k1 -> parameter 1, p2, k2 -> parameter 2, k3 -> parameter 3, k4 -> parameter 4. Old (version 1) JSON patches with `<param>_cv` cables are converted on load. The editor shows each card's two inputs with sliders and the matrix as a checkbox grid; changes to the matrix re-send the patch, attenuverters and parameters are live.

**Preset bank (M1-0.8).** The firmware has the 32 factory presets compiled in (`presets.h`, generated from `web/src/presets.ts`). The editor's Presets page lets you choose up to 32 presets (factory ones or the patch in the editor), order them, and upload them: each goes into one flash sector (`slotstore.c`, slots 0..31 at 0xA0000..0xC0000, header with name and CRC written last) through the SLOT / SLOT_ERASE / SLOT_INFO commands, without being loaded. With a non-empty bank the PRESET knob (encoder 6) and MIDI program change step through the occupied slots, otherwise through the compiled-in presets; the LCD shows `NN/CC NAME`. The bank is separate from the current patch of STORE (patchstore.c), which wins at boot.

Not done yet: the FM-1's buttons, a patch name on the device, reading a patch back, the knob matrix columns have no meaning in the browser (there the sliders are the knobs).

## Open questions

- Per-voice polyphony (the TS engine is mono).
- ~~Fixed-point vs. float on pi32v2~~ settled by measurement, see below.
- Patch binary format and flash layout (must not touch the loader or the boot-guard areas).

## Measured CPU budget (benchmark firmware, `firmware/src/bench.c`)

Per 128-frame block (2902 us of audio), interrupts off, 16 repetitions, 1 us resolution, run on a real FM-1:

| kernel | us/block | instances in 70 % of a block |
|---|---|---|
| osc, Q15 parabolic sine | 7 | 290 |
| 4x Q15 multiply-add | 9 | 225 |
| state variable filter, Q15 | 8 | 253 |
| osc, float | 489 | 4 |
| 4x float multiply-add | 444 | 4 |
| state variable filter, float | 340 | 5 |
| float to int16 per frame | 116 | 17 |

Conclusions: the core has no FPU (float links against `libcompiler-rt.a` soft float) and float is 40 to 70 times slower, so the audio path is **fixed point only** (Q15 samples, 32-bit accumulators, no float, no 64-bit division). Float is acceptable only at control rate (per block), and better avoided. The fixed kernels are tiny; real modules will cost several times their bare kernel (buffer traffic, modulation, CV), so plan for tens of module instances, and measure the real engine before promising more. The 1 us resolution makes the fixed numbers coarse (+-1 us).
