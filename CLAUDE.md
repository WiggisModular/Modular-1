# Modular-1 — notes for Claude

Nord Modular–style patchable synth for the **M-VAVE FM-1**, with a browser app to edit patches and flash firmware. Owner: morgulbrut. Read `README.md`, `docs/ARCHITECTURE.md`, `LICENSING.md` first.

## Decisions already made with the user

- **No FM engine and no Felucca features.** Only Felucca's flashing path and hardware layer (HAL, USB, update loader) are reused. GPL-3.0-only for the whole repo; keep Felucca's SPDX/copyright headers on copied files.
- Own repo (this one), not the stikka repos. The default branch is `main` (the work of `claude/nord-modular-fm1-webapp-cikr6h` and the dependabot bumps, vite 8 / vitest 5, were merged into it); GitHub Pages deploys from `main`. No PR unless asked.
- Aftermarket firmwares exist: Felucca (`hugelton/Felucca`, GPL-3.0, source at commit `a4d7ca2e`), Sloop (installer only, no source found). Felucca is the reference; its source is the only documentation of the hardware.

## State (milestone 1 done, pushed)

- `web/`: Vite + TS. `src/patch.ts` (data model, ordering, validation), `src/modules.ts` (12 module defs + DSP), `src/engine.ts` (block engine, mono), `src/worklet.ts` + `src/audio.ts` (AudioWorklet, bundled via `?worker&url`), `src/editor.ts` (DOM cards + SVG cables), `src/flasher.ts` (UI over Felucca's `src/flasher/fm1ota.js`, `fm1pkg.js`), `src/presets.ts`, `src/input.ts`.
- `npm test` (vitest, 10 tests), `npm run build`, `npm run dev` all pass. The built app was smoke-tested in headless Chromium: audio worklet produces sound on key down, silent after release, cable drag/remove works.
- `firmware/hal`, `firmware/crt0.S`, `firmware/app.ld`, `tools/*` are copies from Felucca (`build.py` adapted, loader ID changed to `ota-FM-1_800`).

## Added since milestone 1 (web only, tests pass: 14)

- Preset `303 Acid`; osc `pw` (pulse width); LFO wave `S&H`.
- Every param has a CV input `<param>_cv` (`cvPort` in `modules.ts`): effective = base + mean(cv over block) * (max-min), clamped, applied per block in `Engine.applyCv`. Exception: module `fm1`.
- Module `fm1` ("FM-1 panel"): pitch/gate/vel plus knobs k1..k4 (sliders on the card). Buttons and the other 3 encoders were deliberately left out.
- A single-file HTML build was tried and reverted at the user's request; the build stays multi-file.
- Firmware build was attempted: blocked on macOS (no Docker, toolchain is Linux x86-64), no `firmware/src/`, no SDK files. The user moves to a Linux box, so step 1 below can run natively.

## Firmware build (works, not flashed)

- `nix develop` (flake.nix) sets `JIELI_TOOLCHAIN=~/.jieli/toolchain` and `AC79_SDK=~/.jieli/ac79-sdk`. Toolchain: `tools/get_toolchain.sh`; SDK: sparse clone of `cpu/wl82/tools` at tag `AC79NN_SDK_V1.2.1_2023-12-13` (the 3 files match the expected SHA-256). The prebuilt clang runs on NixOS via nix-ld.
- `python3 tools/build.py` builds `build/modular-1.bin` (15 KB), `build/loader/ota.bin` and `build/modular-1.fwsc` (identity `FM-1_800`) with no warnings; all checks pass.
- `firmware/src/` (bring-up): `modular.c` (single unit), `main.c` (reduced Felucca boot: boot-loop guard, OCT-/OCT+ 5 s UBOOT, OTA, UBOOT SysEx), `audio.c` (sine voice on MIDI notes), `font5x7.c`, plus Felucca's `usb.c`, `ota.c`, `ota_hw.c`, `storage_hw.c`, `lcd.c`, `libc.c` (ota_hw.c edited to use `text5x7`). Felucca source: `~/.jieli/felucca` at `a4d7ca2e`.
- build.py no longer runs `generate()` (the gen_*.py tools are not in this repo) and skips `size_fns.py` unless `FELUCCA_SIZE=1`.
- The sine/MIDI/LCD code has never run on hardware.

## Measured on device (flashing works, user-confirmed)

Benchmark (`firmware/src/bench.c`, numbers in docs/ARCHITECTURE.md): no FPU, float 40-70x slower than Q15 (osc 489 us vs 7 us per 128-frame block). **Decision: audio path fixed point only.** `tools/build.py` links `libcompiler-rt.a` from the toolchain; install with `nix develop -c python3 tools/fm1_install.py build/modular-1.fwsc` (mido/rtmidi are in the flake; the tool asks for confirmation interactively).

## Engine port (done on the host, not yet measured on device)

- `firmware/src/engine.c` (+ `engine.h`, generated `defs.h`, `presets.h`, `dsp_tables.h`): fixed-point port of all 13 modules, mono, Q16.16. `tools/golden.sh` (inside `nix develop`) compares it with `web/src/engine.ts`: 11 patches pass (rms error <= 3 % of reference rms; smooth patches ~0.1 %).
- Regenerate after changing `modules.ts` or `presets.ts`: `cd web && npm run export-defs` (defs.h, presets.h); tables: `python3 tools/gen_dsp_tables.py`. Binary patch format: `web/src/patchbin.ts` (type/port indices are append-only).
- **Verified on the FM-1 (M1-0.2): sound is clean.** 303 Acid costs 11 % of the audio budget (peak 12 %, ISR time per 128-frame half incl. key scan and USB). The 27 keys play F3..G5 (`fm1_in.notes`), the MASTER pot is a software gain read via ADC (square law, `master_q12`); output samples are 24-bit in the LOW bits of each I2S word (Q16 << 6, -6 dBFS ceiling), not left-justified as the HAL comment suggests.
- Device: audio ISR renders the engine; MIDI program change = factory patch, LCD shows CPU %. Build passes; flash it and read the CPU figure for the 303 Acid preset before anything else.
- `firmware/src/bench.c` is only compiled with `-DM1_BENCH`.

## Patch transfer (M1-0.3, host-tested, NOT yet run on the device)

- SysEx protocol in `web/src/protocol.ts` (documented there) <-> `firmware/src/proto.c`; hooks and flash store in `main.c` / `patchstore.c`; browser side `web/src/device.ts` + toolbar in `index.html`/`main.ts`; terminal tool `tools/m1_link.py` (`info`, `upload build/golden/303-acid.m1p`, `param`, `store`, `note`, `bank` = write the 32 factory presets from `presets.h` into the flash bank, `bank-info`, `bank-clear`; the bank commands are untested on the device).
- First thing to do after flashing M1-0.3: `nix develop -c python3 tools/m1_link.py info`, then upload, param, store, power-cycle (the stored patch must come back: label `STORED PATCH`). Flash writes by the app (sectors 0x97000/0x98000) have never run on the device.

## Parameter inputs + matrix + module select (M1-0.6, host-tested; device UI not yet seen)

- Replaces the per-parameter `<param>_cv` inputs: two inputs `p1`, `p2` per module (not fm1/output) with attenuverters `att` (-1..1) and a matrix `map` (sources p1 p2 k1..k4 -> parameters). Model in `web/src/patch.ts` (`fixMatrix`, `defaultMap`, version 2, v1 migration), semantics in `engine.ts applyCv` and `engine.c apply_cv`, binary patch format 2 (`web/src/patchbin.ts`: +8 byte attenuverters +6 byte masks per module that has inputs), protocol cmd 0x21 ATT.
- Device: SELECT (encoder 0) steps through the modules, K1..K4 change the parameters of the selected module per its matrix column (fm1 module: k1..k4 as before). LCD shows module name, the four knob rows and the P1/P2 attenuverters + assignments. Patch size limit 3072 B. Old stored patches (format 1) are refused at boot -> factory patch 0.
- Found and fixed with the golden test: the filter had a DC offset at low cutoff (truncation bias in Q16 states); states are now Q20 and products rounded.
- M1-0.7: the `midi` module is gone. Keys and USB MIDI feed the one voice, which only the `fm1` (FM-1 panel) module outputs (pitch/gate/vel + k1..k4); old patches with `midi` load as `fm1`. Binary patch format is now 3 (type indices moved). A patch needs an FM-1 panel module to be playable.
- Factory patch "FM-1 Panel synth" uses p1/p2 of filter and delay with explicit matrices.

## Preset bank + 32 factory presets (M1-0.8, host-tested; flash bank not yet run on the device)

- `web/src/presets.ts`: 32 factory presets (5 original + 27 generated with `voice()` / `toOut()` helpers, all playing from the FM-1 panel module). `cd web && npm run export-defs` regenerates `firmware/src/presets.h` (all 32 are compiled into the firmware as fallback).
- Bank in flash: `firmware/src/slotstore.c` (32 sectors at 0xA0000), protocol commands SLOT 0x13 / SLOT_ERASE 0x14 / SLOT_INFO 0x15 (reply 0x42), web page `web/src/bank.ts` (tab "Presets", list stored in localStorage `modular-1.bank`). Device: encoder 6 (PRESET) and program change step through the bank (or the factory list if the bank is empty); boot loads the STOREd patch if any, else entry 0.
- Hazard I hit: never `git checkout <file>` on a file with uncommitted work (I lost presets.ts once and had to rewrite it).

## M1-0.9 (host-tested; device UI/buttons not yet seen)

- Buttons: OCT- (id 0) / OCT+ (id 1) octave shift of the keys, SAVE (id 9) stores the state (patch + octave + selected module + preset) via `m1_store`; other buttons show `BTN n` in the header (ids from Felucca's panel table: only PLAY/REC were measured, SAVE may differ: check on the device).
- LCD rewritten in `firmware/src/ui.c`; preview without a device: `nix develop -c tools/ui_preview.sh out.png PRESET [MODULE [OCTAVE]]`. Font now has + % & > = ( ) ? ! # * < , _ '.
- Choice names / units / input names / log flags are generated into `defs.h` (`export-defs.ts`).
- New module `reverb`; patch binary format 4; filter cutoff is logarithmic (`ParamDef.log`); each preset in `presets.ts` ends with plain `wire(kb, 'kN', module, 'p1'|'p2')` lines for K1..K4 (the module's `add` has the base value, `map` and `att`; the panel's `add` the k1..k4 start positions; `voice()` returns `[vca, adsr]` and takes optional opts for both) (rules in docs/ARCHITECTURE.md).
- Pool use is 318 KB of 368 KB (signal buffers, 2 delays, 2 reverbs, the LCD picture buffer).

## Filter rework (host-tested, golden 42 patches pass; not flashed)

- Module `filter` (label now just "Filter", it was "SVF filter"): outputs in this order `lp24`, `lp12` (was `lp`, old patches are migrated in `validatePatch`), `bp`, `hp`, `mix`; input `morph` (after `cv`); params `track` (check box, `ParamDef.toggle`; cutoff follows the played pitch 1 oct/oct, C4 = cutoff as set, per block) and `morph` (0 = 24 dB LP .. 1 = HP, plus the input, clamped). lp24 = a second SVF stage on lp12, damped (k = sqrt 2), so the resonance comes from stage 1. `hp` is the 12 dB high pass of stage 1.
- Binary patch format is now 6 (5: port indices after `cv` moved, 2 new params; 6: filter output order); old stored patches fall back to factory patch 0.

## Flash tab (simplified)

- One click installs the firmware bundled with the app: `tools/build.py` then `cd web && npm run firmware` copies `build/modular-1.fwsc` to `web/public/firmware/modular-1.fwsc` + `modular-1.json` (version, product, sha256; git-ignored). If the FM-1 is sitting in its update loader from an interrupted install, the same button finishes that write. "Return to stock" needs the official `FM-1.fwsc`; "Install another package" takes any `.fwsc` that carries the loader. The "combine the official V15 with an app.bin" step is gone (`buildFeluccaPackage` is no longer used by the UI).

## Not verified — do not claim otherwise

- The flash tab and installer have never run against a real FM-1 in this project.
- FM-1 CPU clock, RAM and flash sizes are unknown. A single reverse-engineering write-up (not seen first-hand) names the chip JieLi AC791N / pi32v2. Memory map hints: `firmware/app.ld`, `tools/build.py` (app slot 0x8DFBC bytes).
- Whether `buildFeluccaPackage` (patch an app.bin into the user's official V15) also swaps in the Felucca loader. Read `web/src/flasher/fm1pkg.js` `Package.setApp` before relying on it.
- Product identity `FM-1_800` (`PRODUCT` in `web/src/flasher.ts` and `tools/build.py`) is a choice, not something the device mandated; it must match `FELUCCA_ID` in the firmware build.

## How the flash path works (from reading Felucca)

SysEx over USB MIDI. The device *pulls* blocks: it sends read requests (cmd 0x30) for addresses in a "logical image" (package minus 20 marker bytes after each of the first 20 32-byte blocks); the host answers with `response()`. Step 1: running firmware checks the package (finish addr `0xE0000000`) and stages the loader; step 2: the loader (`usb_hid_ota.bin`, marker `FELUCCA-LOADER-1`) reads everything and writes flash (finish `0xF0000000`), then reboots. Identity handshake `F0 00 32 45 00 00 00 40 7F F7`; upgrade command `F0 22 24 35 7F F7`. Recovery: an interrupted write leaves the device as `ota-FM-1_*` ("WL80UBOOT" USB name); run the install again. Return to stock: official V15 file, sha256 `db1642b2…edb8a`. Hold OCT− + OCT+ for 5 s on a running Felucca to enter UBOOT.

## Next steps (in order)

1. (done, see above) **Get the toolchain.** `tools/get_toolchain.sh` (downloads from `pkgman.jieliapp.com`) and the AC79 SDK (`Gitee Jieli-Tech/fw-AC79_AIoT_SDK`, tag `AC79NN_SDK_V1.2.1_2023-12-13`; needs `cpu/wl82/tools/uboot.boot`, `cfg_tool.bin`, `cfg/eq_cfg_hw.bin`). These hosts were blocked in the cloud sandbox but should work on the user's machine. Linux x86-64 native; macOS uses Docker (`JIELI_DOCKER`). Env: `JIELI_TOOLCHAIN`, `AC79_SDK`.
2. (built, untested on device) **Hello-world firmware** in `firmware/src/` (`main.c` etc.): boot on the Felucca HAL, LCD text, sine out, identity `FM-1_800`. Reuse Felucca's `main.c` boot order, `fm1_cstart`, `usb.c` (MIDI + the OTA request handling in `ota.c`/`ota_hw.c` must stay so the device can be re-flashed!), `audio.c`, `lcd.c`. Strip everything engine/UI related. Adapt `tools/build.py` (it compiles only `firmware/src/felucca.c` as a single unit including the other `.c` files, and runs `mmio_check`: register access only inside `hal/`).
3. Run `tools/build.py`, flash with `web/` flasher or `tools/fm1_install.py`. Keep the official `FM-1.fwsc` handy for return-to-stock. Test the recovery path (interrupt a write) deliberately before trusting anything.
4. (done on host) Port the engine to fixed-point C; golden-vector test against `web/src/engine.ts`. Measure CPU on device (Felucca shows CPU load in MENU > SYSTEM > INFO; its `tests/target_budget.py` is a reference).
5. Patch transfer over SysEx, panel mapping. See `docs/ARCHITECTURE.md`.

## Gotchas

- **Chromium aborts the whole browser (SIGILL) in `midi::MidiManager::SetInputPortState`** (its ALSA Web MIDI backend, Chromium 151 on this NixOS box; `coredumpctl list` shows a series) when MIDI ports change while any page holds MIDI access, e.g. the FM-1 re-enumerating after a reset or during a firmware update, or other programs (`fm1_install.py`, `m1_link.py`: rtmidi/ALSA clients) coming and going. The web app therefore requests MIDI only on a click (Enable MIDI keyboard, Connect FM-1, the flasher) and has Release MIDI (reload). Prefer `tools/fm1_install.py` for flashing; close the app's tab before running CLI tools. Headless Chromium runs I started also dump core when killed.

- Never flash untested firmware that lacks Felucca's OTA handling; that removes the way back (apart from the loader/stock path).
- `pkill -f "<pattern>"` inside a Bash tool call matches the shell itself; use `pgrep`+PID or a pidfile.
- Web MIDI + SysEx needs Chrome/Edge and https or localhost.
- Engine buffers are 128 frames; modules read `ctx.n` (≤128), not `BLOCK`.
- Keep patch-format changes in `patch.ts` and update `validatePatch`, presets and tests together.
