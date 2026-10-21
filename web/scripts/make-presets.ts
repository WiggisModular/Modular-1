// SPDX-License-Identifier: GPL-3.0-only
// Writes the factory presets: web/presets/NN-name.json (NN = slot in the patch bank, the order the firmware compiles them in). Run from web/:
//   npx vite-node scripts/make-presets.ts      then      npm run export-defs      (regenerates firmware/src/presets.h)
// Every preset is a mono voice built from the same pieces (sources -> mixer -> filter -> VCA with an envelope -> delay -> reverb -> output) plus
// whatever makes it special, and routes the four panel knobs: K1 is the filter cutoff when there is a filter, K3 the reverb mix, K4 the delay
// mix (tests in src/engine.test.ts check these rules). The routed parameter's base is the bottom of its range and the knob carries the value
// (so the knobs start where the sound was designed). The output level is set so that every preset peaks at the same level.
import { mkdirSync, readdirSync, rmSync, writeFileSync } from 'node:fs';
import { Engine } from '../src/engine';
import { DEFS } from '../src/modules';
import { addModule, connect, emptyPatch, fromUnit, toUnit, type Patch } from '../src/patch';

type Params = Record<string, number>;
interface KnobOpts {
  base?: number;
  max?: number;
}

const COLX = [20, 250, 520, 790, 1060, 1330, 1600];
const PEAK = 0.5;

class B {
  p: Patch;
  reg: Record<string, number> = {};
  kb: number;
  private colY: number[] = COLX.map(() => 30);
  private pins = new Map<number, number>();
  private used = new Set<string>();
  constructor(name: string) {
    this.p = emptyPatch(name);
    this.kb = this.add('fm1', {}, 0, 'kb');
  }
  mod(id: number) {
    return this.p.modules.find((m) => m.id === id)!;
  }
  /** a module in column col (left to right) with these parameters, registered under `name` */
  add(type: string, params: Params = {}, col = 1, name?: string): number {
    const def = DEFS[type];
    const x = COLX[col];
    const y = this.colY[col];
    this.colY[col] += 110 + 42 * def.params.length;
    const m = addModule(this.p, def, Math.round(x * 0.77), Math.round(y * 0.77));
    Object.assign(m.params, params);
    if (name) this.reg[name] = m.id;
    return m.id;
  }
  w(a: number | string, ap: string, b: number | string, bp: string): void {
    const ai = typeof a === 'string' ? this.reg[a] : a;
    const bi = typeof b === 'string' ? this.reg[b] : b;
    if (!connect(this.p, DEFS, { module: ai, port: ap }, { module: bi, port: bp })) throw new Error(`${this.p.name}: bad cable ${a}.${ap} -> ${b}.${bp}`);
  }
  /** a sound source (osc or chip), played by the keys; a noise source has no pitch */
  src(type: 'osc' | 'chip' | 'noise', params: Params, name: string): number {
    const id = this.add(type, params, 1, name);
    if (type !== 'noise') this.w(this.kb, 'pitch', id, 'pitch');
    return id;
  }
  /** route panel knob n (1..4) to a parameter of the module `name` through its next free parameter input */
  k(n: number, name: string, param: string, o: KnobOpts = {}): boolean {
    const id = this.reg[name];
    if (id === undefined) return false;
    const m = this.mod(id);
    const pd = DEFS[m.type].params.find((q) => q.name === param);
    const slot = this.pins.get(id) ?? 0;
    if (!pd || slot > 1 || this.used.has(`${name}.${param}`)) return false;
    const value = m.params[param];
    const base = pd.log ? pd.min : o.base ?? pd.min;
    const max = pd.log ? pd.max : o.max ?? pd.max;
    const pin = slot === 0 ? 'p1' : 'p2';
    m.map![pin] = [param];
    m.att![slot] = pd.log ? 1 : (max - base) / (pd.max - pd.min);
    m.params[param] = base;
    this.mod(this.kb).params[`k${n}`] = pd.log ? Math.round(toUnit(pd, value) * 1e4) / 1e4 : Math.round(((value - base) / (max - base)) * 1e4) / 1e4;
    this.w(this.kb, `k${n}`, id, pin);
    this.pins.set(id, slot + 1);
    this.used.add(`${name}.${param}`);
    return true;
  }
  /** a cable from a signal into the next free parameter input of the module `name`, steering `param` (att: how far) */
  cv(src: number, port: string, name: string, param: string, att = 1): void {
    const id = this.reg[name];
    const slot = this.pins.get(id) ?? 0;
    if (slot > 1) throw new Error(`${this.p.name}: ${name} has no free parameter input`);
    const m = this.mod(id);
    m.map![slot === 0 ? 'p1' : 'p2'] = [param];
    m.att![slot] = att;
    this.w(src, port, id, slot === 0 ? 'p1' : 'p2');
    this.pins.set(id, slot + 1);
    this.used.add(`${name}.${param}`);
  }
  /** K1..K4: what is asked, else the first candidate that exists */
  knobs(ask: (string | undefined)[] = []): void {
    const cand: string[][] = [
      ['filter.cutoff', 'o0.wave', 'o0.shape', 'o0.pw', 'env.a', 'vca.gain'],
      ['filter.res', 'o0.pw', 'o0.shape', 'env.d', 'env.r', 'vca.gain'],
      ['reverb.mix', 'env.d', 'env.r', 'env.s', 'vca.gain'],
      ['delay.mix', 'env.r', 'env.a', 'env.s', 'vca.gain'],
    ];
    for (let n = 1; n <= 4; n++) {
      const want = ask[n - 1];
      for (const c of want ? [want, ...cand[n - 1]] : cand[n - 1]) {
        const [name, param] = c.split('.');
        if (this.k(n, name, param)) break;
      }
    }
    for (let n = 1; n <= 4; n++) if (!this.p.cables.some((c) => c.from.module === this.kb && c.from.port === `k${n}`)) throw new Error(`${this.p.name}: knob ${n} routes nowhere`);
  }
}

interface Std {
  filter?: { cutoff: number; res?: number; out?: string; cvamt?: number; fenv?: number[]; track?: number; morph?: number; lfo?: { rate: number; depth: number; wave?: number } };
  env: number[];
  gain?: number;
  delay?: { time: number; fb: number; mix: number; pingpong?: number };
  reverb?: { size: number; damp: number; mix: number };
}

/** sources -> (mixer) -> (filter) -> VCA <- ADSR -> (delay) -> (reverb) -> output */
function std(b: B, srcs: { id: number; lvl?: number }[], o: Std): void {
  const adsr = (e: number[]): Params => ({ a: e[0], d: e[1], s: e[2], r: e[3] });
  let sig: number;
  let sigPort = 'out';
  if (srcs.length > 1) {
    const mix = b.add('mixer', Object.fromEntries(srcs.map((s, i) => [`l${i + 1}`, s.lvl ?? 0.5])), 2, 'mix');
    srcs.forEach((s, i) => b.w(s.id, 'out', mix, `in${i + 1}`));
    sig = mix;
  } else sig = srcs[0].id;
  if (o.filter) {
    const f = o.filter;
    const flt = b.add('filter', { cutoff: f.cutoff, res: f.res ?? 0.2, cvamt: f.cvamt ?? 1, track: f.track ?? 0, morph: f.morph ?? 0 }, 3, 'filter');
    b.w(sig, sigPort, flt, 'in');
    if (f.fenv) {
      const fe = b.add('adsr', adsr(f.fenv), 3, 'fenv');
      b.w(b.kb, 'gate', fe, 'gate');
      b.w(fe, 'env', flt, 'cv');
    }
    if (f.lfo) {
      const l = b.add('lfo', { wave: f.lfo.wave ?? 0, rate: f.lfo.rate, depth: f.lfo.depth }, 2, 'lfo');
      b.w(l, 'out', flt, 'cv');
    }
    sig = flt;
    sigPort = f.out ?? 'lp12';
  }
  const env = b.add('adsr', adsr(o.env), 4, 'env');
  b.w(b.kb, 'gate', env, 'gate');
  const vca = b.add('vca', { gain: o.gain ?? 1 }, 4, 'vca');
  b.w(sig, sigPort, vca, 'in');
  b.w(env, 'env', vca, 'cv');
  sig = vca;
  if (o.delay) {
    const d = b.add('delay', { time: o.delay.time, fb: o.delay.fb, mix: o.delay.mix, pingpong: o.delay.pingpong ?? 0 }, 5, 'delay');
    b.w(sig, 'out', d, 'in');
    sig = d;
  }
  if (o.reverb) {
    const r = b.add('reverb', o.reverb, 5, 'reverb');
    b.w(sig, 'out', r, 'in');
    sig = r;
  }
  const out = b.add('output', { level: 0.7 }, 6, 'out');
  b.w(sig, 'out', out, 'l');
}

/** a slow LFO into the pitch of the sources `names` (vibrato), its depth on a knob-sized scale */
function vibrato(b: B, names: string[], rate: number, depth: number): void {
  const l = b.add('lfo', { wave: 0, rate, depth: 1 }, 2, 'vlfo');
  const sc = b.add('scale', { gain: depth }, 2, 'vib');
  b.w(l, 'out', sc, 'in');
  for (const n of names) b.w(sc, 'out', n, 'pitch');
}

type Spec = (b: B) => string[] | void;
const SPECS: [string, Spec][] = [];
const P = (name: string, f: Spec): void => void SPECS.push([name, f]);

const SAW = 2, SQR = 3, TRI = 1, SIN = 0;

// ---- slot 0: the one the docs measure
P('303 Acid', (b) => {
  const o = b.src('osc', { wave: SAW, coarse: -12, level: 0.8 }, 'o0');
  std(b, [{ id: o }], { filter: { cutoff: 250, res: 0.8, out: 'lp24', cvamt: 2.4, fenv: [0.001, 0.22, 0, 0.1] }, env: [0.001, 0.25, 0.4, 0.08], delay: { time: 214, fb: 0.4, mix: 0.2 } });
  return ['filter.cutoff', 'filter.res', 'fenv.d', 'delay.mix'];
});
// ---- leads
P('Bright Saw', (b) => {
  const a = b.src('osc', { wave: SAW, fine: -6, level: 0.8 }, 'o0');
  const c = b.src('osc', { wave: SAW, fine: 6, level: 0.8 }, 'o1');
  std(b, [{ id: a, lvl: 0.5 }, { id: c, lvl: 0.5 }], { filter: { cutoff: 3200, res: 0.25, fenv: [0.005, 0.3, 0.3, 0.2], cvamt: 0.4 }, env: [0.005, 0.2, 0.75, 0.15], delay: { time: 330, fb: 0.3, mix: 0.18 } });
});
P('Pulse Lead', (b) => {
  const a = b.src('osc', { wave: SQR, pw: 0.3, level: 0.7 }, 'o0');
  const l = b.add('lfo', { wave: 1, rate: 0.7, depth: 1 }, 2, 'lfo');
  b.cv(l, 'out', 'o0', 'pw', 0.2);
  std(b, [{ id: a }], { filter: { cutoff: 2600, res: 0.2, out: 'lp24' }, env: [0.01, 0.15, 0.8, 0.2], delay: { time: 280, fb: 0.35, mix: 0.2 } });
  return ['filter.cutoff', 'lfo.rate', 'reverb.mix', 'delay.mix'];
});
P('Fat Unison', (b) => {
  const s = [-12, 0, 12].map((f, i) => b.src('osc', { wave: SAW, fine: f, level: 0.8 }, `o${i}`));
  std(b, s.map((id) => ({ id, lvl: 0.4 })), { filter: { cutoff: 2000, res: 0.15, fenv: [0.01, 0.5, 0.4, 0.3], cvamt: 0.6 }, env: [0.01, 0.2, 0.8, 0.25], reverb: { size: 0.4, damp: 0.5, mix: 0.15 } });
});
P('Soft Lead', (b) => {
  const a = b.src('osc', { wave: TRI, level: 0.8 }, 'o0');
  const c = b.src('osc', { wave: SIN, coarse: 12, level: 0.5 }, 'o1');
  vibrato(b, ['o0', 'o1'], 5.2, 0.012);
  std(b, [{ id: a, lvl: 0.6 }, { id: c, lvl: 0.3 }], { env: [0.04, 0.25, 0.8, 0.3], reverb: { size: 0.5, damp: 0.4, mix: 0.2 }, delay: { time: 360, fb: 0.3, mix: 0.12 } });
  return ['o0.wave', 'vib.gain', 'reverb.mix', 'delay.mix'];
});
P('Glass Lead', (b) => {
  const a = b.src('osc', { wave: SIN, level: 0.8 }, 'o0');
  const m = b.src('osc', { wave: SIN, coarse: 19, level: 0.7 }, 'o1');
  b.w(m, 'out', a, 'pm');
  b.mod(a).params.pm = 0.9;
  std(b, [{ id: a }], { env: [0.003, 0.5, 0.35, 0.5], reverb: { size: 0.6, damp: 0.3, mix: 0.25 }, delay: { time: 300, fb: 0.35, mix: 0.2 } });
  return ['o0.pm', 'env.d', 'reverb.mix', 'delay.mix'];
});
// ---- basses
P('Sub Bass', (b) => {
  const a = b.src('osc', { wave: SIN, coarse: -12, level: 0.9 }, 'o0');
  const c = b.src('osc', { wave: SQR, coarse: -12, pw: 0.5, level: 0.5 }, 'o1');
  std(b, [{ id: a, lvl: 0.7 }, { id: c, lvl: 0.3 }], { filter: { cutoff: 420, res: 0.1, out: 'lp24', track: 1 }, env: [0.004, 0.2, 0.85, 0.1] });
});
P('Rubber Bass', (b) => {
  const a = b.src('osc', { wave: SAW, coarse: -12, level: 0.8 }, 'o0');
  std(b, [{ id: a }], { filter: { cutoff: 160, res: 0.35, out: 'lp24', fenv: [0.001, 0.18, 0, 0.1], cvamt: 2.2 }, env: [0.001, 0.3, 0.5, 0.1] });
  return ['filter.cutoff', 'filter.res', 'fenv.d', 'env.r'];
});
P('Wobble Bass', (b) => {
  const a = b.src('osc', { wave: SAW, coarse: -12, level: 0.7 }, 'o0');
  const c = b.src('osc', { wave: SQR, coarse: -24, pw: 0.5, level: 0.6 }, 'o1');
  std(b, [{ id: a, lvl: 0.5 }, { id: c, lvl: 0.5 }], { filter: { cutoff: 220, res: 0.45, out: 'lp24', cvamt: 1.6, lfo: { rate: 2.5, depth: 1 } }, env: [0.005, 0.1, 0.9, 0.12], gain: 0.9 });
  return ['filter.cutoff', 'lfo.rate', 'filter.res', 'env.r'];
});
P('Reese', (b) => {
  const s = [-14, 14].map((f, i) => b.src('osc', { wave: SAW, coarse: -12, fine: f, level: 0.8 }, `o${i}`));
  std(b, s.map((id) => ({ id, lvl: 0.5 })), { filter: { cutoff: 520, res: 0.25, out: 'lp24' }, env: [0.01, 0.2, 0.9, 0.2] });
  return ['filter.cutoff', 'filter.res', 'env.a', 'env.r'];
});
P('Phase Bass', (b) => {
  const a = b.src('osc', { wave: SIN, coarse: -12, level: 0.9 }, 'o0');
  const m = b.src('osc', { wave: SIN, coarse: -12, level: 0.8 }, 'o1');
  b.w(m, 'out', a, 'pm');
  b.mod(a).params.pm = 1.6;
  std(b, [{ id: a }], { env: [0.002, 0.25, 0.6, 0.1], filter: { cutoff: 1800, res: 0.1, track: 1 } });
  return ['filter.cutoff', 'o0.pm', 'env.d', 'env.r'];
});
// ---- pads
P('Warm Pad', (b) => {
  const s = [-10, 0, 10].map((f, i) => b.src('osc', { wave: SAW, fine: f, level: 0.8 }, `o${i}`));
  std(b, s.map((id) => ({ id, lvl: 0.35 })), { filter: { cutoff: 1100, res: 0.15, lfo: { rate: 0.2, depth: 0.5 }, cvamt: 0.5 }, env: [0.7, 0.5, 0.85, 1.4], reverb: { size: 0.75, damp: 0.5, mix: 0.35 }, delay: { time: 420, fb: 0.35, mix: 0.12 } });
});
P('PWM Pad', (b) => {
  const a = b.src('osc', { wave: SQR, pw: 0.5, level: 0.7 }, 'o0');
  const c = b.src('osc', { wave: SQR, pw: 0.5, fine: 8, level: 0.7 }, 'o1');
  const l = b.add('lfo', { wave: 0, rate: 0.35, depth: 1 }, 2, 'lfo');
  b.cv(l, 'out', 'o0', 'pw', 0.3);
  b.cv(l, 'out', 'o1', 'pw', 0.3);
  std(b, [{ id: a, lvl: 0.45 }, { id: c, lvl: 0.45 }], { filter: { cutoff: 1800, res: 0.1 }, env: [0.5, 0.4, 0.85, 1.2], reverb: { size: 0.7, damp: 0.5, mix: 0.3 } });
  return ['filter.cutoff', 'lfo.rate', 'reverb.mix', 'env.a'];
});
P('Glass Pad', (b) => {
  const a = b.src('osc', { wave: TRI, level: 0.8 }, 'o0');
  const c = b.src('osc', { wave: SIN, coarse: 12, fine: 5, level: 0.7 }, 'o1');
  const d = b.src('osc', { wave: SIN, coarse: 19, level: 0.5 }, 'o2');
  std(b, [{ id: a, lvl: 0.4 }, { id: c, lvl: 0.35 }, { id: d, lvl: 0.25 }], { filter: { cutoff: 3000, res: 0.1, out: 'lp12', lfo: { rate: 0.15, depth: 0.3 }, cvamt: 0.4 }, env: [0.9, 0.5, 0.8, 1.8], reverb: { size: 0.9, damp: 0.25, mix: 0.45 } });
  return ['filter.cutoff', 'filter.res', 'reverb.mix', 'env.a'];
});
P('String Machine', (b) => {
  const s = [-9, 0, 9].map((f, i) => b.src('osc', { wave: SAW, fine: f, level: 0.8 }, `o${i}`));
  vibrato(b, ['o0', 'o1', 'o2'], 5.8, 0.004);
  std(b, s.map((id) => ({ id, lvl: 0.4 })), { filter: { cutoff: 1500, res: 0.1 }, env: [0.35, 0.4, 0.9, 0.7], reverb: { size: 0.7, damp: 0.5, mix: 0.3 }, delay: { time: 38, fb: 0.2, mix: 0.2 } });
  return ['filter.cutoff', 'vib.gain', 'reverb.mix', 'delay.mix'];
});
P('Breath Pad', (b) => {
  const n = b.src('noise', { level: 1 }, 'n0');
  const a = b.src('osc', { wave: TRI, level: 0.9 }, 'o0');
  std(b, [{ id: n, lvl: 0.9 }, { id: a, lvl: 0.6 }], { filter: { cutoff: 900, res: 0.35, out: 'bp', lfo: { rate: 0.25, depth: 0.4 }, cvamt: 0.8 }, env: [0.8, 0.6, 0.8, 1.5], reverb: { size: 0.85, damp: 0.4, mix: 0.4 } });
  return ['filter.cutoff', 'filter.res', 'reverb.mix', 'env.a'];
});
// ---- keys and plucks
P('Pluck', (b) => {
  const a = b.src('osc', { wave: SAW, level: 0.8 }, 'o0');
  std(b, [{ id: a }], { filter: { cutoff: 300, res: 0.1, fenv: [0.001, 0.001, 0, 0.1], cvamt: 3 }, env: [0.001, 0.35, 0, 0.2], delay: { time: 300, fb: 0.35, mix: 0.2 } });
  return ['filter.cutoff', 'filter.res', 'fenv.a', 'delay.mix'];
});
P('Mallet', (b) => {
  const a = b.src('osc', { wave: SIN, level: 0.9 }, 'o0');
  const c = b.src('osc', { wave: TRI, coarse: 12, level: 0.6 }, 'o1');
  const d = b.src('osc', { wave: SIN, coarse: 28, level: 0.4 }, 'o2');
  std(b, [{ id: a, lvl: 0.6 }, { id: c, lvl: 0.3 }, { id: d, lvl: 0.2 }], { env: [0.001, 0.45, 0, 0.3], reverb: { size: 0.5, damp: 0.5, mix: 0.2 }, delay: { time: 250, fb: 0.25, mix: 0.1 } });
  return ['o0.wave', 'env.d', 'reverb.mix', 'delay.mix'];
});
P('E-Piano', (b) => {
  const a = b.src('osc', { wave: SIN, level: 0.9 }, 'o0');
  const m = b.src('osc', { wave: SIN, coarse: 12, level: 0.8 }, 'o1');
  const mv = b.add('vca', { gain: 1 }, 2, 'mvca');
  const me = b.add('adsr', { a: 0.001, d: 0.5, s: 0, r: 0.4 }, 2, 'menv');
  b.w(m, 'out', mv, 'in');
  b.w(b.kb, 'gate', me, 'gate');
  b.w(me, 'env', mv, 'cv');
  b.w(mv, 'out', a, 'pm');
  b.mod(a).params.pm = 1.1;
  std(b, [{ id: a }], { env: [0.002, 1.4, 0.15, 0.5], reverb: { size: 0.5, damp: 0.5, mix: 0.2 }, delay: { time: 300, fb: 0.2, mix: 0.1 } });
  return ['o0.pm', 'env.d', 'reverb.mix', 'delay.mix'];
});
P('Organ', (b) => {
  const s = [0, 12, 19, 24].map((c, i) => b.src('osc', { wave: SIN, coarse: c, level: 0.8 }, `o${i}`));
  std(b, s.map((id, i) => ({ id, lvl: [0.6, 0.5, 0.3, 0.25][i] })), { env: [0.005, 0.1, 1, 0.06], reverb: { size: 0.4, damp: 0.5, mix: 0.2 } });
  return ['mix.l2', 'mix.l3', 'reverb.mix', 'mix.l4'];
});
P('Clav', (b) => {
  const a = b.src('osc', { wave: SQR, pw: 0.18, level: 0.8 }, 'o0');
  std(b, [{ id: a }], { filter: { cutoff: 700, res: 0.3, out: 'bp', fenv: [0.001, 0.12, 0, 0.1], cvamt: 2 }, env: [0.001, 0.25, 0, 0.12], delay: { time: 180, fb: 0.25, mix: 0.12 } });
  return ['filter.cutoff', 'filter.res', 'fenv.d', 'delay.mix'];
});
P('FM Bell', (b) => {
  const a = b.src('osc', { wave: SIN, coarse: 19, level: 1 }, 'o0');
  const m = b.src('osc', { wave: SIN, level: 0.8 }, 'o1');
  const mv = b.add('vca', { gain: 1 }, 2, 'mvca');
  const me = b.add('adsr', { a: 0.001, d: 1.6, s: 0, r: 1.2 }, 2, 'menv');
  b.w(a, 'out', mv, 'in');
  b.w(b.kb, 'gate', me, 'gate');
  b.w(me, 'env', mv, 'cv');
  b.w(mv, 'out', m, 'pm');
  b.mod(m).params.pm = 1.2;
  std(b, [{ id: m }], { env: [0.001, 2.2, 0, 1.6], reverb: { size: 0.75, damp: 0.3, mix: 0.3 }, delay: { time: 330, fb: 0.35, mix: 0.15 } });
  return ['o1.pm', 'env.d', 'reverb.mix', 'delay.mix'];
});
// ---- effects and textures
P('Zap', (b) => {
  const a = b.src('osc', { wave: TRI, level: 0.8 }, 'o0');
  const pe = b.add('adsr', { a: 0.001, d: 0.14, s: 0, r: 0.1 }, 2, 'penv');
  const sc = b.add('scale', { gain: 2.5 }, 2, 'sweep');
  b.w(b.kb, 'gate', pe, 'gate');
  b.w(pe, 'env', sc, 'in');
  b.w(sc, 'out', a, 'pitch');
  std(b, [{ id: a }], { env: [0.001, 0.25, 0, 0.1], delay: { time: 160, fb: 0.4, mix: 0.2 } });
  return ['penv.d', 'sweep.gain', 'env.d', 'delay.mix'];
});
P('Siren', (b) => {
  const a = b.src('osc', { wave: SIN, coarse: 12, level: 0.8 }, 'o0');
  const l = b.add('lfo', { wave: 1, rate: 0.8, depth: 1 }, 2, 'lfo');
  const sc = b.add('scale', { gain: 0.45 }, 2, 'sweep');
  b.w(l, 'out', sc, 'in');
  b.w(sc, 'out', a, 'pitch');
  std(b, [{ id: a }], { env: [0.2, 0.2, 1, 0.5], reverb: { size: 0.5, damp: 0.5, mix: 0.15 } });
  return ['lfo.rate', 'sweep.gain', 'reverb.mix', 'env.r'];
});
P('S&H Bleeps', (b) => {
  const a = b.src('osc', { wave: SQR, pw: 0.5, level: 0.6 }, 'o0');
  const l = b.add('lfo', { wave: 4, rate: 6, depth: 1 }, 2, 'lfo');
  const sc = b.add('scale', { gain: 0.35 }, 2, 'step');
  b.w(l, 'out', sc, 'in');
  b.w(sc, 'out', a, 'pitch');
  std(b, [{ id: a }], { env: [0.001, 0.12, 0.3, 0.1], delay: { time: 190, fb: 0.45, mix: 0.3, pingpong: 1 } });
  return ['lfo.rate', 'step.gain', 'env.d', 'delay.mix'];
});
P('Wind', (b) => {
  const n = b.src('noise', { level: 0.8 }, 'n0');
  std(b, [{ id: n }], { filter: { cutoff: 700, res: 0.55, out: 'bp', lfo: { rate: 0.2, depth: 0.6 }, cvamt: 1 }, env: [1.2, 0.5, 1, 2], reverb: { size: 0.8, damp: 0.4, mix: 0.4 }, delay: { time: 500, fb: 0.4, mix: 0.15 } });
  return ['filter.cutoff', 'filter.res', 'reverb.mix', 'delay.mix'];
});
P('Metal Drone', (b) => {
  const a = b.src('osc', { wave: SQR, pw: 0.5, level: 0.6 }, 'o0');
  const c = b.src('osc', { wave: SQR, pw: 0.4, coarse: 7, fine: 33, level: 0.6 }, 'o1');
  const d = b.src('osc', { wave: TRI, coarse: -12, fine: -17, level: 0.7 }, 'o2');
  std(b, [{ id: a, lvl: 0.3 }, { id: c, lvl: 0.3 }, { id: d, lvl: 0.35 }], { filter: { cutoff: 1400, res: 0.5, out: 'bp', lfo: { rate: 0.1, depth: 0.7 }, cvamt: 1 }, env: [0.6, 0.5, 1, 1.5], reverb: { size: 0.8, damp: 0.3, mix: 0.35 }, delay: { time: 620, fb: 0.5, mix: 0.2 } });
  return ['filter.cutoff', 'filter.res', 'reverb.mix', 'delay.mix'];
});
P('Noise Sweep', (b) => {
  const n = b.src('noise', { level: 1 }, 'n0');
  std(b, [{ id: n }], { filter: { cutoff: 150, res: 0.45, out: 'lp24', fenv: [1.2, 1.6, 0, 1], cvamt: 3.2 }, env: [0.3, 1.5, 0.4, 1.2], reverb: { size: 0.7, damp: 0.5, mix: 0.25 } });
  return ['filter.cutoff', 'filter.res', 'reverb.mix', 'fenv.a'];
});
// ---- the chip oscillator
P('NES Arp Lead', (b) => {
  const c = b.src('chip', { chip: 0, shape: 1, arp: 1, chord: 1, speed: 60, level: 0.7 }, 'o0');
  std(b, [{ id: c }], { env: [0.003, 0.15, 0.6, 0.08] });
  return ['o0.shape', 'o0.chord', 'env.d', 'env.r'];
});
P('NES Triangle Bass', (b) => {
  const c = b.src('chip', { chip: 0, shape: 4, coarse: -12, level: 0.9 }, 'o0');
  std(b, [{ id: c }], { env: [0.002, 0.2, 0.8, 0.05] });
  return ['o0.chip', 'env.d', 'env.r', 'vca.gain'];
});
P('GB Wave Lead', (b) => {
  const c = b.src('chip', { chip: 1, shape: 4, level: 0.8 }, 'o0');
  vibrato(b, ['o0'], 6, 0.01);
  std(b, [{ id: c }], { env: [0.004, 0.2, 0.7, 0.1], delay: { time: 250, fb: 0.3, mix: 0.15 } });
  return ['o0.shape', 'vib.gain', 'env.r', 'delay.mix'];
});
P('GB Arp Minor', (b) => {
  const c = b.src('chip', { chip: 1, shape: 2, arp: 1, chord: 7, speed: 30, level: 0.7 }, 'o0');
  std(b, [{ id: c }], { env: [0.002, 0.25, 0.7, 0.1], delay: { time: 214, fb: 0.35, mix: 0.2 } });
  return ['o0.shape', 'o0.speed', 'env.r', 'delay.mix'];
});

/** the loudest sample while a note plays and after it (a held C3, then released), at 48 kHz */
function peakOf(p: Patch): number {
  const e = new Engine(48000);
  e.load(structuredClone(p));
  e.noteOn(48);
  let peak = 0;
  for (let i = 0; i < 24; i++) {
    const l = new Float32Array(4800);
    const r = new Float32Array(4800);
    e.render(l, r);
    for (const v of [...l, ...r]) peak = Math.max(peak, Math.abs(v));
    if (i === 15) e.noteOff(48);
  }
  return peak;
}

const dir = new URL('../presets/', import.meta.url);
mkdirSync(dir, { recursive: true });
for (const f of readdirSync(dir)) if (f.endsWith('.json')) rmSync(new URL(f, dir));
const slug = (s: string): string => s.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '');
const report: string[] = [];
SPECS.forEach(([name, f], i) => {
  const b = new B(name);
  const ask = f(b) ?? [];
  b.knobs(ask);
  const out = b.mod(b.reg.out);
  const vca = b.mod(b.reg.vca);
  const first = peakOf(b.p);
  const want = (out.params.level * PEAK) / Math.max(first, 1e-4);        // the output level that gives the common peak
  out.params.level = Math.min(1, Math.round(want * 100) / 100);
  if (want > 1 && !b.p.cables.some((c) => c.to.module === vca.id && c.to.port.startsWith('p'))) vca.params.gain = Math.min(2, Math.round(vca.params.gain * want * 100) / 100);   // still too quiet: the VCA makes up the rest
  const after = peakOf(b.p);
  report.push(`${String(i).padStart(2, '0')} ${name.padEnd(18)} peak ${first.toFixed(2)} -> ${after.toFixed(2)} level ${out.params.level} gain ${vca.params.gain}`);
  writeFileSync(new URL(`${String(i).padStart(2, '0')}-${slug(name)}.json`, dir), JSON.stringify(b.p, null, 1) + '\n');
});
console.log(report.join('\n'));
if (SPECS.length > 32) throw new Error('the patch bank has 32 slots');
