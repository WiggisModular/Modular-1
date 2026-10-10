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

## Firmware: not started, and why

The firmware needs JieLi's pi32v2 clang toolchain and the AC79 SDK (from Gitee). Neither host was reachable from the environment this was written in, so nothing under `firmware/` has been compiled or run; `tools/build.py` still expects Felucca's `firmware/src/`, which is deliberately not copied.

Planned steps, each testable on its own:

1. **Hello-world app:** `main.c` that boots on the Felucca HAL, drives the LCD, prints a counter, outputs a sine, and reports identity `FM-1_800`. Flash it with this repo's flasher. Purpose: prove the toolchain, the package and the recovery path before anything else.
2. **Port the engine to fixed-point C:** same module set, patch graph from a compact binary. Check against the TypeScript engine with golden output vectors. A measured CPU budget decides how many modules fit.
3. **Patch transfer:** SysEx messages to upload a patch to RAM and store it in flash, plus live parameter changes (the editor already emits `param` and `patch` events).
4. **Panel mapping:** keys to the MIDI module, knobs assignable to module parameters, LCD showing the patch.

## Open questions

- Per-voice polyphony (the TS engine is mono).
- Fixed-point vs. float on pi32v2 (needs a measurement).
- Patch binary format and flash layout (must not touch the loader or the boot-guard areas).
