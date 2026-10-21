// SPDX-License-Identifier: GPL-3.0-only
import { describe, expect, it } from 'vitest';
import { DEFS } from './modules';
import { encodePatch, MAX_CABLES, TYPE_ORDER } from './patchbin';
import { make, PRESETS } from './presets';

describe('binary patch format', () => {
  it('encodes every preset with the documented layout', () => {
    for (const [name, make] of Object.entries(PRESETS)) {
      const p = make();
      const b = encodePatch(p);
      const dv = new DataView(b.buffer);
      expect(b[0], name).toBe(0xa1);
      expect(b[1], name).toBe(8);
      expect(b[2], name).toBe(p.modules.length);
      expect(dv.getUint16(4, true), name).toBe(p.cables.length);
      const size = 8 + p.modules.reduce((s, m) => s + 1 + 4 * DEFS[m.type].params.length + (DEFS[m.type].paramInputs ? 14 : 0), 0) + 4 * p.cables.length;
      expect(b.length, name).toBe(size);
      expect(p.cables.length).toBeLessThanOrEqual(MAX_CABLES);
    }
  });

  it('writes parameters as Q16.16 and cables as indices', () => {
    const p = make('saw', (add, wire) => {
      const kb = add('fm1', 0, 0);
      const o = add('osc', 0, 0, { wave: 2, level: 0.8 });
      const out = add('output', 0, 0);
      wire(kb, 'pitch', o, 'pitch');
      wire(o, 'out', out, 'l');
    });
    const b = encodePatch(p);
    // module 0 is the FM-1 panel (type byte + 4 parameters); module 1 osc starts right after
    const osc = 8 + 1 + 4 * DEFS.fm1.params.length;
    expect(b[8]).toBe(TYPE_ORDER.indexOf('fm1'));
    expect(b[osc]).toBe(TYPE_ORDER.indexOf('osc'));
    const dv = new DataView(b.buffer);
    expect(dv.getInt32(osc + 1, true)).toBe(2 * 65536); // wave = saw (2)
    const att = osc + 1 + 4 * DEFS.osc.params.length; // osc: att1, att2, then the masks p1 p2 k1 k2 k3 k4
    expect(dv.getInt32(att, true)).toBe(65536);
    expect(dv.getInt32(att + 4, true)).toBe(65536);
    expect(Array.from(b.slice(att + 8, att + 14))).toEqual([1, 2, 1, 2, 4, 8]); // the default matrix
    const cab = b.length - 4 * p.cables.length;
    expect([b[cab], b[cab + 1], b[cab + 2], b[cab + 3]]).toEqual([0, DEFS.fm1.outputs.findIndex((o) => o.name === 'pitch'), 1, DEFS.osc.inputs.findIndex((i) => i.name === 'pitch')]);
  });
});
