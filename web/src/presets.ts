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

/** an amplitude chain: `from` -> VCA, opened by an ADSR on the panel's gate. Returns the VCA and the ADSR. */
function voice(add: AddFn, wire: WireFn, kb: number, from: number, port: string, env: { a: number; d: number; s: number; r: number }, col: number, gain = 1, envOpts?: AddOpts, vcaOpts?: AddOpts): [vca: number, env: number] {
  const e = add('adsr', ...L(col, 1), env, envOpts);
  const v = add('vca', ...L(col), { gain }, vcaOpts);
  wire(kb, 'gate', e, 'gate');
  wire(from, port, v, 'in');
  wire(e, 'env', v, 'cv');
  return [v, e];
}

function toOut(add: AddFn, wire: WireFn, from: number, port: string, level: number, col: number): number {
  const o = add('output', ...L(col), { level });
  wire(from, port, o, 'l');
  return o;
}

export const PRESETS: Record<string, () => Patch> = {
  '303 Acid': () =>
    make('303 Acid', (add, wire) => {
      const kb = add('fm1', 20, 40, { k1: 0.644, k2: 0.5, k3: 0.219, k4: 0.25 });
      const osc = add('osc', 220, 20, { wave: 2, coarse: -12, level: 0.9 });
      const fenv = add('adsr', 220, 430, { a: 0.001, d: 0.001, s: 0, r: 0.1 }, { map: { p1: ['d'], p2: ['d'], k1: ['a'], k2: ['d'], k3: ['s'], k4: ['r'] }, att: [0.2, 1] });
      const flt = add('filter', 440, 20, { cutoff: 20, res: 0.58, cvamt: 3.2 }, { map: { p1: ['cutoff'], p2: ['res'], k1: ['cutoff'], k2: ['res'], k3: ['cvamt'], k4: ['track'] }, att: [1, 0.6] });
      const aenv = add('adsr', 440, 430, { a: 0.002, d: 0.15, s: 0.8, r: 0.07 });
      const vca = add('vca', 660, 20, { gain: 1.1 });
      const dly = add('delay', 660, 430, { time: 375, fb: 0.35, mix: 0 }, { map: { p1: ['mix'], p2: ['fb'], k1: ['time'], k2: ['fb'], k3: ['mix'], k4: [] }, att: [1, 1] });
      const out = add('output', 880, 40);
      wire(kb, 'pitch', osc, 'pitch');
      wire(osc, 'out', flt, 'in');
      wire(kb, 'gate', fenv, 'gate');
      wire(fenv, 'env', flt, 'cv');
      wire(kb, 'gate', aenv, 'gate');
      wire(flt, 'lp12', vca, 'in');
      wire(aenv, 'env', vca, 'cv');
      wire(vca, 'out', dly, 'in');
      wire(dly, 'out', out, 'l');
      wire(kb, 'k1', flt, 'p1');
      wire(kb, 'k2', flt, 'p2');
      wire(kb, 'k3', fenv, 'p1');
      wire(kb, 'k4', dly, 'p1');
    }),
  'Subtractive saw': () =>
    make('Subtractive saw', (add, wire) => {
      const kb = add('fm1', 20, 40, { k1: 0.8012, k2: 0.5, k3: 0.1663, k4: 0.5 });
      const osc = add('osc', 220, 20, { wave: 2 });
      const env = add('adsr', 220, 430, { a: 0.01, d: 0.3, s: 0.5, r: 0.001 }, { map: { p1: ['a'], p2: ['r'], k1: ['a'], k2: ['d'], k3: ['s'], k4: ['r'] }, att: [0.3, 1] });
      const flt = add('filter', 440, 20, { cutoff: 20, res: 0.15, cvamt: 2 }, { map: { p1: ['cutoff'], p2: ['res'], k1: ['cutoff'], k2: ['res'], k3: ['cvamt'], k4: ['track'] }, att: [1, 0.6] });
      const vca = add('vca', 660, 20, { gain: 0.5 }, { map: { p1: ['gain'], p2: [], k1: ['gain'], k2: [], k3: [], k4: [] }, att: [0.5, 1] });
      const out = add('output', 860, 40);
      wire(kb, 'pitch', osc, 'pitch');
      wire(osc, 'out', flt, 'in');
      wire(kb, 'gate', env, 'gate');
      wire(env, 'env', flt, 'cv');
      wire(flt, 'mix', vca, 'in');
      wire(env, 'env', vca, 'cv');
      wire(vca, 'out', out, 'l');
      wire(kb, 'k1', flt, 'p1');
      wire(kb, 'k2', flt, 'morph');
      wire(kb, 'k3', env, 'p1');
      wire(kb, 'k4', env, 'p2');
    }),
  'Two-operator FM': () =>
    make('Two-operator FM', (add, wire) => {
      const kb = add('fm1', 20, 120, { k1: 0.5, k2: 0.5, k3: 0.5, k4: 0.5 });
      const mod = add('osc', 220, 20, { wave: 0, coarse: 0, level: 1 }, { map: { p1: ['coarse'], p2: ['pw'], k1: ['wave'], k2: ['pw'], k3: ['coarse'], k4: ['fine'] }, att: [0.25, 1] });
      const modEnv = add('adsr', 220, 430, { a: 0.001, d: 0.5, s: 0.1, r: 0.3 });
      const modVca = add('vca', 440, 20, { gain: 1 });
      const car = add('osc', 660, 20, { wave: 0, pm: 1, level: 1 }, { map: { p1: ['pm'], p2: ['pw'], k1: ['wave'], k2: ['pw'], k3: ['coarse'], k4: ['fine'] }, att: [0.5, 1] });
      const ampEnv = add('adsr', 660, 430, { a: 0.003, d: 0.0502, s: 0.4, r: 0.5 }, { map: { p1: ['d'], p2: ['d'], k1: ['a'], k2: ['d'], k3: ['s'], k4: ['r'] }, att: [0.3, 1] });
      const amp = add('vca', 880, 20, { gain: 0.5 }, { map: { p1: ['gain'], p2: [], k1: ['gain'], k2: [], k3: [], k4: [] }, att: [0.5, 1] });
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
      wire(kb, 'k1', car, 'p1');
      wire(kb, 'k2', mod, 'p1');
      wire(kb, 'k3', ampEnv, 'p1');
      wire(kb, 'k4', amp, 'p1');
    }),
  'Wobble bass': () =>
    make('Wobble bass', (add, wire) => {
      const kb = add('fm1', 20, 40, { k1: 0.7149, k2: 0.5, k3: 0.2485, k4: 0.5 });
      const osc = add('osc', 220, 20, { wave: 3, coarse: -12 });
      const lfo = add('lfo', 220, 430, { rate: 0.02, wave: 0 }, { map: { p1: ['rate'], p2: ['rate'], k1: ['wave'], k2: ['rate'], k3: ['depth'], k4: [] }, att: [0.3, 1] });
      const flt = add('filter', 440, 20, { cutoff: 20, res: 0.3, cvamt: 1.5 }, { map: { p1: ['cutoff'], p2: ['res'], k1: ['cutoff'], k2: ['res'], k3: ['cvamt'], k4: ['track'] }, att: [1, 0.6] });
      const env = add('adsr', 440, 430, { a: 0.005, d: 0.1, s: 0.9, r: 0.15 });
      const vca = add('vca', 660, 20, { gain: 0.5 }, { map: { p1: ['gain'], p2: [], k1: ['gain'], k2: [], k3: [], k4: [] }, att: [0.5, 1] });
      const out = add('output', 860, 40);
      wire(kb, 'pitch', osc, 'pitch');
      wire(osc, 'out', flt, 'in');
      wire(lfo, 'out', flt, 'cv');
      wire(kb, 'gate', env, 'gate');
      wire(flt, 'lp12', vca, 'in');
      wire(env, 'env', vca, 'cv');
      wire(vca, 'out', out, 'l');
      wire(kb, 'k1', flt, 'p1');
      wire(kb, 'k2', flt, 'p2');
      wire(kb, 'k3', lfo, 'p1');
      wire(kb, 'k4', vca, 'p1');
    }),
  'FM-1 Panel synth': () =>
    make('FM-1 Panel synth', (add, wire) => {
      const fm = add('fm1', 20, 40, { k1: 0.9027, k2: 0.5, k3: 0.5, k4: 0.3 });
      const osc = add('osc', 260, 20, { wave: 2, level: 0.8 });
      const env = add('adsr', 260, 430, { a: 0.005, d: 0.25, s: 0.6, r: 0.25 });
      const flt = add('filter', 480, 20, { cutoff: 20, res: 0, cvamt: 0 }, { map: { p1: ['cutoff'], p2: ['res'], k1: ['cutoff'], k2: ['res'], k3: ['cvamt'], k4: ['track'] }, att: [1, 0.6] });
      const vca = add('vca', 700, 20);
      const dly = add('delay', 700, 430, { time: 50.25, fb: 0.4, mix: 0 }, { map: { p1: ['time'], p2: ['mix'], k1: ['time'], k2: ['fb'], k3: ['mix'], k4: [] }, att: [0.5, 1] });
      const out = add('output', 920, 40);
      wire(fm, 'pitch', osc, 'pitch');
      wire(fm, 'gate', env, 'gate');
      wire(osc, 'out', flt, 'in');
      wire(flt, 'lp12', vca, 'in');
      wire(env, 'env', vca, 'cv');
      wire(vca, 'out', dly, 'in');
      wire(dly, 'out', out, 'l');
      wire(fm, 'k1', flt, 'p1');
      wire(fm, 'k2', flt, 'p2');
      wire(fm, 'k3', dly, 'p1');
      wire(fm, 'k4', dly, 'p2');
    }),

  // ---- the rest of the factory bank (32 in all). Layout: L(column, row); every patch plays from the FM-1 panel module.
  'Sub Bass': () =>
    make('Sub Bass', (add, wire) => {
      const kb = add('fm1', ...L(0), { k1: 0.758, k2: 0.5, k3: 0.0621, k4: 0.5 });
      const sine = add('osc', ...L(1), { wave: 0, coarse: -12, level: 0.9 });
      const saw = add('osc', ...L(1, 1), { wave: 2, coarse: -24, level: 0.1 }, { map: { p1: ['level'], p2: ['pw'], k1: ['wave'], k2: ['pw'], k3: ['coarse'], k4: ['fine'] }, att: [0.6, 1] });
      const mix = add('mixer', ...L(2), { l1: 0.8, l2: 0.5 });
      const flt = add('filter', ...L(3), { cutoff: 20, res: 0.1 }, { map: { p1: ['cutoff'], p2: ['res'], k1: ['cutoff'], k2: ['res'], k3: ['cvamt'], k4: ['track'] }, att: [1, 1] });
      const [v, vEnv] = voice(add, wire, kb, flt, 'lp12', { a: 0.005, d: 0.2, s: 0.8, r: 0.001 }, 4, 0.5, { map: { p1: ['r'], p2: ['d'], k1: ['a'], k2: ['d'], k3: ['s'], k4: ['r'] }, att: [0.3, 1] }, { map: { p1: ['gain'], p2: [], k1: ['gain'], k2: [], k3: [], k4: [] }, att: [0.5, 1] });
      wire(kb, 'pitch', sine, 'pitch');
      wire(kb, 'pitch', saw, 'pitch');
      wire(sine, 'out', mix, 'in1');
      wire(saw, 'out', mix, 'in2');
      wire(mix, 'out', flt, 'in');
      toOut(add, wire, v, 'out', 0.8, 5);
      wire(kb, 'k1', flt, 'p1');
      wire(kb, 'k2', saw, 'p1');
      wire(kb, 'k3', vEnv, 'p1');
      wire(kb, 'k4', v, 'p1');
    }),
  Pluck: () =>
    make('Pluck', (add, wire) => {
      const kb = add('fm1', ...L(0), { k1: 0.7149, k2: 0.5, k3: 0.179, k4: 0.25 });
      const osc = add('osc', ...L(1), { wave: 2, level: 0.8 });
      const fe = add('adsr', ...L(2, 1), { a: 0.001, d: 0.001, s: 0, r: 0.1 }, { map: { p1: ['d'], p2: ['d'], k1: ['a'], k2: ['d'], k3: ['s'], k4: ['r'] }, att: [0.2, 1] });
      const flt = add('filter', ...L(2), { cutoff: 20, res: 0, cvamt: 3 }, { map: { p1: ['cutoff'], p2: ['res'], k1: ['cutoff'], k2: ['res'], k3: ['cvamt'], k4: ['track'] }, att: [1, 0.6] });
      const [v] = voice(add, wire, kb, flt, 'lp12', { a: 0.001, d: 0.35, s: 0, r: 0.2 }, 3);
      const dly = add('delay', ...L(4), { time: 300, fb: 0.35, mix: 0 }, { map: { p1: ['mix'], p2: ['fb'], k1: ['time'], k2: ['fb'], k3: ['mix'], k4: [] }, att: [1, 1] });
      wire(kb, 'pitch', osc, 'pitch');
      wire(kb, 'gate', fe, 'gate');
      wire(osc, 'out', flt, 'in');
      wire(fe, 'env', flt, 'cv');
      wire(v, 'out', dly, 'in');
      toOut(add, wire, dly, 'out', 0.7, 5);
      wire(kb, 'k1', flt, 'p1');
      wire(kb, 'k2', flt, 'p2');
      wire(kb, 'k3', fe, 'p1');
      wire(kb, 'k4', dly, 'p1');
    }),
  'Warm Pad': () =>
    make('Warm Pad', (add, wire) => {
      const kb = add('fm1', ...L(0), { k1: 0.8476, k2: 0.5, k3: 0.3, k4: 0.3 });
      const a = add('osc', ...L(1), { wave: 2, fine: -8, level: 0.8 });
      const b = add('osc', ...L(1, 1), { wave: 2, fine: 8, level: 0.8 });
      const mix = add('mixer', ...L(2), { l1: 0.5, l2: 0.5 });
      const lfo = add('lfo', ...L(2, 1), { wave: 0, rate: 0.3, depth: 0.2 }, { map: { p1: ['depth'], p2: ['rate'], k1: ['wave'], k2: ['rate'], k3: ['depth'], k4: [] }, att: [0.6, 1] });
      const flt = add('filter', ...L(3), { cutoff: 20, res: 0.15, cvamt: 0.8 }, { map: { p1: ['cutoff'], p2: ['res'], k1: ['cutoff'], k2: ['res'], k3: ['cvamt'], k4: ['track'] }, att: [1, 1] });
      const [v] = voice(add, wire, kb, flt, 'lp12', { a: 1.2, d: 1, s: 0.8, r: 1.8 }, 4);
      const dly = add('delay', ...L(5), { time: 420, fb: 0.4, mix: 0 }, { map: { p1: ['mix'], p2: ['fb'], k1: ['time'], k2: ['fb'], k3: ['mix'], k4: [] }, att: [1, 1] });
      wire(kb, 'pitch', a, 'pitch');
      wire(kb, 'pitch', b, 'pitch');
      wire(a, 'out', mix, 'in1');
      wire(b, 'out', mix, 'in2');
      wire(mix, 'out', flt, 'in');
      wire(lfo, 'out', flt, 'cv');
      wire(v, 'out', dly, 'in');
      const rv = add('reverb', ...L(6), { size: 0.7, damp: 0.5, mix: 0 }, { map: { p1: ['mix'], p2: ['damp'], k1: ['size'], k2: ['damp'], k3: ['mix'], k4: [] }, att: [1, 1] });
      wire(dly, 'out', rv, 'in');
      toOut(add, wire, rv, 'out', 0.7, 7);
      wire(kb, 'k1', flt, 'p1');
      wire(kb, 'k2', lfo, 'p1');
      wire(kb, 'k3', rv, 'p1');
      wire(kb, 'k4', dly, 'p1');
    }),
  'Square Lead': () =>
    make('Square Lead', (add, wire) => {
      const kb = add('fm1', ...L(0), { k1: 0.9725, k2: 0.4167, k3: 0.5, k4: 0.2 });
      const osc = add('osc', ...L(1), { wave: 3, pw: 0.275, level: 0.7 }, { map: { p1: ['pw'], p2: ['pw'], k1: ['wave'], k2: ['pw'], k3: ['coarse'], k4: ['fine'] }, att: [0.5, 1] });
      const flt = add('filter', ...L(2), { cutoff: 20, res: 0 }, { map: { p1: ['cutoff'], p2: ['res'], k1: ['cutoff'], k2: ['res'], k3: ['cvamt'], k4: ['track'] }, att: [1, 0.6] });
      const [v] = voice(add, wire, kb, flt, 'lp12', { a: 0.01, d: 0.2, s: 0.7, r: 0.2 }, 3);
      const dly = add('delay', ...L(5), { time: 250, fb: 0.3, mix: 0 }, { map: { p1: ['mix'], p2: ['fb'], k1: ['time'], k2: ['fb'], k3: ['mix'], k4: [] }, att: [1, 1] });
      wire(kb, 'pitch', osc, 'pitch');
      wire(osc, 'out', flt, 'in');
      wire(v, 'out', dly, 'in');
      toOut(add, wire, dly, 'out', 0.7, 6);
      wire(kb, 'k1', flt, 'p1');
      wire(kb, 'k2', flt, 'p2');
      wire(kb, 'k3', osc, 'p1');
      wire(kb, 'k4', dly, 'p1');
    }),
  'PWM Pad': () =>
    make('PWM Pad', (add, wire) => {
      const kb = add('fm1', ...L(0), { k1: 0.08, k2: 1, k3: 0.3994, k4: 0.5 });
      const osc = add('osc', ...L(1), { wave: 3, level: 0.6 });
      const lfo = add('lfo', ...L(1, 1), { wave: 0, rate: 0.02, depth: 0 }, { map: { p1: ['rate'], p2: ['depth'], k1: ['wave'], k2: ['rate'], k3: ['depth'], k4: [] }, att: [0.15, 1] });
      const [v, vEnv] = voice(add, wire, kb, osc, 'out', { a: 0.001, d: 1, s: 0.9, r: 1.2 }, 2, 0.5, { map: { p1: ['a'], p2: ['d'], k1: ['a'], k2: ['d'], k3: ['s'], k4: ['r'] }, att: [0.3, 1] }, { map: { p1: ['gain'], p2: [], k1: ['gain'], k2: [], k3: [], k4: [] }, att: [0.5, 1] });
      wire(kb, 'pitch', osc, 'pitch');
      wire(lfo, 'out', osc, 'p2'); // p2 controls the pulse width by default
      toOut(add, wire, v, 'out', 0.7, 4);
      wire(kb, 'k1', lfo, 'p1');
      wire(kb, 'k2', lfo, 'p2');
      wire(kb, 'k3', vEnv, 'p1');
      wire(kb, 'k4', v, 'p1');
    }),
  Wind: () =>
    make('Wind', (add, wire) => {
      const kb = add('fm1', ...L(0), { k1: 0.7794, k2: 0.5, k3: 0.35, k4: 0.3 });
      const nz = add('noise', ...L(1), { level: 0.7 });
      const lfo = add('lfo', ...L(1, 1), { wave: 0, rate: 0.15, depth: 1 });
      const flt = add('filter', ...L(2), { cutoff: 20, res: 0.3, cvamt: 1.5 }, { map: { p1: ['cutoff'], p2: ['res'], k1: ['cutoff'], k2: ['res'], k3: ['cvamt'], k4: ['track'] }, att: [1, 0.6] });
      const [v] = voice(add, wire, kb, flt, 'bp', { a: 1.5, d: 1, s: 1, r: 2 }, 3);
      const dly = add('delay', ...L(5), { time: 500, fb: 0.5, mix: 0 }, { map: { p1: ['mix'], p2: ['fb'], k1: ['time'], k2: ['fb'], k3: ['mix'], k4: [] }, att: [1, 1] });
      wire(nz, 'out', flt, 'in');
      wire(lfo, 'out', flt, 'cv');
      wire(v, 'out', dly, 'in');
      const rv = add('reverb', ...L(6), { size: 0.8, damp: 0.5, mix: 0 }, { map: { p1: ['mix'], p2: ['damp'], k1: ['size'], k2: ['damp'], k3: ['mix'], k4: [] }, att: [1, 1] });
      wire(dly, 'out', rv, 'in');
      toOut(add, wire, rv, 'out', 0.7, 7);
      wire(kb, 'k1', flt, 'p1');
      wire(kb, 'k2', flt, 'p2');
      wire(kb, 'k3', rv, 'p1');
      wire(kb, 'k4', dly, 'p1');
    }),
  'Hi-hat': () =>
    make('Hi-hat', (add, wire) => {
      const kb = add('fm1', ...L(0), { k1: 1, k2: 0.3333, k3: 0.118, k4: 0.5 });
      const nz = add('noise', ...L(1), { level: 0.8 });
      const flt = add('filter', ...L(2), { cutoff: 20, res: 0 }, { map: { p1: ['cutoff'], p2: ['res'], k1: ['cutoff'], k2: ['res'], k3: ['cvamt'], k4: ['track'] }, att: [1, 0.6] });
      const [v, vEnv] = voice(add, wire, kb, flt, 'hp', { a: 0.001, d: 0.001, s: 0, r: 0.05 }, 3, 0.5, { map: { p1: ['d'], p2: ['d'], k1: ['a'], k2: ['d'], k3: ['s'], k4: ['r'] }, att: [0.1, 1] }, { map: { p1: ['gain'], p2: [], k1: ['gain'], k2: [], k3: [], k4: [] }, att: [0.5, 1] });
      wire(nz, 'out', flt, 'in');
      toOut(add, wire, v, 'out', 0.7, 5);
      wire(kb, 'k1', flt, 'p1');
      wire(kb, 'k2', flt, 'p2');
      wire(kb, 'k3', vEnv, 'p1');
      wire(kb, 'k4', v, 'p1');
    }),
  Snare: () =>
    make('Snare', (add, wire) => {
      const kb = add('fm1', ...L(0), { k1: 0.9323, k2: 0.5, k3: 0.2981, k4: 0.6 });
      const nz = add('noise', ...L(1), { level: 0.7 });
      const flt = add('filter', ...L(2), { cutoff: 20, res: 0 }, { map: { p1: ['cutoff'], p2: ['res'], k1: ['cutoff'], k2: ['res'], k3: ['cvamt'], k4: ['track'] }, att: [1, 0.6] });
      const body = add('osc', ...L(1, 1), { wave: 1, coarse: -5, level: 0.5 });
      const mix = add('mixer', ...L(3), { l1: 0.7, l2: 0 }, { map: { p1: ['l2'], p2: ['l2'], k1: ['l1'], k2: ['l2'], k3: ['l3'], k4: ['l4'] }, att: [1, 1] });
      const [v, vEnv] = voice(add, wire, kb, mix, 'out', { a: 0.001, d: 0.001, s: 0, r: 0.1 }, 4, 1, { map: { p1: ['d'], p2: ['d'], k1: ['a'], k2: ['d'], k3: ['s'], k4: ['r'] }, att: [0.1, 1] });
      wire(kb, 'pitch', body, 'pitch');
      wire(nz, 'out', flt, 'in');
      wire(flt, 'bp', mix, 'in1');
      wire(body, 'out', mix, 'in2');
      toOut(add, wire, v, 'out', 0.8, 6);
      wire(kb, 'k1', flt, 'p1');
      wire(kb, 'k2', flt, 'p2');
      wire(kb, 'k3', vEnv, 'p1');
      wire(kb, 'k4', mix, 'p1');
    }),
  Kick: () =>
    make('Kick', (add, wire) => {
      const kb = add('fm1', ...L(0), { k1: 0.5, k2: 0.158, k3: 0.5, k4: 0.5 });
      const osc = add('osc', ...L(1), { wave: 0, coarse: -24, level: 1 });
      const pe = add('adsr', ...L(1, 1), { a: 0.001, d: 0.001, s: 0, r: 0.05 }, { map: { p1: ['d'], p2: ['d'], k1: ['a'], k2: ['d'], k3: ['s'], k4: ['r'] }, att: [0.1, 1] });
      const sc = add('scale', ...L(2, 1), { gain: -0.5 }, { map: { p1: ['gain'], p2: ['offset'], k1: ['gain'], k2: ['offset'], k3: [], k4: [] }, att: [0.5, 1] });
      const [v, vEnv] = voice(add, wire, kb, osc, 'out', { a: 0.001, d: 0.05, s: 0, r: 0.1 }, 2, 0.5, { map: { p1: ['d'], p2: ['d'], k1: ['a'], k2: ['d'], k3: ['s'], k4: ['r'] }, att: [0.1, 1] }, { map: { p1: ['gain'], p2: [], k1: ['gain'], k2: [], k3: [], k4: [] }, att: [0.5, 1] });
      wire(kb, 'gate', pe, 'gate');
      wire(pe, 'env', sc, 'in');
      wire(sc, 'out', osc, 'pitch'); // a falling pitch sweep
      toOut(add, wire, v, 'out', 0.9, 4);
      wire(kb, 'k1', sc, 'p1');
      wire(kb, 'k2', pe, 'p1');
      wire(kb, 'k3', vEnv, 'p1');
      wire(kb, 'k4', v, 'p1');
    }),
  Laser: () =>
    make('Laser', (add, wire) => {
      const kb = add('fm1', ...L(0), { k1: 0.5, k2: 0.4981, k3: 0.5, k4: 0.3 });
      const osc = add('osc', ...L(1), { wave: 1, level: 0.7 });
      const pe = add('adsr', ...L(1, 1), { a: 0.001, d: 0.001, s: 0, r: 0.1 }, { map: { p1: ['d'], p2: ['d'], k1: ['a'], k2: ['d'], k3: ['s'], k4: ['r'] }, att: [0.1, 1] });
      const sc = add('scale', ...L(2, 1), { gain: 0 }, { map: { p1: ['gain'], p2: ['offset'], k1: ['gain'], k2: ['offset'], k3: [], k4: [] }, att: [0.5, 1] });
      const [v, vEnv] = voice(add, wire, kb, osc, 'out', { a: 0.001, d: 0.05, s: 0, r: 0.1 }, 2, 1, { map: { p1: ['d'], p2: ['d'], k1: ['a'], k2: ['d'], k3: ['s'], k4: ['r'] }, att: [0.1, 1] });
      const dly = add('delay', ...L(4), { time: 180, fb: 0.4, mix: 0 }, { map: { p1: ['mix'], p2: ['fb'], k1: ['time'], k2: ['fb'], k3: ['mix'], k4: [] }, att: [1, 1] });
      wire(kb, 'pitch', osc, 'pitch');
      wire(kb, 'gate', pe, 'gate');
      wire(pe, 'env', sc, 'in');
      wire(sc, 'out', osc, 'pitch');
      wire(v, 'out', dly, 'in');
      toOut(add, wire, dly, 'out', 0.7, 5);
      wire(kb, 'k1', sc, 'p1');
      wire(kb, 'k2', pe, 'p1');
      wire(kb, 'k3', vEnv, 'p1');
      wire(kb, 'k4', dly, 'p1');
    }),
  Siren: () =>
    make('Siren', (add, wire) => {
      const kb = add('fm1', ...L(0), { k1: 0.2951, k2: 0.5, k3: 0.1869, k4: 0.5 });
      const osc = add('osc', ...L(1), { wave: 0, level: 0.7 });
      const lfo = add('lfo', ...L(1, 1), { wave: 1, rate: 0.02, depth: 1 }, { map: { p1: ['rate'], p2: ['rate'], k1: ['wave'], k2: ['rate'], k3: ['depth'], k4: [] }, att: [0.1, 1] });
      const sc = add('scale', ...L(2, 1), { gain: 0.1 }, { map: { p1: ['gain'], p2: ['offset'], k1: ['gain'], k2: ['offset'], k3: [], k4: [] }, att: [0.1, 1] });
      const [v, vEnv] = voice(add, wire, kb, osc, 'out', { a: 0.05, d: 0.1, s: 1, r: 0.001 }, 2, 0.5, { map: { p1: ['r'], p2: ['d'], k1: ['a'], k2: ['d'], k3: ['s'], k4: ['r'] }, att: [0.2, 1] }, { map: { p1: ['gain'], p2: [], k1: ['gain'], k2: [], k3: [], k4: [] }, att: [0.5, 1] });
      wire(kb, 'pitch', osc, 'pitch');
      wire(lfo, 'out', sc, 'in');
      wire(sc, 'out', osc, 'pitch');
      toOut(add, wire, v, 'out', 0.7, 4);
      wire(kb, 'k1', lfo, 'p1');
      wire(kb, 'k2', sc, 'p1');
      wire(kb, 'k3', vEnv, 'p1');
      wire(kb, 'k4', v, 'p1');
    }),
  'Vibrato Lead': () =>
    make('Vibrato Lead', (add, wire) => {
      const kb = add('fm1', ...L(0), { k1: 0.9546, k2: 0.3333, k3: 0.5, k4: 0.5 });
      const osc = add('osc', ...L(1), { wave: 2, level: 0.7 });
      const lfo = add('lfo', ...L(1, 1), { wave: 0, rate: 2.5015, depth: 1 }, { map: { p1: ['rate'], p2: ['rate'], k1: ['wave'], k2: ['rate'], k3: ['depth'], k4: [] }, att: [0.15, 1] });
      const sc = add('scale', ...L(2, 1), { gain: -0.17 }, { map: { p1: ['gain'], p2: ['offset'], k1: ['gain'], k2: ['offset'], k3: [], k4: [] }, att: [0.05, 1] });
      const flt = add('filter', ...L(2), { cutoff: 20, res: 0 }, { map: { p1: ['cutoff'], p2: ['res'], k1: ['cutoff'], k2: ['res'], k3: ['cvamt'], k4: ['track'] }, att: [1, 0.6] });
      const [v] = voice(add, wire, kb, flt, 'lp12', { a: 0.02, d: 0.3, s: 0.8, r: 0.25 }, 3);
      wire(kb, 'pitch', osc, 'pitch');
      wire(lfo, 'out', sc, 'in');
      wire(sc, 'out', osc, 'pitch');
      wire(osc, 'out', flt, 'in');
      toOut(add, wire, v, 'out', 0.7, 5);
      wire(kb, 'k1', flt, 'p1');
      wire(kb, 'k2', flt, 'p2');
      wire(kb, 'k3', lfo, 'p1');
      wire(kb, 'k4', sc, 'p1');
    }),
  Organ: () =>
    make('Organ', (add, wire) => {
      const kb = add('fm1', ...L(0), { k1: 0.6, k2: 0.4, k3: 0.0988, k4: 0.5 });
      const mix = add('mixer', ...L(2), { l1: 0, l2: 0, l3: 0.25 }, { map: { p1: ['l1'], p2: ['l2'], k1: ['l1'], k2: ['l2'], k3: ['l3'], k4: ['l4'] }, att: [1, 1] });
      [0, 12, 19].forEach((c, i) => {
        const o = add('osc', ...L(1, i), { wave: 0, coarse: c, level: 0.8 });
        wire(kb, 'pitch', o, 'pitch');
        wire(o, 'out', mix, `in${i + 1}`);
      });
      const [v, vEnv] = voice(add, wire, kb, mix, 'out', { a: 0.01, d: 0.1, s: 1, r: 0.001 }, 3, 0.5, { map: { p1: ['r'], p2: ['d'], k1: ['a'], k2: ['d'], k3: ['s'], k4: ['r'] }, att: [0.1, 1] }, { map: { p1: ['gain'], p2: [], k1: ['gain'], k2: [], k3: [], k4: [] }, att: [0.5, 1] });
      toOut(add, wire, v, 'out', 0.7, 5);
      wire(kb, 'k1', mix, 'p1');
      wire(kb, 'k2', mix, 'p2');
      wire(kb, 'k3', vEnv, 'p1');
      wire(kb, 'k4', v, 'p1');
    }),
  'FM Bell': () =>
    make('FM Bell', (add, wire) => {
      const kb = add('fm1', ...L(0), { k1: 0.5, k2: 0.5, k3: 0.25, k4: 0.3 });
      const mod = add('osc', ...L(1), { wave: 0, coarse: 19, level: 1 });
      const [mv, mvEnv] = voice(add, wire, kb, mod, 'out', { a: 0.001, d: 0.4502, s: 0, r: 1 }, 2, 1, { map: { p1: ['d'], p2: ['d'], k1: ['a'], k2: ['d'], k3: ['s'], k4: ['r'] }, att: [0.3, 1] });
      const car = add('osc', ...L(3), { wave: 0, pm: 0.5, level: 0.8 }, { map: { p1: ['pm'], p2: ['pw'], k1: ['wave'], k2: ['pw'], k3: ['coarse'], k4: ['fine'] }, att: [0.5, 1] });
      const [v] = voice(add, wire, kb, car, 'out', { a: 0.001, d: 2, s: 0, r: 1.5 }, 4);
      const dly = add('delay', ...L(6), { time: 330, fb: 0.35, mix: 0 }, { map: { p1: ['mix'], p2: ['fb'], k1: ['time'], k2: ['fb'], k3: ['mix'], k4: [] }, att: [1, 1] });
      wire(kb, 'pitch', mod, 'pitch');
      wire(kb, 'pitch', car, 'pitch');
      wire(mv, 'out', car, 'pm');
      wire(v, 'out', dly, 'in');
      const rv = add('reverb', ...L(7), { size: 0.6, damp: 0.3, mix: 0 }, { map: { p1: ['mix'], p2: ['damp'], k1: ['size'], k2: ['damp'], k3: ['mix'], k4: [] }, att: [1, 1] });
      wire(dly, 'out', rv, 'in');
      toOut(add, wire, rv, 'out', 0.7, 8);
      wire(kb, 'k1', car, 'p1');
      wire(kb, 'k2', mvEnv, 'p1');
      wire(kb, 'k3', rv, 'p1');
      wire(kb, 'k4', dly, 'p1');
    }),
  'Electric Piano': () =>
    make('Electric Piano', (add, wire) => {
      const kb = add('fm1', ...L(0), { k1: 0.375, k2: 0.5, k3: 0.2, k4: 0.5 });
      const mod = add('osc', ...L(1), { wave: 0, coarse: 12, level: 1 });
      const [mv, mvEnv] = voice(add, wire, kb, mod, 'out', { a: 0.001, d: 0.0251, s: 0.15, r: 0.3 }, 2, 1, { map: { p1: ['d'], p2: ['d'], k1: ['a'], k2: ['d'], k3: ['s'], k4: ['r'] }, att: [0.15, 1] });
      const car = add('osc', ...L(3), { wave: 0, pm: 0, level: 0.8 }, { map: { p1: ['pm'], p2: ['pw'], k1: ['wave'], k2: ['pw'], k3: ['coarse'], k4: ['fine'] }, att: [0.4, 1] });
      const [v] = voice(add, wire, kb, car, 'out', { a: 0.002, d: 1, s: 0.2, r: 0.4 }, 4, 0.5, undefined, { map: { p1: ['gain'], p2: [], k1: ['gain'], k2: [], k3: [], k4: [] }, att: [0.5, 1] });
      wire(kb, 'pitch', mod, 'pitch');
      wire(kb, 'pitch', car, 'pitch');
      wire(mv, 'out', car, 'pm');
      const rv = add('reverb', ...L(5), { size: 0.5, damp: 0.5, mix: 0 }, { map: { p1: ['mix'], p2: ['damp'], k1: ['size'], k2: ['damp'], k3: ['mix'], k4: [] }, att: [1, 1] });
      wire(v, 'out', rv, 'in');
      toOut(add, wire, rv, 'out', 0.7, 6);
      wire(kb, 'k1', car, 'p1');
      wire(kb, 'k2', mvEnv, 'p1');
      wire(kb, 'k3', rv, 'p1');
      wire(kb, 'k4', v, 'p1');
    }),
  Brass: () =>
    make('Brass', (add, wire) => {
      const kb = add('fm1', ...L(0), { k1: 0.7519, k2: 0.1667, k3: 0.1987, k4: 0.5 });
      const a = add('osc', ...L(1), { wave: 2, fine: -6, level: 0.8 });
      const b = add('osc', ...L(1, 1), { wave: 2, fine: 6, level: 0.8 });
      const mix = add('mixer', ...L(2), { l1: 0.5, l2: 0.5 });
      const fe = add('adsr', ...L(2, 1), { a: 0.001, d: 0.3, s: 0.6, r: 0.2 }, { map: { p1: ['a'], p2: ['d'], k1: ['a'], k2: ['d'], k3: ['s'], k4: ['r'] }, att: [0.15, 1] });
      const flt = add('filter', ...L(3), { cutoff: 20, res: 0, cvamt: 2.2 }, { map: { p1: ['cutoff'], p2: ['res'], k1: ['cutoff'], k2: ['res'], k3: ['cvamt'], k4: ['track'] }, att: [1, 0.6] });
      const [v] = voice(add, wire, kb, flt, 'lp12', { a: 0.08, d: 0.2, s: 0.85, r: 0.2 }, 4, 0.5, undefined, { map: { p1: ['gain'], p2: [], k1: ['gain'], k2: [], k3: [], k4: [] }, att: [0.5, 1] });
      wire(kb, 'pitch', a, 'pitch');
      wire(kb, 'pitch', b, 'pitch');
      wire(kb, 'gate', fe, 'gate');
      wire(a, 'out', mix, 'in1');
      wire(b, 'out', mix, 'in2');
      wire(mix, 'out', flt, 'in');
      wire(fe, 'env', flt, 'cv');
      toOut(add, wire, v, 'out', 0.7, 5);
      wire(kb, 'k1', flt, 'p1');
      wire(kb, 'k2', flt, 'p2');
      wire(kb, 'k3', fe, 'p1');
      wire(kb, 'k4', v, 'p1');
    }),
  Strings: () =>
    make('Strings', (add, wire) => {
      const kb = add('fm1', ...L(0), { k1: 0.9419, k2: 0.5, k3: 0.3, k4: 0.35 });
      const mix = add('mixer', ...L(2), { l1: 0.4, l2: 0.4, l3: 0.4 });
      [-9, 0, 9].forEach((f, i) => {
        const o = add('osc', ...L(1, i), { wave: 2, fine: f, level: 0.8 });
        wire(kb, 'pitch', o, 'pitch');
        wire(o, 'out', mix, `in${i + 1}`);
      });
      const lfo = add('lfo', ...L(2, 1), { wave: 0, rate: 0.6, depth: 0 }, { map: { p1: ['depth'], p2: ['rate'], k1: ['wave'], k2: ['rate'], k3: ['depth'], k4: [] }, att: [0.6, 1] });
      const flt = add('filter', ...L(3), { cutoff: 20, res: 0.1, cvamt: 0.4 }, { map: { p1: ['cutoff'], p2: ['res'], k1: ['cutoff'], k2: ['res'], k3: ['cvamt'], k4: ['track'] }, att: [1, 1] });
      const [v] = voice(add, wire, kb, flt, 'lp12', { a: 0.5, d: 0.5, s: 0.9, r: 1 }, 4);
      const dly = add('delay', ...L(6), { time: 35, fb: 0.2, mix: 0 }, { map: { p1: ['mix'], p2: ['fb'], k1: ['time'], k2: ['fb'], k3: ['mix'], k4: [] }, att: [1, 1] }); // a short delay: chorus-like
      wire(mix, 'out', flt, 'in');
      wire(lfo, 'out', flt, 'cv');
      wire(v, 'out', dly, 'in');
      const rv = add('reverb', ...L(7), { size: 0.75, damp: 0.5, mix: 0 }, { map: { p1: ['mix'], p2: ['damp'], k1: ['size'], k2: ['damp'], k3: ['mix'], k4: [] }, att: [1, 1] });
      wire(dly, 'out', rv, 'in');
      toOut(add, wire, rv, 'out', 0.7, 8);
      wire(kb, 'k1', flt, 'p1');
      wire(kb, 'k2', lfo, 'p1');
      wire(kb, 'k3', rv, 'p1');
      wire(kb, 'k4', dly, 'p1');
    }),
  'Wobble 2': () =>
    make('Wobble 2', (add, wire) => {
      const kb = add('fm1', ...L(0), { k1: 0.6904, k2: 0.5, k3: 0.4986, k4: 0.5 });
      const osc = add('osc', ...L(1), { wave: 3, coarse: -12, level: 0.5 });
      const lfo = add('lfo', ...L(1, 1), { wave: 1, rate: 0.02, depth: 1 }, { map: { p1: ['rate'], p2: ['rate'], k1: ['wave'], k2: ['rate'], k3: ['depth'], k4: [] }, att: [0.3, 1] });
      const flt = add('filter', ...L(2), { cutoff: 20, res: 0.4, cvamt: 2.5 }, { map: { p1: ['cutoff'], p2: ['res'], k1: ['cutoff'], k2: ['res'], k3: ['cvamt'], k4: ['track'] }, att: [1, 0.6] });
      const [v] = voice(add, wire, kb, flt, 'lp12', { a: 0.003, d: 0.1, s: 1, r: 0.1 }, 3, 0.5, undefined, { map: { p1: ['gain'], p2: [], k1: ['gain'], k2: [], k3: [], k4: [] }, att: [0.5, 1] });
      wire(kb, 'pitch', osc, 'pitch');
      wire(osc, 'out', flt, 'in');
      wire(lfo, 'out', flt, 'cv');
      toOut(add, wire, v, 'out', 0.7, 5);
      wire(kb, 'k1', flt, 'p1');
      wire(kb, 'k2', flt, 'p2');
      wire(kb, 'k3', lfo, 'p1');
      wire(kb, 'k4', v, 'p1');
    }),
  'Echo Pluck': () =>
    make('Echo Pluck', (add, wire) => {
      const kb = add('fm1', ...L(0), { k1: 0.5, k2: 0.5, k3: 0.3321, k4: 0.5 });
      const osc = add('osc', ...L(1), { wave: 1, level: 0.65 }, { map: { p1: ['level'], p2: ['pw'], k1: ['wave'], k2: ['pw'], k3: ['coarse'], k4: ['fine'] }, att: [0.5, 1] });
      const [v, vEnv] = voice(add, wire, kb, osc, 'out', { a: 0.001, d: 0.001, s: 0, r: 0.1 }, 2, 1, { map: { p1: ['d'], p2: ['d'], k1: ['a'], k2: ['d'], k3: ['s'], k4: ['r'] }, att: [0.15, 1] });
      const dly = add('delay', ...L(4), { time: 110.25, fb: 0.6, mix: 0 }, { map: { p1: ['time'], p2: ['mix'], k1: ['time'], k2: ['fb'], k3: ['mix'], k4: [] }, att: [0.5, 1] });
      wire(kb, 'pitch', osc, 'pitch');
      wire(v, 'out', dly, 'in');
      toOut(add, wire, dly, 'out', 0.7, 5);
      wire(kb, 'k1', dly, 'p1');
      wire(kb, 'k2', osc, 'p1');
      wire(kb, 'k3', vEnv, 'p1');
      wire(kb, 'k4', dly, 'p2');
    }),
  'Dub Stab': () =>
    make('Dub Stab', (add, wire) => {
      const kb = add('fm1', ...L(0), { k1: 0.8192, k2: 0.5, k3: 0.5, k4: 0.45 });
      const a = add('osc', ...L(1), { wave: 2, level: 0.8 });
      const b = add('osc', ...L(1, 1), { wave: 3, coarse: 7, level: 0.6 });
      const mix = add('mixer', ...L(2), { l1: 0.5, l2: 0.5 });
      const fe = add('adsr', ...L(2, 1), { a: 0.001, d: 0.25, s: 0, r: 0.1 });
      const flt = add('filter', ...L(3), { cutoff: 20, res: 0.2, cvamt: 2 }, { map: { p1: ['cutoff'], p2: ['res'], k1: ['cutoff'], k2: ['res'], k3: ['cvamt'], k4: ['track'] }, att: [1, 0.6] });
      const [v] = voice(add, wire, kb, flt, 'lp12', { a: 0.001, d: 0.3, s: 0, r: 0.2 }, 4);
      const dly = add('delay', ...L(6), { time: 500, fb: 0.4125, mix: 0 }, { map: { p1: ['fb'], p2: ['mix'], k1: ['time'], k2: ['fb'], k3: ['mix'], k4: [] }, att: [0.5, 1] });
      wire(kb, 'pitch', a, 'pitch');
      wire(kb, 'pitch', b, 'pitch');
      wire(kb, 'gate', fe, 'gate');
      wire(a, 'out', mix, 'in1');
      wire(b, 'out', mix, 'in2');
      wire(mix, 'out', flt, 'in');
      wire(fe, 'env', flt, 'cv');
      wire(v, 'out', dly, 'in');
      toOut(add, wire, dly, 'out', 0.7, 7);
      wire(kb, 'k1', flt, 'p1');
      wire(kb, 'k2', flt, 'p2');
      wire(kb, 'k3', dly, 'p1');
      wire(kb, 'k4', dly, 'p2');
    }),
  'Noise Sweep': () =>
    make('Noise Sweep', (add, wire) => {
      const kb = add('fm1', ...L(0), { k1: 0.6592, k2: 0.5, k3: 0.5, k4: 0.5 });
      const nz = add('noise', ...L(1), { level: 0.7 });
      const fe = add('adsr', ...L(1, 1), { a: 0.7502, d: 1, s: 0.2, r: 1 }, { map: { p1: ['a'], p2: ['d'], k1: ['a'], k2: ['d'], k3: ['s'], k4: ['r'] }, att: [0.3, 1] });
      const flt = add('filter', ...L(2), { cutoff: 20, res: 0.5, cvamt: 4 }, { map: { p1: ['cutoff'], p2: ['res'], k1: ['cutoff'], k2: ['res'], k3: ['cvamt'], k4: ['track'] }, att: [1, 0.6] });
      const [v] = voice(add, wire, kb, flt, 'bp', { a: 0.3, d: 1, s: 0.8, r: 0.8 }, 3, 0.5, undefined, { map: { p1: ['gain'], p2: [], k1: ['gain'], k2: [], k3: [], k4: [] }, att: [0.5, 1] });
      wire(kb, 'gate', fe, 'gate');
      wire(nz, 'out', flt, 'in');
      wire(fe, 'env', flt, 'cv');
      toOut(add, wire, v, 'out', 0.8, 5);
      wire(kb, 'k1', flt, 'p1');
      wire(kb, 'k2', flt, 'p2');
      wire(kb, 'k3', fe, 'p1');
      wire(kb, 'k4', v, 'p1');
    }),
  Metal: () =>
    make('Metal', (add, wire) => {
      const kb = add('fm1', ...L(0), { k1: 0.5, k2: 0.5, k3: 0.5, k4: 0.5 });
      const mod = add('osc', ...L(1), { wave: 0, coarse: 7, fine: -33, level: 1 }, { map: { p1: ['fine'], p2: ['pw'], k1: ['wave'], k2: ['pw'], k3: ['coarse'], k4: ['fine'] }, att: [0.5, 1] });
      const car = add('osc', ...L(2), { wave: 0, pm: 4, level: 0.7 }, { map: { p1: ['pm'], p2: ['pw'], k1: ['wave'], k2: ['pw'], k3: ['coarse'], k4: ['fine'] }, att: [0.5, 1] });
      const [v, vEnv] = voice(add, wire, kb, car, 'out', { a: 0.001, d: 0.35, s: 0.1, r: 0.4 }, 3, 0.5, { map: { p1: ['d'], p2: ['d'], k1: ['a'], k2: ['d'], k3: ['s'], k4: ['r'] }, att: [0.1, 1] }, { map: { p1: ['gain'], p2: [], k1: ['gain'], k2: [], k3: [], k4: [] }, att: [0.5, 1] });
      wire(kb, 'pitch', mod, 'pitch');
      wire(kb, 'pitch', car, 'pitch');
      wire(mod, 'out', car, 'pm');
      toOut(add, wire, v, 'out', 0.7, 5);
      wire(kb, 'k1', car, 'p1');
      wire(kb, 'k2', mod, 'p1');
      wire(kb, 'k3', vEnv, 'p1');
      wire(kb, 'k4', v, 'p1');
    }),
  Drone: () =>
    make('Drone', (add, wire) => {
      // sounds without a key; the keys change the pitch
      const kb = add('fm1', ...L(0), { k1: 0.7794, k2: 0.5, k3: 0.05, k4: 0.4 });
      const a = add('osc', ...L(1), { wave: 2, coarse: -24, level: 0.8 });
      const b = add('osc', ...L(1, 1), { wave: 2, coarse: -17, level: 0.7 });
      const mix = add('mixer', ...L(2), { l1: 0.5, l2: 0.5 });
      const lfo = add('lfo', ...L(2, 1), { wave: 0, rate: 0.02, depth: 1 }, { map: { p1: ['rate'], p2: ['rate'], k1: ['wave'], k2: ['rate'], k3: ['depth'], k4: [] }, att: [0.05, 1] });
      const flt = add('filter', ...L(3), { cutoff: 20, res: 0, cvamt: 1 }, { map: { p1: ['cutoff'], p2: ['res'], k1: ['cutoff'], k2: ['res'], k3: ['cvamt'], k4: ['track'] }, att: [1, 0.6] });
      const dly = add('delay', ...L(4), { time: 700, fb: 0.5, mix: 0 }, { map: { p1: ['mix'], p2: ['fb'], k1: ['time'], k2: ['fb'], k3: ['mix'], k4: [] }, att: [1, 1] });
      wire(kb, 'pitch', a, 'pitch');
      wire(kb, 'pitch', b, 'pitch');
      wire(a, 'out', mix, 'in1');
      wire(b, 'out', mix, 'in2');
      wire(mix, 'out', flt, 'in');
      wire(lfo, 'out', flt, 'cv');
      wire(flt, 'lp12', dly, 'in');
      toOut(add, wire, dly, 'out', 0.6, 5);
      wire(kb, 'k1', flt, 'p1');
      wire(kb, 'k2', flt, 'p2');
      wire(kb, 'k3', lfo, 'p1');
      wire(kb, 'k4', dly, 'p1');
    }),
  'Tremolo Keys': () =>
    make('Tremolo Keys', (add, wire) => {
      const kb = add('fm1', ...L(0), { k1: 0.5, k2: 0.5, k3: 0.3738, k4: 0.5 });
      const osc = add('osc', ...L(1), { wave: 1, level: 0.9 });
      const [v, vEnv] = voice(add, wire, kb, osc, 'out', { a: 0.01, d: 0.6, s: 0.5, r: 0.001 }, 2, 0.5, { map: { p1: ['r'], p2: ['d'], k1: ['a'], k2: ['d'], k3: ['s'], k4: ['r'] }, att: [0.1, 1] }, { map: { p1: ['gain'], p2: [], k1: ['gain'], k2: [], k3: [], k4: [] }, att: [0.5, 1] });
      const lfo = add('lfo', ...L(2, 1), { wave: 0, rate: 2.0015, depth: 1 }, { map: { p1: ['rate'], p2: ['rate'], k1: ['wave'], k2: ['rate'], k3: ['depth'], k4: [] }, att: [0.15, 1] });
      const sc = add('scale', ...L(3, 1), { gain: 0, offset: 0.6 }, { map: { p1: ['gain'], p2: ['offset'], k1: ['gain'], k2: ['offset'], k3: [], k4: [] }, att: [0.1, 1] });
      const trem = add('vca', ...L(3), { gain: 1 });
      wire(kb, 'pitch', osc, 'pitch');
      wire(lfo, 'out', sc, 'in');
      wire(v, 'out', trem, 'in');
      wire(sc, 'out', trem, 'cv');
      toOut(add, wire, trem, 'out', 0.7, 5);
      wire(kb, 'k1', lfo, 'p1');
      wire(kb, 'k2', sc, 'p1');
      wire(kb, 'k3', vEnv, 'p1');
      wire(kb, 'k4', v, 'p1');
    }),
  'S&H Bleeps': () =>
    make('S&H Bleeps', (add, wire) => {
      const kb = add('fm1', ...L(0), { k1: 0.5, k2: 0.5, k3: 0.198, k4: 0.3 });
      const osc = add('osc', ...L(1), { wave: 3, level: 0.6 });
      const sh = add('lfo', ...L(1, 1), { wave: 4, rate: 4.002, depth: 1 }, { map: { p1: ['rate'], p2: ['rate'], k1: ['wave'], k2: ['rate'], k3: ['depth'], k4: [] }, att: [0.2, 1] });
      const sc = add('scale', ...L(2, 1), { gain: 0.1 }, { map: { p1: ['gain'], p2: ['offset'], k1: ['gain'], k2: ['offset'], k3: [], k4: [] }, att: [0.1, 1] });
      const [v, vEnv] = voice(add, wire, kb, osc, 'out', { a: 0.001, d: 0.001, s: 0.6, r: 0.1 }, 2, 1, { map: { p1: ['d'], p2: ['d'], k1: ['a'], k2: ['d'], k3: ['s'], k4: ['r'] }, att: [0.1, 1] });
      const dly = add('delay', ...L(4), { time: 190, fb: 0.45, mix: 0 }, { map: { p1: ['mix'], p2: ['fb'], k1: ['time'], k2: ['fb'], k3: ['mix'], k4: [] }, att: [1, 1] });
      wire(kb, 'pitch', osc, 'pitch');
      wire(sh, 'out', sc, 'in');
      wire(sc, 'out', osc, 'pitch');
      wire(v, 'out', dly, 'in');
      toOut(add, wire, dly, 'out', 0.7, 5);
      wire(kb, 'k1', sh, 'p1');
      wire(kb, 'k2', sc, 'p1');
      wire(kb, 'k3', vEnv, 'p1');
      wire(kb, 'k4', dly, 'p1');
    }),
  Glass: () =>
    make('Glass', (add, wire) => {
      const kb = add('fm1', ...L(0), { k1: 0.5, k2: 0.038, k3: 0.35, k4: 0.4 });
      const a = add('osc', ...L(1), { wave: 0, level: 0.6 });
      const b = add('osc', ...L(1, 1), { wave: 1, coarse: 12, level: 0.3 });
      const mix = add('mixer', ...L(2), { l1: 0.7, l2: 0 }, { map: { p1: ['l2'], p2: ['l2'], k1: ['l1'], k2: ['l2'], k3: ['l3'], k4: ['l4'] }, att: [1, 1] });
      const [v, vEnv] = voice(add, wire, kb, mix, 'out', { a: 0.001, d: 1.5, s: 0.3, r: 1.2 }, 3, 1, { map: { p1: ['a'], p2: ['d'], k1: ['a'], k2: ['d'], k3: ['s'], k4: ['r'] }, att: [0.1, 1] });
      const dly = add('delay', ...L(5), { time: 400, fb: 0.55, mix: 0 }, { map: { p1: ['mix'], p2: ['fb'], k1: ['time'], k2: ['fb'], k3: ['mix'], k4: [] }, att: [1, 1] });
      wire(kb, 'pitch', a, 'pitch');
      wire(kb, 'pitch', b, 'pitch');
      wire(a, 'out', mix, 'in1');
      wire(b, 'out', mix, 'in2');
      wire(v, 'out', dly, 'in');
      const rv = add('reverb', ...L(6), { size: 0.8, damp: 0.3, mix: 0 }, { map: { p1: ['mix'], p2: ['damp'], k1: ['size'], k2: ['damp'], k3: ['mix'], k4: [] }, att: [1, 1] });
      wire(dly, 'out', rv, 'in');
      toOut(add, wire, rv, 'out', 0.7, 7);
      wire(kb, 'k1', mix, 'p1');
      wire(kb, 'k2', vEnv, 'p1');
      wire(kb, 'k3', rv, 'p1');
      wire(kb, 'k4', dly, 'p1');
    }),
  'Fat Mono': () =>
    make('Fat Mono', (add, wire) => {
      const kb = add('fm1', ...L(0), { k1: 0.9216, k2: 0.3333, k3: 0.3987, k4: 0.5 });
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
      const fe = add('adsr', ...L(2, 1), { a: 0.005, d: 0.001, s: 0.4, r: 0.2 }, { map: { p1: ['d'], p2: ['d'], k1: ['a'], k2: ['d'], k3: ['s'], k4: ['r'] }, att: [0.15, 1] });
      const flt = add('filter', ...L(3), { cutoff: 20, res: 0, cvamt: 2 }, { map: { p1: ['cutoff'], p2: ['res'], k1: ['cutoff'], k2: ['res'], k3: ['cvamt'], k4: ['track'] }, att: [1, 0.6] });
      const [v] = voice(add, wire, kb, flt, 'lp12', { a: 0.005, d: 0.2, s: 0.9, r: 0.15 }, 4, 0.5, undefined, { map: { p1: ['gain'], p2: [], k1: ['gain'], k2: [], k3: [], k4: [] }, att: [0.5, 1] });
      wire(kb, 'gate', fe, 'gate');
      wire(mix, 'out', flt, 'in');
      wire(fe, 'env', flt, 'cv');
      toOut(add, wire, v, 'out', 0.7, 5);
      wire(kb, 'k1', flt, 'p1');
      wire(kb, 'k2', flt, 'p2');
      wire(kb, 'k3', fe, 'p1');
      wire(kb, 'k4', v, 'p1');
    }),
};
