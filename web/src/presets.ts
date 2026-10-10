// SPDX-License-Identifier: GPL-3.0-only
import { DEFS } from './modules';
import { addModule, connect, emptyPatch, type Patch, type Source } from './patch';

export interface AddOpts {
  map?: Partial<Record<Source, string[]>>;
  att?: [number, number];
}
export type Build = (add: (type: string, x: number, y: number, params?: Record<string, number>, opts?: AddOpts) => number, wire: (a: number, ap: string, b: number, bp: string) => void) => void;

export function make(name: string, build: Build): Patch {
  const p = emptyPatch(name);
  const add = (type: string, x: number, y: number, params: Record<string, number> = {}, opts: AddOpts = {}): number => {
    const m = addModule(p, DEFS[type], Math.round(x * 1.3), Math.round(y * 1.3)); // cards are wider and taller than they used to be
    Object.assign(m.params, params);
    if (opts.map) Object.assign(m.map!, opts.map);
    if (opts.att) m.att = opts.att;
    return m.id;
  };
  const wire = (a: number, ap: string, b: number, bp: string): void => {
    if (!connect(p, DEFS, { module: a, port: ap }, { module: b, port: bp })) throw new Error(`preset ${name}: bad cable ${a}.${ap} -> ${b}.${bp}`);
  };
  build(add, wire);
  return p;
}


/** layout of the generated patches: column, row */
const L = (col: number, row = 0): [number, number] => [20 + col * 210, 20 + row * 400];

type AddFn = Parameters<Build>[0];
type WireFn = Parameters<Build>[1];

/** an amplitude chain: `from` -> VCA, opened by an ADSR on the panel's gate. Returns the VCA. */
function voice(add: AddFn, wire: WireFn, kb: number, from: number, port: string, env: { a: number; d: number; s: number; r: number }, col: number, gain = 1): number {
  const e = add('adsr', ...L(col, 1), env);
  const v = add('vca', ...L(col), { gain });
  wire(kb, 'gate', e, 'gate');
  wire(from, port, v, 'in');
  wire(e, 'env', v, 'cv');
  return v;
}

function toOut(add: AddFn, wire: WireFn, from: number, port: string, level: number, col: number): number {
  const o = add('output', ...L(col), { level });
  wire(from, port, o, 'l');
  return o;
}

export const PRESETS: Record<string, () => Patch> = {
  '303 Acid': () =>
    make('303 Acid', (add, wire) => {
      const kb = add('fm1', 20, 40);
      const osc = add('osc', 220, 20, { wave: 2, coarse: -12, level: 0.9 });
      const fenv = add('adsr', 220, 430, { a: 0.001, d: 0.22, s: 0, r: 0.1 });
      const flt = add('filter', 440, 20, { cutoff: 180, res: 0.88, cvamt: 3.2 });
      const aenv = add('adsr', 440, 430, { a: 0.002, d: 0.15, s: 0.8, r: 0.07 });
      const vca = add('vca', 660, 20, { gain: 1.1 });
      const dly = add('delay', 660, 430, { time: 375, fb: 0.35, mix: 0.25 });
      const out = add('output', 880, 40);
      wire(kb, 'pitch', osc, 'pitch');
      wire(osc, 'out', flt, 'in');
      wire(kb, 'gate', fenv, 'gate');
      wire(fenv, 'env', flt, 'cv');
      wire(kb, 'gate', aenv, 'gate');
      wire(flt, 'lp', vca, 'in');
      wire(aenv, 'env', vca, 'cv');
      wire(vca, 'out', dly, 'in');
      wire(dly, 'out', out, 'l');
    }),
  'Subtractive saw': () =>
    make('Subtractive saw', (add, wire) => {
      const kb = add('fm1', 20, 40);
      const osc = add('osc', 220, 20, { wave: 2 });
      const env = add('adsr', 220, 430, { a: 0.01, d: 0.3, s: 0.5, r: 0.4 });
      const flt = add('filter', 440, 20, { cutoff: 600, res: 0.45, cvamt: 2 });
      const vca = add('vca', 660, 20);
      const out = add('output', 860, 40);
      wire(kb, 'pitch', osc, 'pitch');
      wire(osc, 'out', flt, 'in');
      wire(kb, 'gate', env, 'gate');
      wire(env, 'env', flt, 'cv');
      wire(flt, 'lp', vca, 'in');
      wire(env, 'env', vca, 'cv');
      wire(vca, 'out', out, 'l');
    }),
  'Two-operator FM': () =>
    make('Two-operator FM', (add, wire) => {
      const kb = add('fm1', 20, 120);
      const mod = add('osc', 220, 20, { wave: 0, coarse: 12, level: 1 });
      const modEnv = add('adsr', 220, 430, { a: 0.001, d: 0.5, s: 0.1, r: 0.3 });
      const modVca = add('vca', 440, 20, { gain: 1 });
      const car = add('osc', 660, 20, { wave: 0, pm: 3, level: 1 });
      const ampEnv = add('adsr', 660, 430, { a: 0.003, d: 0.8, s: 0.4, r: 0.5 });
      const amp = add('vca', 880, 20);
      const out = add('output', 1080, 40);
      wire(kb, 'pitch', mod, 'pitch');
      wire(kb, 'pitch', car, 'pitch');
      wire(kb, 'gate', modEnv, 'gate');
      wire(kb, 'gate', ampEnv, 'gate');
      wire(mod, 'out', modVca, 'in');
      wire(modEnv, 'env', modVca, 'cv');
      wire(modVca, 'out', car, 'pm');
      wire(car, 'out', amp, 'in');
      wire(ampEnv, 'env', amp, 'cv');
      wire(amp, 'out', out, 'l');
    }),
  'Wobble bass': () =>
    make('Wobble bass', (add, wire) => {
      const kb = add('fm1', 20, 40);
      const osc = add('osc', 220, 20, { wave: 3, coarse: -12 });
      const lfo = add('lfo', 220, 430, { rate: 3, wave: 0 });
      const flt = add('filter', 440, 20, { cutoff: 300, res: 0.6, cvamt: 1.5 });
      const env = add('adsr', 440, 430, { a: 0.005, d: 0.1, s: 0.9, r: 0.15 });
      const vca = add('vca', 660, 20);
      const out = add('output', 860, 40);
      wire(kb, 'pitch', osc, 'pitch');
      wire(osc, 'out', flt, 'in');
      wire(lfo, 'out', flt, 'cv');
      wire(kb, 'gate', env, 'gate');
      wire(flt, 'lp', vca, 'in');
      wire(env, 'env', vca, 'cv');
      wire(vca, 'out', out, 'l');
    }),
  'FM-1 Panel synth': () =>
    make('FM-1 Panel synth', (add, wire) => {
      // the FM-1's own keys and the four knobs: K1 cutoff, K2 resonance, K3 delay mix, K4 delay time
      const fm = add('fm1', 20, 40, { k1: 0.5, k2: 0.4, k3: 0.25, k4: 0.35 });
      const osc = add('osc', 260, 20, { wave: 2, level: 0.8 });
      const env = add('adsr', 260, 430, { a: 0.005, d: 0.25, s: 0.6, r: 0.25 });
      const flt = add('filter', 480, 20, { cutoff: 20, res: 0, cvamt: 0 }); // p1 = cutoff, p2 = resonance (the default matrix)
      const vca = add('vca', 700, 20);
      const dly = add('delay', 700, 430, { time: 1, fb: 0.4, mix: 0 }, { map: { p1: ['mix'], p2: ['time'] } });
      const out = add('output', 920, 40);
      wire(fm, 'pitch', osc, 'pitch');
      wire(fm, 'gate', env, 'gate');
      wire(osc, 'out', flt, 'in');
      wire(fm, 'k1', flt, 'p1');
      wire(fm, 'k2', flt, 'p2');
      wire(flt, 'lp', vca, 'in');
      wire(env, 'env', vca, 'cv');
      wire(vca, 'out', dly, 'in');
      wire(fm, 'k3', dly, 'p1');
      wire(fm, 'k4', dly, 'p2');
      wire(dly, 'out', out, 'l');
    }),

  // ---- the rest of the factory bank (32 in all). Layout: L(column, row); every patch plays from the FM-1 panel module.
  'Sub Bass': () =>
    make('Sub Bass', (add, wire) => {
      const kb = add('fm1', ...L(0));
      const sine = add('osc', ...L(1), { wave: 0, coarse: -12, level: 0.9 });
      const saw = add('osc', ...L(1, 1), { wave: 2, coarse: -24, level: 0.4 });
      const mix = add('mixer', ...L(2), { l1: 0.8, l2: 0.5 });
      const flt = add('filter', ...L(3), { cutoff: 420, res: 0.1 });
      const v = voice(add, wire, kb, flt, 'lp', { a: 0.005, d: 0.2, s: 0.8, r: 0.15 }, 4);
      wire(kb, 'pitch', sine, 'pitch');
      wire(kb, 'pitch', saw, 'pitch');
      wire(sine, 'out', mix, 'in1');
      wire(saw, 'out', mix, 'in2');
      wire(mix, 'out', flt, 'in');
      toOut(add, wire, v, 'out', 0.8, 5);
    }),
  Pluck: () =>
    make('Pluck', (add, wire) => {
      const kb = add('fm1', ...L(0));
      const osc = add('osc', ...L(1), { wave: 2, level: 0.8 });
      const fe = add('adsr', ...L(2, 1), { a: 0.001, d: 0.18, s: 0, r: 0.1 });
      const flt = add('filter', ...L(2), { cutoff: 300, res: 0.3, cvamt: 3 });
      const v = voice(add, wire, kb, flt, 'lp', { a: 0.001, d: 0.35, s: 0, r: 0.2 }, 3);
      const dly = add('delay', ...L(4), { time: 300, fb: 0.35, mix: 0.25 });
      wire(kb, 'pitch', osc, 'pitch');
      wire(kb, 'gate', fe, 'gate');
      wire(osc, 'out', flt, 'in');
      wire(fe, 'env', flt, 'cv');
      wire(v, 'out', dly, 'in');
      toOut(add, wire, dly, 'out', 0.7, 5);
    }),
  'Warm Pad': () =>
    make('Warm Pad', (add, wire) => {
      const kb = add('fm1', ...L(0));
      const a = add('osc', ...L(1), { wave: 2, fine: -8, level: 0.8 });
      const b = add('osc', ...L(1, 1), { wave: 2, fine: 8, level: 0.8 });
      const mix = add('mixer', ...L(2), { l1: 0.5, l2: 0.5 });
      const lfo = add('lfo', ...L(2, 1), { wave: 0, rate: 0.3, depth: 0.5 });
      const flt = add('filter', ...L(3), { cutoff: 900, res: 0.15, cvamt: 0.8 });
      const v = voice(add, wire, kb, flt, 'lp', { a: 1.2, d: 1, s: 0.8, r: 1.8 }, 4);
      const dly = add('delay', ...L(5), { time: 420, fb: 0.4, mix: 0.3 });
      wire(kb, 'pitch', a, 'pitch');
      wire(kb, 'pitch', b, 'pitch');
      wire(a, 'out', mix, 'in1');
      wire(b, 'out', mix, 'in2');
      wire(mix, 'out', flt, 'in');
      wire(lfo, 'out', flt, 'cv');
      wire(v, 'out', dly, 'in');
      toOut(add, wire, dly, 'out', 0.7, 6);
    }),
  'Square Lead': () =>
    make('Square Lead', (add, wire) => {
      const kb = add('fm1', ...L(0));
      const osc = add('osc', ...L(1), { wave: 3, pw: 0.5, level: 0.7 });
      const flt = add('filter', ...L(2), { cutoff: 3000, res: 0.25 });
      const v = voice(add, wire, kb, flt, 'lp', { a: 0.01, d: 0.2, s: 0.7, r: 0.2 }, 3);
      const dly = add('delay', ...L(5), { time: 250, fb: 0.3, mix: 0.2 });
      wire(kb, 'pitch', osc, 'pitch');
      wire(osc, 'out', flt, 'in');
      wire(v, 'out', dly, 'in');
      toOut(add, wire, dly, 'out', 0.7, 6);
    }),
  'PWM Pad': () =>
    make('PWM Pad', (add, wire) => {
      const kb = add('fm1', ...L(0));
      const osc = add('osc', ...L(1), { wave: 3, level: 0.6 });
      const lfo = add('lfo', ...L(1, 1), { wave: 0, rate: 0.5, depth: 1 });
      const v = voice(add, wire, kb, osc, 'out', { a: 0.6, d: 1, s: 0.9, r: 1.2 }, 2);
      wire(kb, 'pitch', osc, 'pitch');
      wire(lfo, 'out', osc, 'p2'); // p2 controls the pulse width by default
      toOut(add, wire, v, 'out', 0.7, 4);
    }),
  Wind: () =>
    make('Wind', (add, wire) => {
      const kb = add('fm1', ...L(0));
      const nz = add('noise', ...L(1), { level: 0.7 });
      const lfo = add('lfo', ...L(1, 1), { wave: 0, rate: 0.15, depth: 1 });
      const flt = add('filter', ...L(2), { cutoff: 500, res: 0.6, cvamt: 1.5 });
      const v = voice(add, wire, kb, flt, 'bp', { a: 1.5, d: 1, s: 1, r: 2 }, 3);
      const dly = add('delay', ...L(5), { time: 500, fb: 0.5, mix: 0.3 });
      wire(nz, 'out', flt, 'in');
      wire(lfo, 'out', flt, 'cv');
      wire(v, 'out', dly, 'in');
      toOut(add, wire, dly, 'out', 0.7, 6);
    }),
  'Hi-hat': () =>
    make('Hi-hat', (add, wire) => {
      const kb = add('fm1', ...L(0));
      const nz = add('noise', ...L(1), { level: 0.8 });
      const flt = add('filter', ...L(2), { cutoff: 6000, res: 0.2 });
      const v = voice(add, wire, kb, flt, 'hp', { a: 0.001, d: 0.06, s: 0, r: 0.05 }, 3);
      wire(nz, 'out', flt, 'in');
      toOut(add, wire, v, 'out', 0.7, 5);
    }),
  Snare: () =>
    make('Snare', (add, wire) => {
      const kb = add('fm1', ...L(0));
      const nz = add('noise', ...L(1), { level: 0.7 });
      const flt = add('filter', ...L(2), { cutoff: 2000, res: 0.3 });
      const body = add('osc', ...L(1, 1), { wave: 1, coarse: -5, level: 0.5 });
      const mix = add('mixer', ...L(3), { l1: 0.7, l2: 0.6 });
      const v = voice(add, wire, kb, mix, 'out', { a: 0.001, d: 0.15, s: 0, r: 0.1 }, 4);
      wire(kb, 'pitch', body, 'pitch');
      wire(nz, 'out', flt, 'in');
      wire(flt, 'bp', mix, 'in1');
      wire(body, 'out', mix, 'in2');
      toOut(add, wire, v, 'out', 0.8, 6);
    }),
  Kick: () =>
    make('Kick', (add, wire) => {
      const kb = add('fm1', ...L(0));
      const osc = add('osc', ...L(1), { wave: 0, coarse: -24, level: 1 });
      const pe = add('adsr', ...L(1, 1), { a: 0.001, d: 0.08, s: 0, r: 0.05 });
      const sc = add('scale', ...L(2, 1), { gain: 1.5 });
      const v = voice(add, wire, kb, osc, 'out', { a: 0.001, d: 0.3, s: 0, r: 0.1 }, 2);
      wire(kb, 'gate', pe, 'gate');
      wire(pe, 'env', sc, 'in');
      wire(sc, 'out', osc, 'pitch'); // a falling pitch sweep
      toOut(add, wire, v, 'out', 0.9, 4);
    }),
  Laser: () =>
    make('Laser', (add, wire) => {
      const kb = add('fm1', ...L(0));
      const osc = add('osc', ...L(1), { wave: 1, level: 0.7 });
      const pe = add('adsr', ...L(1, 1), { a: 0.001, d: 0.25, s: 0, r: 0.1 });
      const sc = add('scale', ...L(2, 1), { gain: 2 });
      const v = voice(add, wire, kb, osc, 'out', { a: 0.001, d: 0.3, s: 0, r: 0.1 }, 2);
      const dly = add('delay', ...L(4), { time: 180, fb: 0.4, mix: 0.3 });
      wire(kb, 'pitch', osc, 'pitch');
      wire(kb, 'gate', pe, 'gate');
      wire(pe, 'env', sc, 'in');
      wire(sc, 'out', osc, 'pitch');
      wire(v, 'out', dly, 'in');
      toOut(add, wire, dly, 'out', 0.7, 5);
    }),
  Siren: () =>
    make('Siren', (add, wire) => {
      const kb = add('fm1', ...L(0));
      const osc = add('osc', ...L(1), { wave: 0, level: 0.7 });
      const lfo = add('lfo', ...L(1, 1), { wave: 1, rate: 1.2, depth: 1 });
      const sc = add('scale', ...L(2, 1), { gain: 0.5 });
      const v = voice(add, wire, kb, osc, 'out', { a: 0.05, d: 0.1, s: 1, r: 0.3 }, 2);
      wire(kb, 'pitch', osc, 'pitch');
      wire(lfo, 'out', sc, 'in');
      wire(sc, 'out', osc, 'pitch');
      toOut(add, wire, v, 'out', 0.7, 4);
    }),
  'Vibrato Lead': () =>
    make('Vibrato Lead', (add, wire) => {
      const kb = add('fm1', ...L(0));
      const osc = add('osc', ...L(1), { wave: 2, level: 0.7 });
      const lfo = add('lfo', ...L(1, 1), { wave: 0, rate: 5.5, depth: 1 });
      const sc = add('scale', ...L(2, 1), { gain: 0.03 });
      const flt = add('filter', ...L(2), { cutoff: 2500, res: 0.2 });
      const v = voice(add, wire, kb, flt, 'lp', { a: 0.02, d: 0.3, s: 0.8, r: 0.25 }, 3);
      wire(kb, 'pitch', osc, 'pitch');
      wire(lfo, 'out', sc, 'in');
      wire(sc, 'out', osc, 'pitch');
      wire(osc, 'out', flt, 'in');
      toOut(add, wire, v, 'out', 0.7, 5);
    }),
  Organ: () =>
    make('Organ', (add, wire) => {
      const kb = add('fm1', ...L(0));
      const mix = add('mixer', ...L(2), { l1: 0.6, l2: 0.4, l3: 0.25 });
      [0, 12, 19].forEach((c, i) => {
        const o = add('osc', ...L(1, i), { wave: 0, coarse: c, level: 0.8 });
        wire(kb, 'pitch', o, 'pitch');
        wire(o, 'out', mix, `in${i + 1}`);
      });
      const v = voice(add, wire, kb, mix, 'out', { a: 0.01, d: 0.1, s: 1, r: 0.08 }, 3);
      toOut(add, wire, v, 'out', 0.7, 5);
    }),
  'FM Bell': () =>
    make('FM Bell', (add, wire) => {
      const kb = add('fm1', ...L(0));
      const mod = add('osc', ...L(1), { wave: 0, coarse: 19, level: 1 });
      const mv = voice(add, wire, kb, mod, 'out', { a: 0.001, d: 1.2, s: 0, r: 1 }, 2, 1);
      const car = add('osc', ...L(3), { wave: 0, pm: 2.5, level: 0.8 });
      const v = voice(add, wire, kb, car, 'out', { a: 0.001, d: 2, s: 0, r: 1.5 }, 4);
      const dly = add('delay', ...L(6), { time: 330, fb: 0.35, mix: 0.3 });
      wire(kb, 'pitch', mod, 'pitch');
      wire(kb, 'pitch', car, 'pitch');
      wire(mv, 'out', car, 'pm');
      wire(v, 'out', dly, 'in');
      toOut(add, wire, dly, 'out', 0.7, 7);
    }),
  'Electric Piano': () =>
    make('Electric Piano', (add, wire) => {
      const kb = add('fm1', ...L(0));
      const mod = add('osc', ...L(1), { wave: 0, coarse: 12, level: 1 });
      const mv = voice(add, wire, kb, mod, 'out', { a: 0.001, d: 0.4, s: 0.15, r: 0.3 }, 2, 1);
      const car = add('osc', ...L(3), { wave: 0, pm: 1.2, level: 0.8 });
      const v = voice(add, wire, kb, car, 'out', { a: 0.002, d: 1, s: 0.2, r: 0.4 }, 4);
      wire(kb, 'pitch', mod, 'pitch');
      wire(kb, 'pitch', car, 'pitch');
      wire(mv, 'out', car, 'pm');
      toOut(add, wire, v, 'out', 0.7, 6);
    }),
  Brass: () =>
    make('Brass', (add, wire) => {
      const kb = add('fm1', ...L(0));
      const a = add('osc', ...L(1), { wave: 2, fine: -6, level: 0.8 });
      const b = add('osc', ...L(1, 1), { wave: 2, fine: 6, level: 0.8 });
      const mix = add('mixer', ...L(2), { l1: 0.5, l2: 0.5 });
      const fe = add('adsr', ...L(2, 1), { a: 0.15, d: 0.3, s: 0.6, r: 0.2 });
      const flt = add('filter', ...L(3), { cutoff: 400, res: 0.1, cvamt: 2.2 });
      const v = voice(add, wire, kb, flt, 'lp', { a: 0.08, d: 0.2, s: 0.85, r: 0.2 }, 4);
      wire(kb, 'pitch', a, 'pitch');
      wire(kb, 'pitch', b, 'pitch');
      wire(kb, 'gate', fe, 'gate');
      wire(a, 'out', mix, 'in1');
      wire(b, 'out', mix, 'in2');
      wire(mix, 'out', flt, 'in');
      wire(fe, 'env', flt, 'cv');
      toOut(add, wire, v, 'out', 0.7, 5);
    }),
  Strings: () =>
    make('Strings', (add, wire) => {
      const kb = add('fm1', ...L(0));
      const mix = add('mixer', ...L(2), { l1: 0.4, l2: 0.4, l3: 0.4 });
      [-9, 0, 9].forEach((f, i) => {
        const o = add('osc', ...L(1, i), { wave: 2, fine: f, level: 0.8 });
        wire(kb, 'pitch', o, 'pitch');
        wire(o, 'out', mix, `in${i + 1}`);
      });
      const lfo = add('lfo', ...L(2, 1), { wave: 0, rate: 0.6, depth: 0.3 });
      const flt = add('filter', ...L(3), { cutoff: 2200, res: 0.1, cvamt: 0.4 });
      const v = voice(add, wire, kb, flt, 'lp', { a: 0.5, d: 0.5, s: 0.9, r: 1 }, 4);
      const dly = add('delay', ...L(6), { time: 35, fb: 0.2, mix: 0.35 }); // a short delay: chorus-like
      wire(mix, 'out', flt, 'in');
      wire(lfo, 'out', flt, 'cv');
      wire(v, 'out', dly, 'in');
      toOut(add, wire, dly, 'out', 0.7, 7);
    }),
  'Wobble 2': () =>
    make('Wobble 2', (add, wire) => {
      const kb = add('fm1', ...L(0));
      const osc = add('osc', ...L(1), { wave: 3, coarse: -12, level: 0.5 });
      const lfo = add('lfo', ...L(1, 1), { wave: 1, rate: 6, depth: 1 });
      const flt = add('filter', ...L(2), { cutoff: 250, res: 0.7, cvamt: 2.5 });
      const v = voice(add, wire, kb, flt, 'lp', { a: 0.003, d: 0.1, s: 1, r: 0.1 }, 3);
      wire(kb, 'pitch', osc, 'pitch');
      wire(osc, 'out', flt, 'in');
      wire(lfo, 'out', flt, 'cv');
      toOut(add, wire, v, 'out', 0.7, 5);
    }),
  'Echo Pluck': () =>
    make('Echo Pluck', (add, wire) => {
      const kb = add('fm1', ...L(0));
      const osc = add('osc', ...L(1), { wave: 1, level: 0.9 });
      const v = voice(add, wire, kb, osc, 'out', { a: 0.001, d: 0.25, s: 0, r: 0.1 }, 2);
      const dly = add('delay', ...L(4), { time: 360, fb: 0.6, mix: 0.5 });
      wire(kb, 'pitch', osc, 'pitch');
      wire(v, 'out', dly, 'in');
      toOut(add, wire, dly, 'out', 0.7, 5);
    }),
  'Dub Stab': () =>
    make('Dub Stab', (add, wire) => {
      const kb = add('fm1', ...L(0));
      const a = add('osc', ...L(1), { wave: 2, level: 0.8 });
      const b = add('osc', ...L(1, 1), { wave: 3, coarse: 7, level: 0.6 });
      const mix = add('mixer', ...L(2), { l1: 0.5, l2: 0.5 });
      const fe = add('adsr', ...L(2, 1), { a: 0.001, d: 0.25, s: 0, r: 0.1 });
      const flt = add('filter', ...L(3), { cutoff: 700, res: 0.5, cvamt: 2 });
      const v = voice(add, wire, kb, flt, 'lp', { a: 0.001, d: 0.3, s: 0, r: 0.2 }, 4);
      const dly = add('delay', ...L(6), { time: 500, fb: 0.65, mix: 0.45 });
      wire(kb, 'pitch', a, 'pitch');
      wire(kb, 'pitch', b, 'pitch');
      wire(kb, 'gate', fe, 'gate');
      wire(a, 'out', mix, 'in1');
      wire(b, 'out', mix, 'in2');
      wire(mix, 'out', flt, 'in');
      wire(fe, 'env', flt, 'cv');
      wire(v, 'out', dly, 'in');
      toOut(add, wire, dly, 'out', 0.7, 7);
    }),
  'Noise Sweep': () =>
    make('Noise Sweep', (add, wire) => {
      const kb = add('fm1', ...L(0));
      const nz = add('noise', ...L(1), { level: 0.7 });
      const fe = add('adsr', ...L(1, 1), { a: 1.5, d: 1, s: 0.2, r: 1 });
      const flt = add('filter', ...L(2), { cutoff: 200, res: 0.8, cvamt: 4 });
      const v = voice(add, wire, kb, flt, 'bp', { a: 0.3, d: 1, s: 0.8, r: 0.8 }, 3);
      wire(kb, 'gate', fe, 'gate');
      wire(nz, 'out', flt, 'in');
      wire(fe, 'env', flt, 'cv');
      toOut(add, wire, v, 'out', 0.8, 5);
    }),
  Metal: () =>
    make('Metal', (add, wire) => {
      const kb = add('fm1', ...L(0));
      const mod = add('osc', ...L(1), { wave: 0, coarse: 7, fine: 17, level: 1 });
      const car = add('osc', ...L(2), { wave: 0, pm: 6, level: 0.7 });
      const v = voice(add, wire, kb, car, 'out', { a: 0.001, d: 0.6, s: 0.1, r: 0.4 }, 3);
      wire(kb, 'pitch', mod, 'pitch');
      wire(kb, 'pitch', car, 'pitch');
      wire(mod, 'out', car, 'pm');
      toOut(add, wire, v, 'out', 0.7, 5);
    }),
  Drone: () =>
    make('Drone', (add, wire) => {
      // sounds without a key; the keys change the pitch
      const kb = add('fm1', ...L(0));
      const a = add('osc', ...L(1), { wave: 2, coarse: -24, level: 0.8 });
      const b = add('osc', ...L(1, 1), { wave: 2, coarse: -17, level: 0.7 });
      const mix = add('mixer', ...L(2), { l1: 0.5, l2: 0.5 });
      const lfo = add('lfo', ...L(2, 1), { wave: 0, rate: 0.12, depth: 1 });
      const flt = add('filter', ...L(3), { cutoff: 500, res: 0.3, cvamt: 1 });
      const dly = add('delay', ...L(4), { time: 700, fb: 0.5, mix: 0.4 });
      wire(kb, 'pitch', a, 'pitch');
      wire(kb, 'pitch', b, 'pitch');
      wire(a, 'out', mix, 'in1');
      wire(b, 'out', mix, 'in2');
      wire(mix, 'out', flt, 'in');
      wire(lfo, 'out', flt, 'cv');
      wire(flt, 'lp', dly, 'in');
      toOut(add, wire, dly, 'out', 0.6, 5);
    }),
  'Tremolo Keys': () =>
    make('Tremolo Keys', (add, wire) => {
      const kb = add('fm1', ...L(0));
      const osc = add('osc', ...L(1), { wave: 1, level: 0.9 });
      const v = voice(add, wire, kb, osc, 'out', { a: 0.01, d: 0.6, s: 0.5, r: 0.3 }, 2);
      const lfo = add('lfo', ...L(2, 1), { wave: 0, rate: 5, depth: 1 });
      const sc = add('scale', ...L(3, 1), { gain: 0.4, offset: 0.6 });
      const trem = add('vca', ...L(3), { gain: 1 });
      wire(kb, 'pitch', osc, 'pitch');
      wire(lfo, 'out', sc, 'in');
      wire(v, 'out', trem, 'in');
      wire(sc, 'out', trem, 'cv');
      toOut(add, wire, trem, 'out', 0.7, 5);
    }),
  'S&H Bleeps': () =>
    make('S&H Bleeps', (add, wire) => {
      const kb = add('fm1', ...L(0));
      const osc = add('osc', ...L(1), { wave: 3, level: 0.6 });
      const sh = add('lfo', ...L(1, 1), { wave: 4, rate: 8, depth: 1 });
      const sc = add('scale', ...L(2, 1), { gain: 0.5 });
      const v = voice(add, wire, kb, osc, 'out', { a: 0.001, d: 0.1, s: 0.6, r: 0.1 }, 2);
      const dly = add('delay', ...L(4), { time: 190, fb: 0.45, mix: 0.3 });
      wire(kb, 'pitch', osc, 'pitch');
      wire(sh, 'out', sc, 'in');
      wire(sc, 'out', osc, 'pitch');
      wire(v, 'out', dly, 'in');
      toOut(add, wire, dly, 'out', 0.7, 5);
    }),
  Glass: () =>
    make('Glass', (add, wire) => {
      const kb = add('fm1', ...L(0));
      const a = add('osc', ...L(1), { wave: 0, level: 0.6 });
      const b = add('osc', ...L(1, 1), { wave: 1, coarse: 12, level: 0.3 });
      const mix = add('mixer', ...L(2), { l1: 0.7, l2: 0.5 });
      const v = voice(add, wire, kb, mix, 'out', { a: 0.02, d: 1.5, s: 0.3, r: 1.2 }, 3);
      const dly = add('delay', ...L(5), { time: 400, fb: 0.55, mix: 0.4 });
      wire(kb, 'pitch', a, 'pitch');
      wire(kb, 'pitch', b, 'pitch');
      wire(a, 'out', mix, 'in1');
      wire(b, 'out', mix, 'in2');
      wire(v, 'out', dly, 'in');
      toOut(add, wire, dly, 'out', 0.7, 6);
    }),
  'Fat Mono': () =>
    make('Fat Mono', (add, wire) => {
      const kb = add('fm1', ...L(0));
      const mix = add('mixer', ...L(2), { l1: 0.4, l2: 0.4, l3: 0.4 });
      [
        [0, -12],
        [0, 12],
        [-12, 0],
      ].forEach(([c, f], i) => {
        const o = add('osc', ...L(1, i), { wave: 2, coarse: c, fine: f, level: 0.8 });
        wire(kb, 'pitch', o, 'pitch');
        wire(o, 'out', mix, `in${i + 1}`);
      });
      const fe = add('adsr', ...L(2, 1), { a: 0.005, d: 0.3, s: 0.4, r: 0.2 });
      const flt = add('filter', ...L(3), { cutoff: 1800, res: 0.2, cvamt: 2 });
      const v = voice(add, wire, kb, flt, 'lp', { a: 0.005, d: 0.2, s: 0.9, r: 0.15 }, 4);
      wire(kb, 'gate', fe, 'gate');
      wire(mix, 'out', flt, 'in');
      wire(fe, 'env', flt, 'cv');
      toOut(add, wire, v, 'out', 0.7, 5);
    }),
};
