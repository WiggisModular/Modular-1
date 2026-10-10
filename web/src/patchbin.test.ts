// SPDX-License-Identifier: GPL-3.0-only
import { describe, expect, it } from 'vitest';
import { DEFS } from './modules';
import { encodePatch, MAX_CABLES, TYPE_ORDER } from './patchbin';
import { PRESETS } from './presets';

describe('binary patch format', () => {
  it('encodes every preset with the documented layout', () => {
    for (const [name, make] of Object.entries(PRESETS)) {
      const p = make();
      const b = encodePatch(p);
      const dv = new DataView(b.buffer);
      expect(b[0], name).toBe(0xa1);
      expect(b[1], name).toBe(1);
      expect(b[2], name).toBe(p.modules.length);
      expect(dv.getUint16(4, true), name).toBe(p.cables.length);
      const size = 8 + p.modules.reduce((s, m) => s + 1 + 4 * DEFS[m.type].params.length, 0) + 4 * p.cables.length;
      expect(b.length, name).toBe(size);
      expect(p.cables.length).toBeLessThanOrEqual(MAX_CABLES);
    }
  });

  it('writes parameters as Q16.16 and cables as indices', () => {
    const p = PRESETS['Subtractive saw']();
    const b = encodePatch(p);
    // module 0 is midi (no params): type byte only; module 1 osc starts right after
    expect(b[8]).toBe(TYPE_ORDER.indexOf('midi'));
    expect(b[9]).toBe(TYPE_ORDER.indexOf('osc'));
    const dv = new DataView(b.buffer);
    expect(dv.getInt32(10, true)).toBe(2 * 65536); // wave = saw (2)
    const cab = b.length - 4 * p.cables.length;
    expect([b[cab], b[cab + 1], b[cab + 2], b[cab + 3]]).toEqual([0, DEFS.midi.outputs.findIndex((o) => o.name === 'pitch'), 1, DEFS.osc.inputs.findIndex((i) => i.name === 'pitch')]);
  });
});
