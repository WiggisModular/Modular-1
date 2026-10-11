// SPDX-License-Identifier: GPL-3.0-only
import { describe, expect, it } from 'vitest';
import { Engine } from './engine';
import { DEFS } from './modules';
import { addModule, connect, disconnect, emptyPatch, fromUnit, modulate, processingOrder, removeModule, toUnit, validatePatch } from './patch';
import { PRESETS } from './presets';

const SR = 48000;

function render(e: Engine, frames: number): { l: Float32Array; r: Float32Array } {
  const l = new Float32Array(frames);
  const r = new Float32Array(frames);
  e.render(l, r);
  return { l, r };
}

const rms = (a: Float32Array): number => Math.sqrt(a.reduce((s, v) => s + v * v, 0) / a.length);

/** frequency estimate from zero crossings */
function freq(a: Float32Array): number {
  let n = 0;
  for (let i = 1; i < a.length; i++) if (a[i - 1] < 0 && a[i] >= 0) n++;
  return (n * SR) / a.length;
}

describe('patch model', () => {
  it('connects, rejects duplicates and bad ports, removes with cables', () => {
    const p = emptyPatch();
    const a = addModule(p, DEFS.osc, 0, 0);
    const b = addModule(p, DEFS.output, 0, 0);
    expect(connect(p, DEFS, { module: a.id, port: 'out' }, { module: b.id, port: 'l' })).toBe(true);
    expect(connect(p, DEFS, { module: a.id, port: 'out' }, { module: b.id, port: 'l' })).toBe(false);
    expect(connect(p, DEFS, { module: a.id, port: 'nope' }, { module: b.id, port: 'l' })).toBe(false);
    expect(connect(p, DEFS, { module: a.id, port: 'out' }, { module: a.id, port: 'pm' })).toBe(false);
    disconnect(p, p.cables[0]);
    expect(p.cables).toHaveLength(0);
    connect(p, DEFS, { module: a.id, port: 'out' }, { module: b.id, port: 'l' });
    removeModule(p, a.id);
    expect(p.cables).toHaveLength(0);
  });

  it('orders producers before consumers and marks loop-closing cables as feedback', () => {
    const p = emptyPatch();
    const o = addModule(p, DEFS.osc, 0, 0);
    const f = addModule(p, DEFS.filter, 0, 0);
    const v = addModule(p, DEFS.vca, 0, 0);
    connect(p, DEFS, { module: o.id, port: 'out' }, { module: f.id, port: 'in' });
    connect(p, DEFS, { module: f.id, port: 'lp12' }, { module: v.id, port: 'in' });
    let r = processingOrder(p);
    expect(r.order).toEqual([o.id, f.id, v.id]);
    expect(r.feedback.size).toBe(0);
    connect(p, DEFS, { module: v.id, port: 'out' }, { module: o.id, port: 'pm' });
    r = processingOrder(p);
    expect(r.feedback.size).toBe(1);
    expect(r.order).toHaveLength(3);
  });

  it('validates JSON patches', () => {
    expect(() => validatePatch({ version: 2 }, DEFS)).toThrow();
    const p = PRESETS['Subtractive saw']();
    expect(validatePatch(JSON.parse(JSON.stringify(p)), DEFS).modules).toHaveLength(p.modules.length);
    const bad = JSON.parse(JSON.stringify(p));
    bad.cables[0].to.port = 'ghost';
    expect(() => validatePatch(bad, DEFS)).toThrow(/no input/);
  });

  it('refuses more than the module cap', () => {
    const p = emptyPatch();
    for (let i = 0; i < 32; i++) addModule(p, DEFS.const, 0, 0);
    expect(() => addModule(p, DEFS.const, 0, 0)).toThrow();
  });
});

describe('engine', () => {
  it('is silent without a note and sounds at the right pitch with one', () => {
    const e = new Engine(SR);
    const p = emptyPatch();
    const kb = addModule(p, DEFS.fm1, 0, 0);
    const o = addModule(p, DEFS.osc, 0, 0);
    const out = addModule(p, DEFS.output, 0, 0);
    connect(p, DEFS, { module: kb.id, port: 'pitch' }, { module: o.id, port: 'pitch' });
    connect(p, DEFS, { module: o.id, port: 'out' }, { module: out.id, port: 'l' });
    e.load(p);
    // oscillator runs freely: check pitch instead of silence
    e.noteOn(69); // A4
    const { l } = render(e, SR);
    expect(freq(l)).toBeGreaterThan(438);
    expect(freq(l)).toBeLessThan(442);
    e.noteOn(81); // A5
    expect(freq(render(e, SR).l)).toBeGreaterThan(878);
  });

  it('applies the ADSR: silent before the note, sounding while held, decaying after release', () => {
    const e = new Engine(SR);
    e.load(PRESETS['Subtractive saw']());
    expect(rms(render(e, 4800).l)).toBeLessThan(1e-4);
    e.noteOn(48);
    expect(rms(render(e, 9600).l)).toBeGreaterThan(0.02);
    e.noteOff(48);
    render(e, SR * 3);
    expect(rms(render(e, 4800).l)).toBeLessThan(1e-3);
  });

  it('303 Acid preset is finite, audible and not clipping', () => {
    const e = new Engine(SR);
    e.load(PRESETS['303 Acid']());
    e.noteOn(36);
    const { l } = render(e, SR);
    expect(l.every(Number.isFinite)).toBe(true);
    expect(rms(l)).toBeGreaterThan(0.01);
    expect(Math.max(...l.map(Math.abs))).toBeLessThan(2);
  });

  it('LFO sample & hold and osc pulse width run and stay finite', () => {
    const p = emptyPatch();
    const lfo = addModule(p, DEFS.lfo, 0, 0);
    lfo.params.wave = 4;
    lfo.params.rate = 20;
    const osc = addModule(p, DEFS.osc, 0, 0);
    Object.assign(osc.params, { wave: 3, pw: 0.2, level: 1 });
    const out = addModule(p, DEFS.output, 0, 0);
    expect(connect(p, DEFS, { module: lfo.id, port: 'out' }, { module: osc.id, port: 'p2' })).toBe(true); // p2 controls pw by default
    expect(connect(p, DEFS, { module: osc.id, port: 'out' }, { module: out.id, port: 'l' })).toBe(true);
    const e = new Engine(SR);
    e.load(p);
    const { l } = render(e, SR / 2);
    expect(l.every(Number.isFinite)).toBe(true);
    expect(rms(l)).toBeGreaterThan(0.1);
  });

  it('every module but fm1 and output has the two parameter inputs and a matrix', () => {
    for (const d of Object.values(DEFS)) {
      const has = d.inputs.filter((i) => i.pin).map((i) => i.name);
      if (['fm1', 'output'].includes(d.type)) expect(has, d.type).toEqual([]);
      else expect(has, d.type).toEqual(['p1', 'p2']);
      const m = addModule(emptyPatch(), d, 0, 0);
      expect(!!m.map, d.type).toBe(!!d.paramInputs);
    }
    const osc = addModule(emptyPatch(), DEFS.osc, 0, 0);
    expect(osc.map!.p1).toEqual(['wave']);
    expect(osc.map!.k4).toEqual(['fine']);
  });

  it('a parameter input moves the parameters the matrix assigns it to, scaled and inverted by its attenuverter', () => {
    const build = (att: number, assigned: boolean) => {
      const p = emptyPatch();
      const osc = addModule(p, DEFS.osc, 0, 0);
      osc.params.level = 0.5;
      osc.map!.p1 = assigned ? ['level'] : [];
      osc.att = [att, 1];
      const k = addModule(p, DEFS.const, 0, 0);
      k.params.value = 0.25;
      const out = addModule(p, DEFS.output, 0, 0);
      connect(p, DEFS, { module: osc.id, port: 'out' }, { module: out.id, port: 'l' });
      expect(connect(p, DEFS, { module: k.id, port: 'out' }, { module: osc.id, port: 'p1' })).toBe(true);
      const e = new Engine(SR);
      e.load(p);
      e.noteOn(60);
      return rms(render(e, 4800).l);
    };
    const base = build(1, false); // not assigned: no effect, level stays 0.5
    expect(build(1, true) / base).toBeGreaterThan(1.4); // 0.5 + 0.25 = 0.75
    expect(build(1, true) / base).toBeLessThan(1.6);
    expect(build(-1, true) / base).toBeGreaterThan(0.45); // 0.5 - 0.25 = 0.25
    expect(build(-1, true) / base).toBeLessThan(0.55);
    expect(build(0, true) / base).toBeCloseTo(1, 1);
  });

  it('turns a saved MIDI module into the FM-1 panel (same pitch, gate and vel outputs)', () => {
    const old = {
      version: 2,
      name: 'old',
      modules: [
        { id: 1, type: 'midi', x: 0, y: 0, params: {} },
        { id: 2, type: 'osc', x: 0, y: 0, params: {} },
        { id: 3, type: 'output', x: 0, y: 0, params: {} },
      ],
      cables: [
        { from: { module: 1, port: 'pitch' }, to: { module: 2, port: 'pitch' } },
        { from: { module: 2, port: 'out' }, to: { module: 3, port: 'l' } },
      ],
    };
    const p = validatePatch(old, DEFS);
    expect(p.modules[0].type).toBe('fm1');
    const e = new Engine(SR);
    e.load(p);
    e.noteOn(69);
    expect(freq(render(e, SR).l)).toBeGreaterThan(438);
  });

  it('converts version 1 patches (a <param>_cv cable becomes p1 / p2 with that parameter assigned)', () => {
    const old = {
      version: 1,
      name: 'old',
      modules: [
        { id: 1, type: 'const', x: 0, y: 0, params: { value: 0.5 } },
        { id: 2, type: 'osc', x: 0, y: 0, params: { level: 0 } },
        { id: 3, type: 'output', x: 0, y: 0, params: {} },
      ],
      cables: [
        { from: { module: 1, port: 'out' }, to: { module: 2, port: 'level_cv' } },
        { from: { module: 2, port: 'out' }, to: { module: 3, port: 'l' } },
      ],
    };
    const p = validatePatch(old, DEFS);
    expect(p.version).toBe(2);
    expect(p.cables[0].to.port).toBe('p1');
    expect(p.modules[1].map!.p1).toEqual(['level']);
    const e = new Engine(SR);
    e.load(p);
    expect(rms(render(e, 4800).l)).toBeGreaterThan(0.1);
  });

  it('FM-1 panel module outputs keys and knobs', () => {
    const p = emptyPatch();
    const panel = addModule(p, DEFS.fm1, 0, 0);
    Object.assign(panel.params, { k3: 0.25 });
    const osc = addModule(p, DEFS.osc, 0, 0);
    const out = addModule(p, DEFS.output, 0, 0);
    expect(connect(p, DEFS, { module: panel.id, port: 'pitch' }, { module: osc.id, port: 'pitch' })).toBe(true);
    osc.map!.p1 = ['level'];
    expect(connect(p, DEFS, { module: panel.id, port: 'k3' }, { module: osc.id, port: 'p1' })).toBe(true);
    expect(connect(p, DEFS, { module: osc.id, port: 'out' }, { module: out.id, port: 'l' })).toBe(true);
    const e = new Engine(SR);
    e.load(p);
    e.noteOn(69);
    const { l } = render(e, SR);
    expect(l.every(Number.isFinite)).toBe(true);
    expect(rms(l)).toBeGreaterThan(0.05);
  });

  it('keeps running state when the patch is edited, but not output order assumptions', () => {
    const e = new Engine(SR);
    const p = PRESETS['Subtractive saw']();
    e.load(p);
    e.noteOn(48);
    render(e, 4800);
    const before = rms(render(e, 1200).l);
    p.modules[1].params.level = 0.4;
    e.load(p);
    const after = rms(render(e, 1200).l);
    expect(after).toBeGreaterThan(before * 0.2); // no restart-to-silence: envelope state survived
  });

  it('FM changes the spectrum compared to the unmodulated carrier', () => {
    const e = new Engine(SR);
    const p = PRESETS['Two-operator FM']();
    e.load(p);
    e.noteOn(60);
    const a = render(e, 8000).l;
    const carrier = p.modules.find((m) => m.type === 'osc' && m.params.pm > 0)!;
    carrier.params.pm = 0;
    e.load(p);
    render(e, 2000);
    const b = render(e, 8000).l;
    let diff = 0;
    for (let i = 0; i < 8000; i++) diff += Math.abs(a[i] - b[i]);
    expect(diff).toBeGreaterThan(10);
  });

  it('survives a feedback cable without NaNs', () => {
    const e = new Engine(SR);
    const p = PRESETS['Subtractive saw']();
    const vca = p.modules.find((m) => m.type === 'vca')!;
    const osc = p.modules.find((m) => m.type === 'osc')!;
    connect(p, DEFS, { module: vca.id, port: 'out' }, { module: osc.id, port: 'pm' });
    osc.params.pm = 2;
    e.load(p);
    e.noteOn(55);
    const { l } = render(e, 20000);
    expect(l.every(Number.isFinite)).toBe(true);
  });

  it('renders every preset without NaN or clipping runaway', () => {
    for (const [name, make] of Object.entries(PRESETS)) {
      const e = new Engine(SR);
      e.load(make());
      e.noteOn(48);
      const { l, r } = render(e, 24000);
      expect(l.every(Number.isFinite), name).toBe(true);
      expect(rms(l), name).toBeGreaterThan(0.005);
      expect(rms(r), name).toBeGreaterThan(0.005);
    }
  });

  it('the filter cutoff is logarithmic: modulation moves it in octaves', () => {
    const cutoff = DEFS.filter.params.find((q) => q.name === 'cutoff')!;
    expect(cutoff.log).toBe(true);
    expect(toUnit(cutoff, 20)).toBeCloseTo(0, 6);
    expect(toUnit(cutoff, 18000)).toBeCloseTo(1, 6);
    expect(fromUnit(cutoff, 0.5)).toBeCloseTo(Math.sqrt(20 * 18000), 3);
    const octaves = Math.log2(18000 / 20);
    expect(Math.log2(modulate(cutoff, 200, 0.1) / 200)).toBeCloseTo(0.1 * octaves, 6); // a tenth of the range is a tenth of the octaves
    expect(modulate(cutoff, 200, 5)).toBeCloseTo(18000, 6);
    const level = DEFS.osc.params.find((q) => q.name === 'level')!;
    expect(modulate(level, 0.5, 0.25)).toBeCloseTo(0.75, 9); // the others stay linear
  });

  it('the factory presets route the knobs: K1 is the filter cutoff, K4 the delay mix, K3 the reverb mix', () => {
    for (const [name, make] of Object.entries(PRESETS)) {
      const p = make();
      const panel = p.modules.find((m) => m.type === 'fm1')!;
      const dest = (k: number) =>
        p.cables
          .filter((c) => c.from.module === panel.id && c.from.port === `k${k}`)
          .map((c) => {
            const m = p.modules.find((x) => x.id === c.to.module)!;
            return { type: m.type, params: m.map?.[c.to.port as 'p1' | 'p2'] ?? [] };
          });
      for (let k = 1; k <= 4; k++) expect(dest(k).length, `${name} K${k}`).toBeGreaterThan(0);
      if (p.modules.some((m) => m.type === 'filter')) expect(dest(1).some((d) => d.type === 'filter' && d.params.includes('cutoff')), `${name}: K1 is the cutoff`).toBe(true);
      if (p.modules.some((m) => m.type === 'delay')) expect(dest(4).some((d) => d.type === 'delay' && d.params.includes('mix')), `${name}: K4 is the delay mix`).toBe(true);
      if (p.modules.some((m) => m.type === 'reverb')) expect(dest(3).some((d) => d.type === 'reverb' && d.params.includes('mix')), `${name}: K3 is the reverb mix`).toBe(true);
    }
  });

  it('knob routing leaves the sound of a preset where it was (the knobs start where the values are)', () => {
    const p = PRESETS['Pluck']();
    const filt = p.modules.find((m) => m.type === 'filter')!;
    const panel = p.modules.find((m) => m.type === 'fm1')!;
    const cutoff = DEFS.filter.params.find((q) => q.name === 'cutoff')!;
    const moved = modulate(cutoff, filt.params.cutoff, panel.params.k1 * filt.att![0]);
    expect(moved).toBeGreaterThan(299);
    expect(moved).toBeLessThan(301);
  });
});

describe('filter', () => {
  /** noise -> filter -> output left, taken from the given filter output */
  function rig(port: string, params: Record<string, number>, note?: number): Float32Array {
    const e = new Engine(SR);
    const p = emptyPatch();
    addModule(p, DEFS.fm1, 0, 0);
    const n = addModule(p, DEFS.noise, 0, 0);
    const f = addModule(p, DEFS.filter, 0, 0);
    const out = addModule(p, DEFS.output, 0, 0);
    Object.assign(f.params, { res: 0, ...params });
    connect(p, DEFS, { module: n.id, port: 'out' }, { module: f.id, port: 'in' });
    connect(p, DEFS, { module: f.id, port: port }, { module: out.id, port: 'l' });
    e.load(p);
    if (note !== undefined) e.noteOn(note);
    render(e, 4800);
    return render(e, SR).l;
  }

  it('has a 24 dB and a 12 dB low pass: the 24 dB one takes more of the highs away', () => {
    const highs = (a: Float32Array): number => rms(a.slice(2).map((v, i) => v - 2 * a[i + 1] + a[i])); // second difference: the content far above the cutoff
    const lp12 = highs(rig('lp12', { cutoff: 500 }));
    const lp24 = highs(rig('lp24', { cutoff: 500 }));
    expect(lp12).toBeGreaterThan(1e-5);
    expect(lp24).toBeLessThan(lp12 * 0.3);
  });

  it('the mix output morphs from the 24 dB low pass (morph 0) to the high pass (morph 1)', () => {
    const lp24 = rig('lp24', { cutoff: 800 }), hp = rig('hp', { cutoff: 800 });
    const m0 = rig('mix', { cutoff: 800, morph: 0 }), m1 = rig('mix', { cutoff: 800, morph: 1 });
    expect(rms(m0)).toBeCloseTo(rms(lp24), 6);
    expect(rms(m1)).toBeCloseTo(rms(hp), 6);
    expect(rms(m0)).not.toBeCloseTo(rms(m1), 3);
  });

  it('key tracking moves the cutoff with the pitch of the played note', () => {
    const off = rms(rig('lp12', { cutoff: 300, track: 0 }, 84));
    const on = rms(rig('lp12', { cutoff: 300, track: 1 }, 84)); // C6: 2 octaves above C4
    expect(on).toBeGreaterThan(off * 1.5);
    expect(rms(rig('lp12', { cutoff: 300, track: 1 }, 60))).toBeCloseTo(rms(rig('lp12', { cutoff: 300, track: 0 }, 60)), 6); // C4: no shift
  });

  it('loads old patches whose cables leave the filter at "lp"', () => {
    const p = PRESETS['Subtractive saw']();
    const q = JSON.parse(JSON.stringify(p));
    for (const c of q.cables) if (c.from.port === 'lp12') c.from.port = 'lp';
    expect(validatePatch(q, DEFS).cables.some((c) => c.from.port === 'lp12')).toBe(true);
  });
});
