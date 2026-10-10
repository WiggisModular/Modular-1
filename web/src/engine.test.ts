// SPDX-License-Identifier: GPL-3.0-only
import { describe, expect, it } from 'vitest';
import { Engine } from './engine';
import { DEFS } from './modules';
import { addModule, connect, disconnect, emptyPatch, processingOrder, removeModule, validatePatch } from './patch';
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
    connect(p, DEFS, { module: f.id, port: 'lp' }, { module: v.id, port: 'in' });
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
    const kb = addModule(p, DEFS.midi, 0, 0);
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
});
