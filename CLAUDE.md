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
- Device: audio ISR renders the engine; MIDI program change = factory patch, the CPU figure is no longer drawn (`eng_us_last`/`eng_us_max` remain).
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

- Buttons: OCT- (id 0) / OCT+ (id 1) octave shift of the keys, SAVE (id 9) stores the state (patch + octave + selected module + preset) via `m1_store`; unused buttons show nothing (ids from Felucca's panel table: only PLAY/REC were measured, SAVE may differ: check on the device).
- LCD rewritten in `firmware/src/ui.c`; preview without a device: `nix develop -c tools/ui_preview.sh out.png PRESET [MODULE [OCTAVE]]`. Font now has + % & > = ( ) ? ! # * < , _ '.
- Choice names / units / input names / log flags are generated into `defs.h` (`export-defs.ts`).
- New module `reverb`; patch binary format 4; filter cutoff is logarithmic (`ParamDef.log`); each preset in `presets.ts` ends with plain `wire(kb, 'kN', module, 'p1'|'p2')` lines for K1..K4 (the module's `add` has the base value, `map` and `att`; the panel's `add` the k1..k4 start positions; `voice()` returns `[vca, adsr]` and takes optional opts for both) (rules in docs/ARCHITECTURE.md).
- Pool use is 318 KB of 368 KB (signal buffers, 2 delays, 2 reverbs, the LCD picture buffer).

## Filter rework (host-tested, golden 42 patches pass; not flashed)

- Module `filter` (label now just "Filter", it was "SVF filter"): outputs in this order `lp24`, `lp12` (was `lp`, old patches are migrated in `validatePatch`), `bp`, `hp`, `mix`; input `morph` (after `cv`); params `track` (check box, `ParamDef.toggle`; cutoff follows the played pitch 1 oct/oct, C4 = cutoff as set, per block) and `morph` (0 = 24 dB LP .. 1 = HP, plus the input, clamped). lp24 = a second SVF stage on lp12, damped (k = sqrt 2), so the resonance comes from stage 1. `hp` is the 12 dB high pass of stage 1.
- Binary patch format is now 6 (5: port indices after `cv` moved, 2 new params; 6: filter output order); old stored patches fall back to factory patch 0.

## Panel and LCD pass (host-previewed / built, not flashed)

- LCD: top bar = preset name only (black on `C_ACC`, messages replace it for 1 s); module page like the arp pages (`text_mid` = 1.5x font, 9 px per char), value bars on every knob row (`param_bar`), patch diagram as a vertical strip at x 164..240. `screen_idle` no longer blanks the screen (page switches still do); `put_param` terminates its string.
- Buttons (main.c): OCT- 0, OCT+ 1, FX 2, SEL 3, ENV 4, LFO 5, EDIT 6, GLO 7, HOME 8 (FM-1 panel page), SAVE 9, ARP 10, SEQ 11, PLAY 12, REC 13; unused ones do nothing. HOME/ARP/SEQ/LFO glow dimly; octave LEDs breathe with speed by |octave| (HAL `fm1_br_step`, one shared speed, PLAY's green LED breathes while the arp runs).
- TRS MIDI in: `firmware/src/midi_uart.c` (Felucca's parser), polled in the TIMER5 ISR; never tried with a real source (TRS type A/B unknown).
- USB product string is `Modular-1` (port "Modular-1:Modular-1 MIDI 1"); host tools match felucca|modular|fm-1. Boot splash is off (`-DM1_SPLASH`).

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

## Delay sync / ping-pong (host-tested, golden pass; not flashed)

- Module `delay`: inputs `in`, `clk`; outputs `out`, `r`; params `time`, `fb`, `mix`, `sync` (toggle), `pingpong` (toggle). Sync: the same `time` knob picks a note length (range split in `DELAY_DIVS`, shortest first: 1/16T .. 1/1, shared with the LFO's `div`; `ParamDef.altText` shows the name, LCD special case in `ui.c`, `delay_div()` in engine.c). Tempo = the last 24 pulses of `clk` (the Main module's clock, 24 ppqn), smoothed; 120 bpm until a clock has come in. Ping-pong: one buffer split in two halves, so the time is limited to ~525 ms then; `out` = left, `r` = right (without ping-pong `r` = `out`); `fb` and `mix` apply to both sides. Binary patch format 8.

## Main clock output (host-tested, golden pass; not flashed)

- Module `fm1` (Main) has a gate output `clock` after `k4` (appended, patch format unchanged). Source: the arpeggiator's 24 ppqn tick while `arp_play`, else MIDI clock (0xF8 from USB or TRS; 0xFC mutes until 0xFA/0xFB), else no output. `eng_clock_tick()` counts ticks; `m_fm1` / `Engine.voice.clock` turn a block that saw a tick into a pulse over its first half (block-quantized, ticks within one block merge). Browser: Web MIDI 0xF8/0xFA/0xFB/0xFC via `input.ts` -> worklet `clock` / `clockrun`.

## Two voices (host-tested, golden + arp tests pass; not flashed, CPU not measured)

- Bi-timbral: the engine has `ENG_VOICES` = 2 independent patches (`eng_t EV[]`, `E` = the selected one via `eng_voice_select`; `eng_render` mixes both and restores the pointer, the clock `clk_ticks` is shared). Per voice pool: 80 buffers, 1 delay, 1 reverb (was 160/2/2 for the one patch; same total pool, 323660 B; a patch with two delays or reverbs no longer loads, no preset uses them).
- SEL (button 3) switches the voice that keys, MIDI, knobs, LCD (bar shows `1 NAME` / `2 NAME`), program change and editor uploads act on; the SEL LED is lit on voice 2; notes held on the old voice end. Per-voice panel state is in `vs[]` (ui.c, macros `sel`, `cur_preset`, `patch_label`, `load_err`), the patch copy in `m1_curv[]` (proto.c). One arpeggiator per voice (`AV[2]`, `arp_p` = the selected voice's; PLAY and the arp pages act on it; the clock output follows voice 1's arp, else voice 2's, else MIDI); voice 2's scale is always voice 1's (the scale knob is refused there).
- Storage: see "Saved state" below. Editor STORE is refused while voice 2 is selected.
- LCD accent (`C_ACC`): orange on voice 1, purple (0xA19F) on voice 2; preset names are per voice (`label_buf[]`); the module colours in the patch picture are unchanged.
- Both voices rendering doubles the audio ISR time: measure on the device (`eng_us_max`).

## Mixer page + settings menu (host-previewed / built, not flashed)

- `firmware/src/menu.c` (included after `arp_ui.c`): GLO opens the mixer (`PAGE_MIXER` 4): K1 / K2 the level of voice 1 / 2 (percent, square law, `eng_voice_gain`), the F#3 (OP1) and G#3 (OP2) keys mute voice 1 / 2 and play no notes (`keys_poll`, `MIX_KEYS`). HOME held 0.7 s opens the settings menu (`PAGE_MENU` 5, Felucca style: ALGO steps the tabs DISPLAY / CONTROL / MIDI / SYSTEM, PRESET or SELECT the rows, K1..K4 or OCT-/OCT+ the value; HOME tap or hold closes). HOME now acts on release (tap = go to Main).
- Rows: LEDS, CPU IN BAR, KNOB ACCEL, OCTAVE, MIDI TO (selected / voice 1 / voice 2, notes only; program change stays on the selected voice), CLOCK IN, PROG CHANGE, VERSION, CPU PEAK, UPDATE MODE (OCT+ twice within 3 s -> UBOOT). Settings are `cfg` (ui.c); they and the mixer levels / mutes are saved by SAVE in the upper bits of `ps_state[0]` (`state_pack` / `state_unpack` in main.c; bit 15 marks a record that has them).
- The top bar shows the name at the left and the CPU load at the right (`cpu_pct`: peak ISR time of the last 0.5 s in % of the 2.9 ms half buffer, `cpu_poll`).

## Saved state + defaults (built, not flashed)

- SAVE (and the editor's STORE) writes the whole panel state as one blob into the patch store (`blob_build` in main.c; sectors 0x97000 / 0x98000, `PS_MAX` 3840 B): both voices' patches as they run (parameter edits included), both arpeggiators' settings (`arp_pack`, transport not saved), plus in the header words the octave, menu settings and mixer levels / mutes (`state_pack`), each voice's selected module and preset. Boot: `boot_blob`; a record from before (a bare patch, first byte 0xA1) still loads as voice 1. Too big for the sector: `ERROR`. RAM .bss is now 60 KB of 72.
- MENU > SYSTEM > DEFAULTS (OCT+ twice within 3 s): settings, mixer, arps, octave back to the defaults, voice 1 = preset 0, voice 2 = preset 1, and the stored state is erased (`defaults_now`, `ps_erase`).

## MIDI channels per voice, LED modes, mute LEDs (built, not flashed)

- `cfg.ch[2]`: voice 1 / 2 listen to their own MIDI channel (menu rows V1 / V2 CHANNEL, 0 = omni, defaults 1 and 2; both on the same channel = both play). Notes, CC 123 and program change go to the voices whose channel matches (`midi_drain`); program change loads into that voice. The panel keys still play the selected voice. MENU > MIDI TO is gone.
- `cfg.leds`: OFF / DIM LOW / DIM HIGH (`fm1_led_dim_level`, default dim high). Channels and LED mode are saved in a 3 byte tail of the state blob (after the two patches); the old bits 8 and 13..14 of `ps_state[0]` are no longer used.
- The F#3 / G#3 key LEDs are lit while voice 1 / 2 is muted (`mix_apply`), on every page.

## Four voices, LED levels (built, host tests + golden pass; not flashed, CPU not measured)

- `ENG_VOICES` = 4 (engine.h): four independent patches, SEL cycles 1 -> 2 -> 3 -> 4, colours orange / purple / cyan / green (`VOICE_COLOR`, `C_ACC`), one arpeggiator each (`AV[4]`, scale of voices 2..4 = voice 1's), per voice MIDI channel (defaults 1..4), mixer rows K1..K4, mute keys F#3 G#3 A#3 C#4 = OP1..OP4 (`MIX_KEY_BIT`; OP3/OP4 positions are a guess), mute LEDs on those keys. The clock output follows the lowest voice whose arp runs.
- Memory: to fit four engines, per voice 24 modules, 128 cables, 40 signal buffers, one delay of `DELAY_LEN` 23000 samples (521 ms, the TS reference engine uses the same length now; the old 1.05 s did not fit), one reverb; `M1_PATCH_MAX` 2048. RAM region of app.ld is 24 KB, pool 408 KB (everything big is `ENG_POOL`: `EV`, `m1_curv`, the staging buffers). Pool headroom is 10 KB (build.py wants >= 8 KB). Preset 27 Drone's delay was 700 ms, now 500. The editor's HELLO reply reports 24 modules / 128 cables / 1 delay.
- Saved state blob version 2 (4 patches, 4 arps, tail with channels, LED mode, mixer levels and mutes); version 1 and bare-patch records are still read. Word 1 of the record holds one selected-module byte per voice, word 2 one preset byte per voice. Boot shows STATE RESTORED (or RESTORE FAILED when a record was there but did not load).
- LEDs: OFF / DIM LOW (the old glow, `FM1_LED_DIM_NS` 3.2 us) / DIM HIGH (`FM1_LED_DIM_HI_NS` 8 us, the tick waits the rest of the pulse); `fm1_led_dim_level(0|1|2)`.

## Loading screen (flashed in 1.0.1; not seen on the display by Claude)

- The boot splash is always on now: `splash_draw` right after `lcd_init`, then `boot_step(what)` prints `LOADING INPUT / AUDIO / USB / MIDI / STATE / VOICE n` and a bar (accent colour) while `fm1_main` and `patch_boot` run; it stays up at least `SPLASH_MS` = 5 s (the bar is the elapsed time; after loading the line reads READY; meanwhile `midi_drain` and `host_poll` (editor SysEx, M-UPGRADE, UBOOT SysEx) run, so hosts are answered from the start). The OCT- + OCT+ 5 s UBOOT hold counts from the end of the splash (`t_btn`). While `booting` is set (ui.c) `screen_idle` draws nothing, so the page does not flash through; after `patch_boot` the screen is cleared and the page plus the bar are drawn, and the restore note (STATE RESTORED / RESTORE FAILED) comes from `boot_msg`. The old `M1_SPLASH` define is gone. Host preview sets `booting = 0`.

## Manual tab (flashed in 1.0.1; not seen on the display by Claude)

- The settings menu has a fifth tab MANUAL (`menu.c`, text in `manual.c`): controls, MIDI mapping, editor link, memory limits, one scrollable page (SELECT or PRESET scrolls; PRESET only steps patches outside the menu, ALGO changes tab, OCT buttons do nothing there). Lines are `MANUAL[]` entries, first character = style (`#` heading, ` ` text, `~` dim), at most 35 characters, capitals only (no `;` in the font). Keep it in step with the buttons, `midi_drain` and the limits. Preview: `tools/ui_preview.sh out.png 1 0 0 5 0 4 <first line>`.

- Wording: in user-visible text (LCD manual, web app) a stored sound is a "patch", not a "preset"; only the hardware PRESET knob keeps its name. Identifiers (`PRESETS`, `preset_load`, `#presets` tab id, file names) are unchanged.

## Drum machine, voice 3 (host-tested + previewed, not built for the device, not flashed, CPU not measured)

- `ENG_VOICES` is 2 (two patch voices); the panel has `PVOICES` = 3, SEL cycles 1 -> 2 -> DRUMS (`dsel` in `ui.c`; `eng_voice()` stays at the patch voice used last). `firmware/src/drums.c` (engine side, included after `arp.c`): four synthesized parts KICK / SNARE / C HAT / O HAT (sine with pitch sweep; sine + highpassed noise; second-difference noise; a closed hat chokes the open one), fixed point, Q30 exponential decays, level / decay per part, length (1..16), BPM. `dr_run` (ISR, before `eng_render`) consumes the panel's trigger requests (`dr_req` counters), runs the clock (internal BPM, or MIDI clock ticks counted by `midi_drain` in `dr_ext_ticks`; the internal clock waits 64 blocks after an external tick) and the steps (6 ticks = a 16th); `dr_render` adds the sound after `eng_render` (`dr_gain` = the mixer). 8 factory patterns (`DR_PRESETS`, plain ON steps).
- **Step matrix**: `dr.st[part][step]` = `{mode, cond, vel}`. mode: OFF, ON, RATCHET 2/3/4 (the hits evenly over the step, each 85 % of the one before; the step length is measured between steps, so it follows the external clock too), FLAM (a grace note at 45 % on the step, the hit 20 ms later). cond: 0 always, 1..5 = 90 / 75 / 50 / 25 / 10 %, 6.. = A:B for A, B = 1..4 (`DC_PROB + (A-1)*4 + (B-1)`): plays when `loop % (A+B) < A`, `loop` = passes of the pattern since PLAY. vel: GHOST 45 / NORMAL 110 / HARD 127. `dr_hits[]` counts triggers (the host test uses it).
- Panel (`drum_ui.c`): page 0 = MATRIX (rows = parts, columns = steps, a cursor cell): SELECT = part, K1 = step, K2 = mode, K3 = condition, K4 = volume; keys 1..16 move the cursor to that step and switch it on / off (a step switched on is heard), keys 17..20 select + play a part, the keys play no notes. Page 1 = SOUND (the ARP button, again = back): K1 level and K2 decay of the part, K3 BPM, K4 length. PRESET = factory pattern, PLAY = transport. SEQ / LFO are refused. Cells: stripes = ratchet, two bars = flam, dimmed = ghost, white cap = hard, red corner = has a condition, cyan frame = cursor, white = playhead. Mixer: 3 rows, OP3 key (bit 5, a guess like OP3/OP4 before) mutes. MIDI: channel `cfg.ch[2]` (default 10, menu row DRUM CHANNEL), GM notes 35/36 kick, 38/40 snare, 42/44 closed hat, 46 open hat, program change = pattern.
- Saved state is blob version 4 = version 3 + `DRUM_PACK` (143 bytes: 2 bytes a step, levels, decays, length, pattern, BPM, channel, mixer level / mute) after the tail; version 1..3 records still load (drums at the defaults). `dr_defaults` runs before `patch_boot` and in `defaults_now`.
- Tests: `tools/arp_test.sh` also builds and runs `firmware/test/drum_test.c`. Preview: `tools/ui_preview.sh out.png 0 0 0 9 [PLAYHEAD_STEP]` (page 9 = the drum matrix, 8 = its sound page).
- Not done: swing, tune, per-step probability beyond the list, MIDI start/stop for the drums, drum patterns in the web editor, a drum clock output, a copy / clear of a row.
