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

## JieLi SDK files (Apache-2.0)

Packages made by `tools/build.py` embed three files from the JieLi AC79 SDK (`uboot.boot`, `cfg_tool.bin`, `cfg/eq_cfg_hw.bin`), Apache-2.0. They are not in this repository. Ship `LICENSES/Apache-2.0.txt` with every package. The other texts in `LICENSES/` belong to Felucca's own bundled material (fonts, DSP ports) and apply only to files not copied here; they are kept so any later copy stays correctly attributed.
