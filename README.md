# Modular-1

**Wake up honey a new FM-1 FW just dropped!**

100% vibecoded FM-1 FW that turns your favorite Chinese synth into a modular synth.

In the web app you build your own patch and upload it (up to 32 presets) to your FM-1. The same app flashes the firmware.

It's even more Alpha than Andrew Taint at the moment: there's an arpeggiator and a sequencer page, but no polyphony. Try it out.

## Use

- Web app: <https://morgulbrut.github.io/Modular-1/> (Chrome or Edge, MIDI access needed)
- Run it yourself: `cd web && npm install && npm run dev`
- Build the firmware: `nix develop`, then `python3 tools/build.py`

## License

GPL-3.0-only. The flasher, loader, hardware layer and tools are copied from [Felucca](https://github.com/hugelton/Felucca) by Leo Kuroshita / Hügelton Instruments (GPL-3.0-only); see [LICENSING.md](LICENSING.md).
