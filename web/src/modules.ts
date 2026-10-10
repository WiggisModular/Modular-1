// SPDX-License-Identifier: GPL-3.0-only
// The module library: definitions (shared with the editor) and DSP implementations.

import type { ModuleDef, ParamDef, PortDef } from './patch';

export interface Voice {
  /** octaves relative to middle C (MIDI 60) */
  pitch: number;
  gate: number;
  velocity: number;
}

export interface Ctx {
  sr: number;
  n: number;
  inp: Record<string, Float32Array>;
  out: Record<string, Float32Array>;
  connected: Record<string, boolean>;
  p: Record<string, number>;
  voice: Voice;
}

export interface ModuleImpl<S = any> {
  def: ModuleDef;
  init(sr: number): S;
  process(c: Ctx, st: S): void;
}

const audio = (name: string): PortDef => ({ name, kind: 'audio' });
const control = (name: string): PortDef => ({ name, kind: 'control' });
const gate = (name: string): PortDef => ({ name, kind: 'gate' });
const param = (name: string, min: number, max: number, def: number, extra: Partial<ParamDef> = {}): ParamDef => ({
  name,
  min,
  max,
  default: def,
  ...extra,
});

const WAVES = ['sine', 'triangle', 'saw', 'square'];
const C4 = 261.6255653;

function polyBlep(t: number, dt: number): number {
  if (t < dt) {
    t /= dt;
    return t + t - t * t - 1;
  }
  if (t > 1 - dt) {
    t = (t - 1) / dt;
    return t * t + t + t + 1;
  }
  return 0;
}

function wave(kind: number, phase: number, dt: number, pw = 0.5): number {
  switch (kind) {
    case 0:
      return Math.sin(2 * Math.PI * phase);
    case 1:
      return 4 * Math.abs(phase - Math.floor(phase + 0.5)) - 1;
    case 2:
      return 2 * phase - 1 - polyBlep(phase, dt);
    default: {
      let v = phase < pw ? 1 : -1;
      v += polyBlep(phase, dt);
      v -= polyBlep((phase + 1 - pw) % 1, dt);
      return v;
    }
  }
}

const midi: ModuleImpl<null> = {
  def: { type: 'midi', label: 'MIDI in', inputs: [], outputs: [control('pitch'), gate('gate'), control('vel')], params: [] },
  init: () => null,
  process(c) {
    c.out.pitch.fill(c.voice.pitch, 0, c.n);
    c.out.gate.fill(c.voice.gate, 0, c.n);
    c.out.vel.fill(c.voice.velocity, 0, c.n);
  },
};

/**
 * The FM-1 front panel as a module: the 27 note keys (pitch/gate/velocity, octave buttons applied),
 * and encoders 1-4 as 0..1 values. In the browser the panel is the module's own sliders; on the
 * device the encoders feed the same outputs.
 */
const N_KNOBS = 4;
const knobNames = Array.from({ length: N_KNOBS }, (_, i) => `k${i + 1}`);

const fm1: ModuleImpl<null> = {
  def: {
    type: 'fm1',
    label: 'FM-1 panel',
    inputs: [],
    outputs: [control('pitch'), gate('gate'), control('vel'), ...knobNames.map(control)],
    params: [...knobNames.map((n) => param(n, 0, 1, 0.5, { step: 0.01 }))],
  },
  init: () => null,
  process(c) {
    c.out.pitch.fill(c.voice.pitch, 0, c.n);
    c.out.gate.fill(c.voice.gate, 0, c.n);
    c.out.vel.fill(c.voice.velocity, 0, c.n);
    for (const n of knobNames) c.out[n].fill(c.p[n] ?? 0, 0, c.n);
  },
};

const osc: ModuleImpl<{ phase: number }> = {
  def: {
    type: 'osc',
    label: 'Oscillator',
    inputs: [control('pitch'), audio('pm')],
    outputs: [audio('out')],
    params: [
      param('wave', 0, 3, 0, { step: 1, choices: WAVES }),
      param('pw', 0.05, 0.95, 0.5, { step: 0.01 }),
      param('coarse', -48, 48, 0, { step: 1, unit: 'st' }),
      param('fine', -100, 100, 0, { step: 1, unit: 'ct' }),
      param('pm', 0, 8, 0, { step: 0.01 }),
      param('level', 0, 1, 0.8, { step: 0.01 }),
    ],
  },
  init: () => ({ phase: 0 }),
  process(c, s) {
    const { pitch, pm } = c.inp;
    const pw = Math.min(0.95, Math.max(0.05, c.p.pw ?? 0.5));
    const base = (c.p.coarse ?? 0) / 12 + (c.p.fine ?? 0) / 1200;
    const kind = Math.round(c.p.wave ?? 0);
    const idx = c.p.pm ?? 0;
    const lvl = c.p.level ?? 1;
    const out = c.out.out;
    for (let i = 0; i < c.n; i++) {
      const f = C4 * Math.pow(2, pitch[i] + base);
      const dt = Math.min(f / c.sr, 0.45);
      s.phase += dt;
      if (s.phase >= 1) s.phase -= 1;
      let ph = s.phase;
      if (idx > 0) {
        ph += (idx * pm[i]) / (2 * Math.PI);
        ph -= Math.floor(ph);
      }
      out[i] = wave(kind, ph, dt, pw) * lvl;
    }
  },
};

const LFO_WAVES = [...WAVES, 'S&H'];

const lfo: ModuleImpl<{ phase: number; seed: number; held: number }> = {
  def: {
    type: 'lfo',
    label: 'LFO',
    inputs: [],
    outputs: [control('out')],
    params: [param('wave', 0, 4, 0, { step: 1, choices: LFO_WAVES }), param('rate', 0.02, 40, 4, { unit: 'Hz', step: 0.01 }), param('depth', 0, 1, 1, { step: 0.01 })],
  },
  init: () => ({ phase: 0, seed: 0x2545f491, held: 0 }),
  process(c, s) {
    const dt = (c.p.rate ?? 1) / c.sr;
    const kind = Math.round(c.p.wave ?? 0);
    const depth = c.p.depth ?? 1;
    for (let i = 0; i < c.n; i++) {
      s.phase += dt;
      if (s.phase >= 1) {
        s.phase -= 1;
        // sample & hold: new random value (-1..1) at every cycle start
        s.seed = (Math.imul(s.seed, 1664525) + 1013904223) >>> 0;
        s.held = s.seed / 0x80000000 - 1;
      }
      c.out.out[i] = (kind === 4 ? s.held : wave(kind, s.phase, dt)) * depth;
    }
  },
};

const noise: ModuleImpl<{ seed: number }> = {
  def: { type: 'noise', label: 'Noise', inputs: [], outputs: [audio('out')], params: [param('level', 0, 1, 0.5, { step: 0.01 })] },
  init: () => ({ seed: 0x1234567 }),
  process(c, s) {
    for (let i = 0; i < c.n; i++) {
      s.seed = (Math.imul(s.seed, 1664525) + 1013904223) >>> 0;
      c.out.out[i] = ((s.seed / 0x80000000) - 1) * (c.p.level ?? 1);
    }
  },
};

const constant: ModuleImpl<null> = {
  def: { type: 'const', label: 'Constant', inputs: [], outputs: [control('out')], params: [param('value', -4, 4, 0, { step: 0.01 })] },
  init: () => null,
  process(c) {
    c.out.out.fill(c.p.value ?? 0, 0, c.n);
  },
};

const scale: ModuleImpl<null> = {
  def: {
    type: 'scale',
    label: 'Scale',
    inputs: [control('in')],
    outputs: [control('out')],
    params: [param('gain', -4, 4, 1, { step: 0.01 }), param('offset', -4, 4, 0, { step: 0.01 })],
  },
  init: () => null,
  process(c) {
    for (let i = 0; i < c.n; i++) c.out.out[i] = c.inp.in[i] * (c.p.gain ?? 1) + (c.p.offset ?? 0);
  },
};

interface AdsrState {
  stage: 0 | 1 | 2 | 3 | 4; // idle, attack, decay, sustain, release
  v: number;
  prev: number;
}

const adsr: ModuleImpl<AdsrState> = {
  def: {
    type: 'adsr',
    label: 'ADSR',
    inputs: [gate('gate')],
    outputs: [control('env')],
    params: [
      param('a', 0.001, 5, 0.01, { unit: 's', step: 0.001 }),
      param('d', 0.001, 5, 0.2, { unit: 's', step: 0.001 }),
      param('s', 0, 1, 0.6, { step: 0.01 }),
      param('r', 0.001, 8, 0.3, { unit: 's', step: 0.001 }),
    ],
  },
  init: () => ({ stage: 0, v: 0, prev: 0 }),
  process(c, s) {
    const rate = (t: number) => 1 - Math.exp(-4.6 / Math.max(t * c.sr, 1)); // reaches ~99% in t seconds
    const ad = rate(c.p.d ?? 0.2);
    const ar = rate(c.p.r ?? 0.3);
    const aInc = 1 / Math.max((c.p.a ?? 0.01) * c.sr, 1);
    const sus = c.p.s ?? 0.6;
    for (let i = 0; i < c.n; i++) {
      const g = c.inp.gate[i] > 0.5 ? 1 : 0;
      if (g && !s.prev) s.stage = 1;
      else if (!g && s.prev) s.stage = 4;
      s.prev = g;
      switch (s.stage) {
        case 1:
          s.v += aInc;
          if (s.v >= 1) {
            s.v = 1;
            s.stage = 2;
          }
          break;
        case 2:
          s.v += (sus - s.v) * ad;
          if (Math.abs(s.v - sus) < 1e-4) s.stage = 3;
          break;
        case 3:
          s.v = sus;
          break;
        case 4:
          s.v -= s.v * ar;
          if (s.v < 1e-4) {
            s.v = 0;
            s.stage = 0;
          }
          break;
      }
      c.out.env[i] = s.v;
    }
  },
};

const vca: ModuleImpl<null> = {
  def: {
    type: 'vca',
    label: 'VCA',
    inputs: [audio('in'), control('cv')],
    outputs: [audio('out')],
    params: [param('gain', 0, 2, 1, { step: 0.01 })],
  },
  init: () => null,
  process(c) {
    const open = !c.connected.cv;
    for (let i = 0; i < c.n; i++) c.out.out[i] = c.inp.in[i] * (open ? 1 : c.inp.cv[i]) * (c.p.gain ?? 1);
  },
};

const filter: ModuleImpl<{ ic1: number; ic2: number }> = {
  def: {
    type: 'filter',
    label: 'SVF filter',
    inputs: [audio('in'), control('cv')],
    outputs: [audio('lp'), audio('bp'), audio('hp')],
    params: [
      param('cutoff', 20, 18000, 1200, { unit: 'Hz', step: 1 }),
      param('res', 0, 1, 0.2, { step: 0.01 }),
      param('cvamt', 0, 4, 1, { unit: 'oct', step: 0.01 }),
    ],
  },
  init: () => ({ ic1: 0, ic2: 0 }),
  process(c, s) {
    const k = 2 - 1.98 * (c.p.res ?? 0);
    for (let i = 0; i < c.n; i++) {
      const fc = Math.min((c.p.cutoff ?? 1000) * Math.pow(2, c.inp.cv[i] * (c.p.cvamt ?? 1)), c.sr * 0.45);
      const g = Math.tan((Math.PI * fc) / c.sr);
      const a1 = 1 / (1 + g * (g + k));
      const a2 = g * a1;
      const a3 = g * a2;
      const v0 = c.inp.in[i];
      const v3 = v0 - s.ic2;
      const v1 = a1 * s.ic1 + a2 * v3;
      const v2 = s.ic2 + a2 * s.ic1 + a3 * v3;
      s.ic1 = 2 * v1 - s.ic1;
      s.ic2 = 2 * v2 - s.ic2;
      c.out.lp[i] = v2;
      c.out.bp[i] = v1;
      c.out.hp[i] = v0 - k * v1 - v2;
    }
  },
};

const mixer: ModuleImpl<null> = {
  def: {
    type: 'mixer',
    label: 'Mixer 4',
    inputs: [audio('in1'), audio('in2'), audio('in3'), audio('in4')],
    outputs: [audio('out')],
    params: [1, 2, 3, 4].map((n) => param(`l${n}`, 0, 1, 0.7, { step: 0.01 })),
  },
  init: () => null,
  process(c) {
    for (let i = 0; i < c.n; i++) {
      c.out.out[i] =
        c.inp.in1[i] * (c.p.l1 ?? 0) + c.inp.in2[i] * (c.p.l2 ?? 0) + c.inp.in3[i] * (c.p.l3 ?? 0) + c.inp.in4[i] * (c.p.l4 ?? 0);
    }
  },
};

const delay: ModuleImpl<{ buf: Float32Array; w: number }> = {
  def: {
    type: 'delay',
    label: 'Delay',
    inputs: [audio('in')],
    outputs: [audio('out')],
    params: [param('time', 1, 1000, 250, { unit: 'ms', step: 1 }), param('fb', 0, 0.95, 0.4, { step: 0.01 }), param('mix', 0, 1, 0.3, { step: 0.01 })],
  },
  init: (sr) => ({ buf: new Float32Array(Math.ceil(sr * 1.05)), w: 0 }),
  process(c, s) {
    const len = s.buf.length;
    const d = Math.max(1, Math.min(len - 1, Math.round(((c.p.time ?? 250) / 1000) * c.sr)));
    for (let i = 0; i < c.n; i++) {
      const r = s.buf[(s.w - d + len) % len];
      s.buf[s.w] = c.inp.in[i] + r * (c.p.fb ?? 0);
      s.w = (s.w + 1) % len;
      c.out.out[i] = c.inp.in[i] * (1 - (c.p.mix ?? 0)) + r * (c.p.mix ?? 0);
    }
  },
};

const output: ModuleImpl<null> = {
  def: {
    type: 'output',
    label: 'Output',
    inputs: [audio('l'), audio('r')],
    outputs: [],
    params: [param('level', 0, 1, 0.7, { step: 0.01 })],
  },
  init: () => null,
  process() {
    /* read by the engine */
  },
};

const ALL = [midi, fm1, osc, lfo, noise, constant, scale, adsr, vca, filter, mixer, delay, output];

/** Every parameter (except the FM-1 panel's own controls) gets a control input `<name>_cv`: the effective value is base + cv * (max - min), clamped. */
export const cvPort = (param: string): string => `${param}_cv`;
for (const m of ALL) for (const p of m.def.params) if (m.def.type !== 'fm1') m.def.inputs.push({ name: cvPort(p.name), kind: 'control', param: p.name });

export const IMPLS: Record<string, ModuleImpl> = Object.fromEntries(ALL.map((m) => [m.def.type, m as ModuleImpl]));

export const DEFS: Record<string, ModuleDef> = Object.fromEntries(Object.values(IMPLS).map((m) => [m.def.type, m.def]));
