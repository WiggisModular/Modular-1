#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 morgulbrut
"""Talk to the Modular-1 firmware over USB-MIDI SysEx (protocol: web/src/protocol.ts), without the browser.

  tools/m1_link.py info
  tools/m1_link.py upload PATCH.m1p       (binary patches: build/golden/*.m1p from tools/golden.sh)
  tools/m1_link.py store
  tools/m1_link.py param MODULE PARAM VALUE     (module / parameter index, value as a float)
  tools/m1_link.py note 60 [seconds]
Needs mido and python-rtmidi (the Nix dev shell has them).
"""
import sys
import time

try:
    import mido
except ImportError:
    sys.exit("needs mido and python-rtmidi")

PREFIX = [0x7D, 0x4D, 0x31]
STATUS = ["ok", "bad offset or size", "checksum mismatch", "the firmware refused the patch", "flash error",
          "unknown command", "nothing to store"]


def pack7(b):
    out, acc, nb = [], 0, 0
    for x in b:
        acc |= x << nb
        nb += 8
        while nb >= 7:
            out.append(acc & 0x7F)
            acc >>= 7
            nb -= 7
    if nb:
        out.append(acc & 0x7F)
    return out


def unpack7(s):
    out, acc, nb = [], 0, 0
    for x in s:
        acc |= (x & 0x7F) << nb
        nb += 7
        if nb >= 8:
            out.append(acc & 0xFF)
            acc >>= 8
            nb -= 8
    return out


def crc16(data):
    c = 0xFFFF
    for x in data:
        c ^= x << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c


def frame(cmd, payload=()):
    return mido.Message("sysex", data=PREFIX + [cmd] + pack7(list(payload)))


class Link:
    def __init__(self):
        pick = lambda names: next((n for n in names if "Felucca" in n or "FM-1" in n), None)
        o, i = pick(mido.get_output_names()), pick(mido.get_input_names())
        if not o or not i:
            sys.exit("FM-1 not found")
        self.out, self.inp = mido.open_output(o), mido.open_input(i)

    def ask(self, msg, want, tries=3):
        for _ in range(tries):
            for _ in self.inp.iter_pending():
                pass
            self.out.send(msg)
            t = time.time()
            while time.time() - t < 0.8:
                for m in self.inp.iter_pending():
                    if m.type == "sysex" and list(m.data[:3]) == PREFIX and m.data[3] == want[0]:
                        p = unpack7(m.data[4:])
                        if want[0] == 0x41 or p[0] == want[1]:
                            return p
                time.sleep(0.002)
        sys.exit("no answer from the FM-1 (is it running the Modular-1 firmware with the editor protocol?)")

    def ack(self, msg, cmd):
        p = self.ask(msg, (0x40, cmd))
        if p[2]:
            sys.exit(f"FM-1: {STATUS[p[2]] if p[2] < len(STATUS) else p[2]} (aux {p[3]})")


def main():
    a = sys.argv[1:]
    if not a:
        sys.exit(__doc__)
    link = Link()
    if a[0] == "info":
        p = link.ask(frame(0x01), (0x41,))
        print(f"protocol {p[0]}, max modules {p[1]}, max cables {p[2]}, flash {'ok' if p[3] & 1 else 'no'}, "
              f"patch {'loaded' if p[3] & 2 else 'none'}, delays {p[4]}")
    elif a[0] == "upload":
        data = open(a[1], "rb").read()
        for seq, off in enumerate(range(0, len(data), 384)):
            part = data[off:off + 384]
            link.ack(frame(0x10, [seq & 0x7F, off & 0xFF, off >> 8, len(data) & 0xFF, len(data) >> 8, *part]), 0x10)
        c = crc16(data)
        link.ack(frame(0x11, [len(data) & 0xFF, len(data) >> 8, c & 0xFF, c >> 8]), 0x11)
        print(f"uploaded {len(data)} bytes")
    elif a[0] == "store":
        link.ack(frame(0x12), 0x12)
        print("stored")
    elif a[0] == "param":
        v = int(round(float(a[3]) * 65536)) & 0xFFFFFFFF
        link.out.send(frame(0x20, [int(a[1]), int(a[2]), v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, v >> 24]))
    elif a[0] == "note":
        link.out.send(mido.Message("note_on", note=int(a[1]), velocity=100))
        time.sleep(float(a[2]) if len(a) > 2 else 1.0)
        link.out.send(mido.Message("note_off", note=int(a[1])))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
