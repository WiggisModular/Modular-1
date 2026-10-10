// SPDX-License-Identifier: GPL-3.0-only
// SysEx protocol between the editor and the Modular-1 firmware (firmware/src/proto.c). Pure functions.
//
//   F0 7D 4D 31 <cmd> <payload packed to 7-bit> F7
//
// 7D = non-commercial manufacturer id, 4D 31 = "M1". The payload is a byte string packed LSB first, 7 bits per
// wire byte (the same packing as Felucca's update protocol); n bytes take ceil(8 n / 7) wire bytes.
//
// host -> device
//   01 HELLO    -                                    device answers INFO
//   10 CHUNK    seq u8, offset u16, total u16, data  a piece of the binary patch (patchbin.ts), in order; ACK each
//   11 COMMIT   total u16, crc16 u16                 check the assembled patch and switch to it; ACK
//   12 STORE    -                                    write the current patch to flash; ACK
//   20 PARAM    module u8, param u8, value i32       live parameter change (Q16.16), no reply
//   21 ATT      module u8, input u8 (0 p1, 1 p2), value i32   live attenuverter change (Q16.16, -1..1), no reply
// device -> host
//   41 INFO     version, max modules, max cables, flags (bit0 flash ok, bit1 patch loaded), delay lines
//   40 ACK      command, seq, status, aux            status 0 ok, 1 bad offset/size, 2 CRC, 3 load error (aux = code),
//                                                    4 flash error, 5 unknown command, 6 nothing to store

export const PREFIX = [0x7d, 0x4d, 0x31];
export const CMD = { hello: 0x01, chunk: 0x10, commit: 0x11, store: 0x12, param: 0x20, att: 0x21, ack: 0x40, info: 0x41 } as const;
export const PROTO_VERSION = 1;
export const CHUNK_BYTES = 384;
export const STATUS_TEXT = ['ok', 'bad offset or size', 'checksum mismatch', 'the firmware refused the patch', 'flash error', 'unknown command', 'nothing to store'];

export function pack7(bytes: ArrayLike<number>): number[] {
  const out: number[] = [];
  let acc = 0;
  let nb = 0;
  for (let i = 0; i < bytes.length; i++) {
    acc |= (bytes[i] & 0xff) << nb;
    nb += 8;
    while (nb >= 7) {
      out.push(acc & 0x7f);
      acc >>>= 7;
      nb -= 7;
    }
  }
  if (nb > 0) out.push(acc & 0x7f);
  return out;
}

export function unpack7(septets: ArrayLike<number>): number[] {
  const out: number[] = [];
  let acc = 0;
  let nb = 0;
  for (let i = 0; i < septets.length; i++) {
    acc |= (septets[i] & 0x7f) << nb;
    nb += 7;
    if (nb >= 8) {
      out.push(acc & 0xff);
      acc >>>= 8;
      nb -= 8;
    }
  }
  return out;
}

/** CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) */
export function crc16(data: ArrayLike<number>): number {
  let c = 0xffff;
  for (let i = 0; i < data.length; i++) {
    c ^= (data[i] & 0xff) << 8;
    for (let k = 0; k < 8; k++) c = c & 0x8000 ? ((c << 1) ^ 0x1021) & 0xffff : (c << 1) & 0xffff;
  }
  return c;
}

export function frame(cmd: number, payload: ArrayLike<number> = []): Uint8Array {
  return Uint8Array.from([0xf0, ...PREFIX, cmd, ...pack7(payload), 0xf7]);
}

export interface Parsed {
  cmd: number;
  payload: number[];
}

/** A complete SysEx message (F0 .. F7) of ours, or null. */
export function parse(data: ArrayLike<number>): Parsed | null {
  const n = data.length;
  if (n < 6 || data[0] !== 0xf0 || data[n - 1] !== 0xf7) return null;
  for (let i = 0; i < 3; i++) if (data[1 + i] !== PREFIX[i]) return null;
  return { cmd: data[4], payload: unpack7(Array.from(data).slice(5, n - 1)) };
}

export const helloFrame = (): Uint8Array => frame(CMD.hello);
export const storeFrame = (): Uint8Array => frame(CMD.store);
export const commitFrame = (patch: Uint8Array): Uint8Array => {
  const crc = crc16(patch);
  return frame(CMD.commit, [patch.length & 0xff, patch.length >> 8, crc & 0xff, crc >> 8]);
};
export const chunkFrames = (patch: Uint8Array, size = CHUNK_BYTES): Uint8Array[] => {
  const out: Uint8Array[] = [];
  for (let off = 0, seq = 0; off < patch.length; off += size, seq = (seq + 1) & 0x7f) {
    const part = patch.subarray(off, off + size);
    out.push(frame(CMD.chunk, [seq, off & 0xff, off >> 8, patch.length & 0xff, patch.length >> 8, ...part]));
  }
  return out;
};
export const paramFrame = (module: number, param: number, q16: number): Uint8Array => {
  const v = q16 | 0;
  return frame(CMD.param, [module, param, v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, (v >>> 24) & 0xff]);
};

export const attFrame = (module: number, input: number, q16: number): Uint8Array => {
  const v = q16 | 0;
  return frame(CMD.att, [module, input, v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, (v >>> 24) & 0xff]);
};

export interface Info {
  version: number;
  maxModules: number;
  maxCables: number;
  flashOk: boolean;
  patchLoaded: boolean;
  delays: number;
}
export interface Ack {
  cmd: number;
  seq: number;
  status: number;
  aux: number;
}
export const parseInfo = (p: Parsed): Info | null =>
  p.cmd === CMD.info && p.payload.length >= 5
    ? { version: p.payload[0], maxModules: p.payload[1], maxCables: p.payload[2], flashOk: !!(p.payload[3] & 1), patchLoaded: !!(p.payload[3] & 2), delays: p.payload[4] }
    : null;
export const parseAck = (p: Parsed): Ack | null => (p.cmd === CMD.ack && p.payload.length >= 4 ? { cmd: p.payload[0], seq: p.payload[1], status: p.payload[2], aux: p.payload[3] } : null);
