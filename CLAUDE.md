# Modular-1 — notes for Claude

Nord Modular–style patchable synth for the **M-VAVE FM-1**, with a browser app to edit patches and flash firmware. Owner: morgulbrut. Read `README.md`, `docs/ARCHITECTURE.md`, `LICENSING.md` first.

## Decisions already made with the user

- **No FM engine and no Felucca features.** Only Felucca's flashing path and hardware layer (HAL, USB, update loader) are reused. GPL-3.0-only for the whole repo; keep Felucca's SPDX/copyright headers on copied files.
- Own repo (this one), not the stikka repos. Work on branch `claude/nord-modular-fm1-webapp-cikr6h`; no PR unless asked.
- Aftermarket firmwares exist: Felucca (`hugelton/Felucca`, GPL-3.0, source at commit `a4d7ca2e`), Sloop (installer only, no source found). Felucca is the reference; its source is the only documentation of the hardware.

## State (milestone 1 done, pushed)

- `web/`: Vite + TS. `src/patch.ts` (data model, ordering, validation), `src/modules.ts` (12 module defs + DSP), `src/engine.ts` (block engine, mono), `src/worklet.ts` + `src/audio.ts` (AudioWorklet, bundled via `?worker&url`), `src/editor.ts` (DOM cards + SVG cables), `src/flasher.ts` (UI over Felucca's `src/flasher/fm1ota.js`, `fm1pkg.js`), `src/presets.ts`, `src/input.ts`.
- `npm test` (vitest, 10 tests), `npm run build`, `npm run dev` all pass. The built app was smoke-tested in headless Chromium: audio worklet produces sound on key down, silent after release, cable drag/remove works.
- `firmware/hal`, `firmware/loader`, `firmware/crt0.S`, `firmware/app.ld`, `tools/*` are **unmodified copies** from Felucca. There is **no `firmware/src/`**, so `tools/build.py` cannot run yet.

## Added since milestone 1 (web only, tests pass: 14)

- Preset `303 Acid`; osc `pw` (pulse width); LFO wave `S&H`.
- Every param has a CV input `<param>_cv` (`cvPort` in `modules.ts`): effective = base + mean(cv over block) * (max-min), clamped, applied per block in `Engine.applyCv`. Exception: module `fm1`.
- Module `fm1` ("FM-1 panel"): pitch/gate/vel plus knobs k1..k4 (sliders on the card). Buttons and the other 3 encoders were deliberately left out.
- A single-file HTML build was tried and reverted at the user's request; the build stays multi-file.
- Firmware build was attempted: blocked on macOS (no Docker, toolchain is Linux x86-64), no `firmware/src/`, no SDK files. The user moves to a Linux box, so step 1 below can run natively.

## Not verified — do not claim otherwise

- The flash tab and installer have never run against a real FM-1 in this project.
- FM-1 CPU clock, RAM and flash sizes are unknown. A single reverse-engineering write-up (not seen first-hand) names the chip JieLi AC791N / pi32v2. Memory map hints: `firmware/app.ld`, `tools/build.py` (app slot 0x8DFBC bytes).
- Whether `buildFeluccaPackage` (patch an app.bin into the user's official V15) also swaps in the Felucca loader. Read `web/src/flasher/fm1pkg.js` `Package.setApp` before relying on it.
- Product identity `FM-1_800` (`PRODUCT` in `web/src/flasher.ts` and `tools/build.py`) is a choice, not something the device mandated; it must match `FELUCCA_ID` in the firmware build.

## How the flash path works (from reading Felucca)

SysEx over USB MIDI. The device *pulls* blocks: it sends read requests (cmd 0x30) for addresses in a "logical image" (package minus 20 marker bytes after each of the first 20 32-byte blocks); the host answers with `response()`. Step 1: running firmware checks the package (finish addr `0xE0000000`) and stages the loader; step 2: the loader (`usb_hid_ota.bin`, marker `FELUCCA-LOADER-1`) reads everything and writes flash (finish `0xF0000000`), then reboots. Identity handshake `F0 00 32 45 00 00 00 40 7F F7`; upgrade command `F0 22 24 35 7F F7`. Recovery: an interrupted write leaves the device as `ota-FM-1_*` ("WL80UBOOT" USB name); run the install again. Return to stock: official V15 file, sha256 `db1642b2…edb8a`. Hold OCT− + OCT+ for 5 s on a running Felucca to enter UBOOT.

## Next steps (in order)

1. **Get the toolchain.** `tools/get_toolchain.sh` (downloads from `pkgman.jieliapp.com`) and the AC79 SDK (`Gitee Jieli-Tech/fw-AC79_AIoT_SDK`, tag `AC79NN_SDK_V1.2.1_2023-12-13`; needs `cpu/wl82/tools/uboot.boot`, `cfg_tool.bin`, `cfg/eq_cfg_hw.bin`). These hosts were blocked in the cloud sandbox but should work on the user's machine. Linux x86-64 native; macOS uses Docker (`JIELI_DOCKER`). Env: `JIELI_TOOLCHAIN`, `AC79_SDK`.
2. **Hello-world firmware** in `firmware/src/` (`main.c` etc.): boot on the Felucca HAL, LCD text, sine out, identity `FM-1_800`. Reuse Felucca's `main.c` boot order, `fm1_cstart`, `usb.c` (MIDI + the OTA request handling in `ota.c`/`ota_hw.c` must stay so the device can be re-flashed!), `audio.c`, `lcd.c`. Strip everything engine/UI related. Adapt `tools/build.py` (it compiles only `firmware/src/felucca.c` as a single unit including the other `.c` files, and runs `mmio_check`: register access only inside `hal/`).
3. Run `tools/build.py`, flash with `web/` flasher or `tools/fm1_install.py`. Keep the official `FM-1.fwsc` handy for return-to-stock. Test the recovery path (interrupt a write) deliberately before trusting anything.
4. Port the engine to fixed-point C; golden-vector test against `web/src/engine.ts`. Measure CPU (Felucca shows CPU load in MENU > SYSTEM > INFO; its `tests/target_budget.py` is a reference).
5. Patch transfer over SysEx, panel mapping. See `docs/ARCHITECTURE.md`.

## Gotchas

- Never flash untested firmware that lacks Felucca's OTA handling; that removes the way back (apart from the loader/stock path).
- `pkill -f "<pattern>"` inside a Bash tool call matches the shell itself; use `pgrep`+PID or a pidfile.
- Web MIDI + SysEx needs Chrome/Edge and https or localhost.
- Engine buffers are 128 frames; modules read `ctx.n` (≤128), not `BLOCK`.
- Keep patch-format changes in `patch.ts` and update `validatePatch`, presets and tests together.
