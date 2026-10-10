# Modular-1

A Nord Modular–style patchable synth for the **M-VAVE FM-1**, with a browser app to build patches and to flash the firmware.

**Status: milestone 1 — browser only.** The patch editor, the module engine and the flasher UI work in the browser. The FM-1 firmware that runs the engine on the device **does not exist yet**; see [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the plan and what blocks it.

## What works now

- **Patch editor** (`web/`): drag modules onto a canvas, drag from a port to a port to cable them, click a cable to remove it. Cables into one input are summed. Feedback cables are allowed (one block of delay).
- **Modules:** MIDI in, oscillator (sine/tri/saw/square, phase-mod input for FM), LFO, noise, constant, scale, ADSR, VCA, SVF filter (LP/BP/HP), 4-way mixer, delay, output. Max 32 modules per patch.
- **Engine** (`web/src/engine.ts`): block-based, no browser dependencies, runs in an AudioWorklet. Monophonic for now.
- **Input:** computer keyboard (A W S E D F T G Y H U J K, Z/X octave) and Web MIDI.
- **Patches:** presets, save/load as JSON, autosave in localStorage.
- **Flash tab:** finds the FM-1 over Web MIDI, installs `.fwsc` packages, returns to official V15, resumes an interrupted write. This is Felucca's installer code and has **not been tested against hardware** by this project.

## Run it

```sh
cd web
npm install
npm run dev      # http://localhost:5173
npm test         # engine and patch-model tests
npm run build
```

Flashing needs Chrome or Edge (Web MIDI with SysEx) over https or localhost.

## Flashing risk

The FM-1 has one firmware slot and no proven rollback. Felucca's loader can normally finish an interrupted write and the official V15 file restores stock, but treat every flash as able to brick the unit.

## Layout

| Path | What |
| --- | --- |
| `web/` | Vite + TypeScript app: editor, engine, flasher UI |
| `web/src/flasher/` | USB-MIDI update protocol and package code, from Felucca |
| `firmware/hal/`, `firmware/loader/`, `firmware/crt0.S`, `firmware/app.ld` | Hardware layer and update loader from Felucca, unmodified |
| `tools/` | Felucca's package maker, installer and build script, unmodified |
| `docs/` | Architecture and plan |

## License

GPL-3.0-only. The flasher, loader, hardware layer and tools are copied from [Felucca](https://github.com/hugelton/Felucca) by Leo Kuroshita / Hügelton Instruments (GPL-3.0-only); see [LICENSING.md](LICENSING.md). Not affiliated with M-VAVE or Clavia.
