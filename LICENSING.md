# Licensing

Modular-1 is GPL-3.0-only (`LICENSE`).

## Copied from Felucca

Source: <https://github.com/hugelton/Felucca> at commit `a4d7ca2e5d52a4c35e2468401f00d883cadbda03`.
Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments. GPL-3.0-only.

Copied files, to be changed only as part of Modular-1's own work:

- `firmware/hal/`, `firmware/loader/`, `firmware/crt0.S`, `firmware/app.ld`
- `tools/build.py`, `tools/fm1_install.py`, `tools/fm1pkg_make.py`, `tools/lz4blk.py`, `tools/get_toolchain.sh`
- `web/src/flasher/fm1ota.js`, `web/src/flasher/fm1pkg.js`

Every one of these keeps its original `SPDX-License-Identifier` and copyright header.

## fm1-emu (MIT)

The Simulator tab runs the firmware on the FM-1 emulator by Joakim Eriksson, MIT: <https://github.com/joakimeriksson/fm1-emu>. Its web build (the page, its scripts and `fm1_web.wasm`) is copied unchanged to `web/public/sim/` by `tools/build_simulator.sh` (`web/scripts/sync-simulator.mjs`), together with its licence (`LICENSE-fm1-emu-MIT.txt`) and `SOURCE.txt` naming the commit it was built from; only `firmwares.json` there is ours. MIT is compatible with this repository's GPL-3.0-only; the emulator's own licence stays with its files.

## Beiruti (SIL Open Font License 1.1)

The larger text on the display (`text_mid`) is the Beiruti typeface, Copyright 2024 The Beiruti Project Authors (<https://github.com/googlefonts/beiruti>), SIL OFL 1.1 (`LICENSES/OFL-1.1-Beiruti.txt`). The font file itself is not in this repository: `tools/gen_font_mid.py` renders it at a fixed size and weight (Bold, 14 px) into anti-aliased bitmaps, `firmware/src/font_mid.c` (generated, carries the notice), which are compiled into the firmware. Those bitmaps are a modified version of the font software under the OFL; the font is not sold on its own, the licence text ships with the repository, and the name is not used for anything else.

## JieLi SDK files (Apache-2.0)

Packages made by `tools/build.py` embed three files from the JieLi AC79 SDK (`uboot.boot`, `cfg_tool.bin`, `cfg/eq_cfg_hw.bin`), Apache-2.0. They are not in this repository as files, but the prebuilt package that the web app bundles (`web/public/firmware/modular-1.fwsc`, made by `tools/build.py` and `npm run firmware`, published on GitHub Pages) contains them, so the licence texts (`LICENSE-GPL-3.0.txt`, `LICENSE-Apache-2.0.txt`) are published next to it and linked from the Flash tab. Ship `LICENSES/Apache-2.0.txt` with every package. The other texts in `LICENSES/` belong to Felucca's own bundled material (fonts, DSP ports) and apply only to files not copied here; they are kept so any later copy stays correctly attributed.
