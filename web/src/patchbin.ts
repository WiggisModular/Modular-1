// SPDX-License-Identifier: GPL-3.0-only
// Compact binary patch format, read by firmware/src/engine.c (eng_load).
//
//   u8  magic 0xA1, u8 version 1, u8 module count, u8 reserved (0), u16 LE cable count, u16 reserved (0)
//   per module:  u8 type index, then one i32 LE per parameter (Q16.16), in the order of the definition
//   per cable:   u8 from module index, u8 from output index, u8 to module index, u8 to input index
//
// Type indices are the position in TYPE_ORDER (append only: the firmware has the same list in defs.h).
// Port indices are positions in ModuleDef.outputs / inputs (explicit ports first, then the `<param>_cv` inputs).

import { DEFS } from './modules';
import { MAX_MODULES, type Patch } from './patch';

export const TYPE_ORDER: string[] = Object.keys(DEFS);
export const MAX_CABLES = 192;
export const Q = 65536;

export const toQ16 = (v: number): number => Math.max(-0x80000000, Math.min(0x7fffffff, Math.round(v * Q)));

export function encodePatch(p: Patch): Uint8Array {
  if (p.modules.length > MAX_MODULES) throw new Error(`more than ${MAX_MODULES} modules`);
  if (p.cables.length > MAX_CABLES) throw new Error(`more than ${MAX_CABLES} cables`);
  const index = new Map(p.modules.map((m, i) => [m.id, i]));
  let size = 8;
  for (const m of p.modules) size += 1 + 4 * DEFS[m.type].params.length;
  size += 4 * p.cables.length;
  const out = new Uint8Array(size);
  const dv = new DataView(out.buffer);
  out[0] = 0xa1;
  out[1] = 1;
  out[2] = p.modules.length;
  dv.setUint16(4, p.cables.length, true);
  let at = 8;
  for (const m of p.modules) {
    const def = DEFS[m.type];
    out[at++] = TYPE_ORDER.indexOf(m.type);
    for (const pd of def.params) {
      dv.setInt32(at, toQ16(m.params[pd.name] ?? pd.default), true);
      at += 4;
    }
  }
  for (const c of p.cables) {
    const from = p.modules[index.get(c.from.module)!];
    const to = p.modules[index.get(c.to.module)!];
    out[at++] = index.get(c.from.module)!;
    out[at++] = DEFS[from.type].outputs.findIndex((o) => o.name === c.from.port);
    out[at++] = index.get(c.to.module)!;
    out[at++] = DEFS[to.type].inputs.findIndex((i) => i.name === c.to.port);
  }
  return out;
}
