#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Render the device LCD of a factory preset on the host: tools/ui_preview.sh OUT.png PRESET_INDEX [MODULE [OCTAVE]]
set -e
cd "$(dirname "$0")/.."
out="$1"; shift
mkdir -p build
gcc -O1 -Wno-unused-function -Wno-unused-result -o build/ui_preview firmware/test/ui_preview.c
build/ui_preview build/ui_preview.ppm "$@"
python3 -I - "$out" <<'PY'
import sys, zlib, struct
d = open('build/ui_preview.ppm', 'rb').read()
w, h = 240, 240
px = d[len(d) - w * h * 3:]
scale = 3
raw = b''
for y in range(h):
    row = px[y * w * 3:(y + 1) * w * 3]
    big = b''.join(row[i:i + 3] * scale for i in range(0, len(row), 3))
    raw += (b'\x00' + big) * scale
def chunk(t, b): return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w * scale, h * scale, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b'')
open(sys.argv[1], 'wb').write(png)
PY
