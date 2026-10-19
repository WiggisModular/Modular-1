#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Build the FM-1 emulator for the web app's Simulator tab and copy it to web/public/sim/ (committed, so GitHub Pages needs no Rust).
#   tools/build_simulator.sh [PATH_TO_FM1_EMU_CHECKOUT]   (default: ~/fm1-emu, cloned from github.com/joakimeriksson/fm1-emu if missing)
# Needs rust (cargo) with the wasm32-unknown-unknown target (rustup target add wasm32-unknown-unknown) and node.
set -e
cd "$(dirname "$0")/.."
EMU="${1:-$HOME/fm1-emu}"
[ -d "$EMU/.git" ] || git clone --depth 1 https://github.com/joakimeriksson/fm1-emu "$EMU"
DIST="$(mktemp -d)"
"$EMU/web/build.sh" "$DIST"
(cd web && node scripts/sync-simulator.mjs "$DIST" "$EMU")
rm -rf "$DIST"
