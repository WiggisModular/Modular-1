// SPDX-License-Identifier: GPL-3.0-only
import { describe, expect, it } from 'vitest';
import { chunkFrames, commitFrame, crc16, frame, pack7, paramFrame, parse, parseSlot, slotEraseFrame, slotFrame, slotInfoFrame, unpack7, CMD } from './protocol';

describe('SysEx protocol', () => {
  it('packs and unpacks every length without loss and with 7-bit bytes only', () => {
    for (let n = 0; n < 40; n++) {
      const b = Array.from({ length: n }, (_, i) => (i * 37 + 200) & 0xff);
      const p = pack7(b);
      expect(p.every((x) => x < 0x80)).toBe(true);
      expect(p.length).toBe(Math.ceil((8 * n) / 7));
      expect(unpack7(p)).toEqual(b);
    }
  });

  it('crc16 matches the CCITT-FALSE check value', () => {
    expect(crc16(Array.from('123456789', (c) => c.charCodeAt(0)))).toBe(0x29b1);
  });

  it('frames parse back and carry only 7-bit bytes between F0 and F7', () => {
    const f = frame(CMD.param, [1, 2, 255, 0, 128, 7]);
    expect(f[0]).toBe(0xf0);
    expect(f[f.length - 1]).toBe(0xf7);
    expect(Array.from(f.slice(1, -1)).every((x) => x < 0x80)).toBe(true);
    expect(parse(f)).toEqual({ cmd: CMD.param, payload: [1, 2, 255, 0, 128, 7] });
    expect(parse(Uint8Array.from([0xf0, 0x7d, 0x00, 0xf7]))).toBeNull();
  });

  it('splits a patch into in-order chunks that fit the firmware frame buffer (640 bytes)', () => {
    const patch = Uint8Array.from({ length: 1500 }, (_, i) => i & 0xff);
    const fr = chunkFrames(patch);
    expect(fr.length).toBe(4);
    let got: number[] = [];
    for (const f of fr) {
      expect(f.length - 2).toBeLessThanOrEqual(640);
      const p = parse(f)!;
      expect(p.cmd).toBe(CMD.chunk);
      expect(p.payload[1] | (p.payload[2] << 8)).toBe(got.length);
      expect(p.payload[3] | (p.payload[4] << 8)).toBe(1500);
      got = got.concat(p.payload.slice(5));
    }
    expect(got).toEqual(Array.from(patch));
    expect(parse(commitFrame(patch))!.payload).toEqual([1500 & 0xff, 1500 >> 8, crc16(patch) & 0xff, crc16(patch) >> 8]);
  });

  it('encodes parameter values as signed Q16.16 little endian', () => {
    expect(parse(paramFrame(3, 1, -65536))!.payload).toEqual([3, 1, 0, 0, 0xff, 0xff]);
  });

  it('builds the preset bank commands', () => {
    const patch = Uint8Array.from({ length: 200 }, (_, i) => i);
    const p = parse(slotFrame(5, '303 Acid', patch))!;
    expect(p.cmd).toBe(CMD.slot);
    expect(p.payload[0]).toBe(5);
    expect(String.fromCharCode(...p.payload.slice(1, 9))).toBe('303 Acid');
    expect(p.payload.slice(9, 21).every((x) => x === 0)).toBe(true); // zero padded to 20 bytes
    expect(p.payload.slice(21)).toEqual([200, 0, crc16(patch) & 0xff, crc16(patch) >> 8]);
    expect(parse(slotEraseFrame(7))!.payload).toEqual([7]);
    expect(parse(slotInfoFrame(31))!.payload).toEqual([31]);
    const reply = frame(CMD.slotReply, [5, 1, 200, 0, ...Array.from('KICK', (c) => c.charCodeAt(0)), ...new Array(16).fill(0)]);
    expect(parseSlot(parse(reply)!)).toEqual({ slot: 5, used: true, length: 200, name: 'KICK' });
  });
});
