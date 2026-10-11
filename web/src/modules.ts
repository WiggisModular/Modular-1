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

/**
 * The FM-1 front panel as a module, the patch's only input: the voice that the 27 note keys and MIDI play
 * (pitch/gate/velocity; MIDI is handled by the firmware / the browser host, not by a module) and encoders 1-4 as 0..1 values. In the browser the panel is the module's own sliders; on the
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

/** state variable filter, two stages: stage 1 (12 dB) gives lp12 / bp / hp, stage 2 takes lp12 and adds the other 12 dB (lp24) */
const filter: ModuleImpl<{ ic1: number; ic2: number; jc1: number; jc2: number }> = {
  def: {
    type: 'filter',
    label: 'Filter',
    inputs: [audio('in'), control('cv'), control('morph')],
    outputs: [audio('lp12'), audio('bp'), audio('hp'), audio('lp24'), audio('mix')],
    params: [
      param('cutoff', 20, 18000, 1200, { unit: 'Hz', step: 1, log: true }),
      param('res', 0, 1, 0.2, { step: 0.01 }),
      param('cvamt', 0, 4, 1, { unit: 'oct', step: 0.01 }),
      param('track', 0, 1, 0, { step: 1, choices: ['off', 'on'], toggle: true }),
      param('morph', 0, 1, 0, { step: 0.01 }),
    ],
  },
  init: () => ({ ic1: 0, ic2: 0, jc1: 0, jc2: 0 }),
  process(c, s) {
    const k = 2 - 1.98 * (c.p.res ?? 0);
    const k2 = Math.SQRT2; // stage 2 is damped (Q 0.7): the resonance peak comes from stage 1 alone
    const tr = (c.p.track ?? 0) >= 0.5 ? c.voice.pitch : 0; // keyboard tracking: the cutoff follows the played pitch 1 octave per octave (C4 = the cutoff as set)
    for (let i = 0; i < c.n; i++) {
      const fc = Math.min((c.p.cutoff ?? 1000) * Math.pow(2, c.inp.cv[i] * (c.p.cvamt ?? 1) + tr), c.sr * 0.45);
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
      const hp = v0 - k * v1 - v2;
      const b1 = 1 / (1 + g * (g + k2));
      const b2 = g * b1;
      const b3 = g * b2;
      const w3 = v2 - s.jc2;
      const w1 = b1 * s.jc1 + b2 * w3;
      const w2 = s.jc2 + b2 * s.jc1 + b3 * w3;
      s.jc1 = 2 * w1 - s.jc1;
      s.jc2 = 2 * w2 - s.jc2;
      const m = Math.min(1, Math.max(0, (c.p.morph ?? 0) + c.inp.morph[i]));
      c.out.lp12[i] = v2;
      c.out.bp[i] = v1;
      c.out.hp[i] = hp;
      c.out.lp24[i] = w2;
      c.out.mix[i] = w2 + (hp - w2) * m; // morph 0 = 24 dB low pass, 1 = high pass
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

/**
 * A small Freeverb-style reverb (mono): four lowpass-damped feedback combs in parallel into two series allpasses.
 * size sets the comb feedback (0.70 .. 0.98), damp the lowpass in the feedback, mix the wet share.
 */
const RV_COMB = [1116, 1188, 1277, 1356]; // delay lengths at 44.1 kHz
const RV_AP = [556, 441];
interface ReverbState {
  comb: Float32Array[];
  ci: number[];
  lp: number[];
  ap: Float32Array[];
  ai: number[];
}

const reverb: ModuleImpl<ReverbState> = {
  def: {
    type: 'reverb',
    label: 'Reverb',
    inputs: [audio('in')],
    outputs: [audio('out')],
    params: [param('size', 0, 1, 0.5, { step: 0.01 }), param('damp', 0, 1, 0.4, { step: 0.01 }), param('mix', 0, 1, 0.3, { step: 0.01 })],
  },
  init: (sr) => {
    const len = (n: number): number => Math.max(1, Math.round((n * sr) / 44100));
    return { comb: RV_COMB.map((n) => new Float32Array(len(n))), ci: [0, 0, 0, 0], lp: [0, 0, 0, 0], ap: RV_AP.map((n) => new Float32Array(len(n))), ai: [0, 0] };
  },
  process(c, s) {
    const fb = 0.7 + 0.28 * (c.p.size ?? 0.5);
    const d1 = (c.p.damp ?? 0.4) * 0.4;
    const mix = c.p.mix ?? 0.3;
    for (let i = 0; i < c.n; i++) {
      const x = c.inp.in[i];
      const g = x * 0.0625;
      let sum = 0;
      for (let k = 0; k < 4; k++) {
        const buf = s.comb[k];
        const o = buf[s.ci[k]];
        s.lp[k] = o * (1 - d1) + s.lp[k] * d1;
        buf[s.ci[k]] = g + s.lp[k] * fb;
        if (++s.ci[k] === buf.length) s.ci[k] = 0;
        sum += o;
      }
      for (let k = 0; k < 2; k++) {
        const buf = s.ap[k];
        const bo = buf[s.ai[k]];
        const y = bo - sum;
        buf[s.ai[k]] = sum + bo * 0.5;
        if (++s.ai[k] === buf.length) s.ai[k] = 0;
        sum = y;
      }
      c.out.out[i] = x * (1 - mix) + sum * mix;
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

const ALL = [fm1, osc, lfo, noise, constant, scale, adsr, vca, filter, mixer, delay, reverb, output];

/**
 * Every module except the FM-1 panel (the input) and the output has two parameter inputs, p1 and p2, each with an
 * attenuverter (-1..1), and a matrix that says which parameters p1, p2 and the knobs k1..k4 control. A parameter's
 * effective value is its base + (block mean of p1 x att1 + block mean of p2 x att2, for the inputs that control it) x
 * (max - min), clamped.
 */
export const PIN_EXEMPT = new Set(['fm1', 'output']);
export const PIN_NAMES = ['p1', 'p2'] as const;
for (const m of ALL)
  if (!PIN_EXEMPT.has(m.def.type) && m.def.params.length) {
    m.def.paramInputs = true;
    m.def.inputs.push({ name: 'p1', kind: 'control', pin: 1 }, { name: 'p2', kind: 'control', pin: 2 });
  }

export const IMPLS: Record<string, ModuleImpl> = Object.fromEntries(ALL.map((m) => [m.def.type, m as ModuleImpl]));

export const DEFS: Record<string, ModuleDef> = Object.fromEntries(Object.values(IMPLS).map((m) => [m.def.type, m.def]));
