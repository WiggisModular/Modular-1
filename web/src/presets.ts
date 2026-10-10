// SPDX-License-Identifier: GPL-3.0-only
import { DEFS } from './modules';
import { addModule, connect, emptyPatch, type Patch } from './patch';

export type Build = (add: (type: string, x: number, y: number, params?: Record<string, number>) => number, wire: (a: number, ap: string, b: number, bp: string) => void) => void;

export function make(name: string, build: Build): Patch {
  const p = emptyPatch(name);
  const add = (type: string, x: number, y: number, params: Record<string, number> = {}): number => {
    const m = addModule(p, DEFS[type], x, y);
    Object.assign(m.params, params);
    return m.id;
  };
  const wire = (a: number, ap: string, b: number, bp: string): void => {
    if (!connect(p, DEFS, { module: a, port: ap }, { module: b, port: bp })) throw new Error(`preset ${name}: bad cable ${a}.${ap} -> ${b}.${bp}`);
  };
  build(add, wire);
  return p;
}

export const PRESETS: Record<string, () => Patch> = {
  '303 Acid': () =>
    make('303 Acid', (add, wire) => {
      const kb = add('midi', 20, 40);
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
      const kb = add('midi', 20, 40);
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
      const kb = add('midi', 20, 120);
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
      const kb = add('midi', 20, 40);
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
};
