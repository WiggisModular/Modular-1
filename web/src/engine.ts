// SPDX-License-Identifier: GPL-3.0-only
// Block-based patch engine. Pure TypeScript with no DOM or Web Audio dependency, so it runs in
// an AudioWorklet, in tests, and (as the reference) next to the firmware port.

import { IMPLS, type Ctx, type ModuleImpl, type Voice } from './modules';
import { defaultMap, processingOrder, SOURCES, type Cable, type Patch, type Source } from './patch';

export const BLOCK = 128;

interface Node {
  id: number;
  type: string;
  impl: ModuleImpl;
  state: unknown;
  params: Record<string, number>;
  /** params after modulation, what the module reads */
  eff: Record<string, number>;
  att: [number, number];
  map: Record<Source, string[]>;
  inp: Record<string, Float32Array>;
  out: Record<string, Float32Array>;
  connected: Record<string, boolean>;
  sources: { port: string; from: Node; fromPort: string }[];
}

export class Engine {
  private nodes = new Map<number, Node>();
  private order: Node[] = [];
  private outputs: Node[] = [];
  private held: { note: number; vel: number }[] = [];
  readonly voice: Voice = { pitch: 0, gate: 0, velocity: 0 };

  constructor(readonly sampleRate: number) {}

  /** Install a patch. Modules whose id and type are unchanged keep their running state. */
  load(patch: Patch): void {
    const next = new Map<number, Node>();
    for (const m of patch.modules) {
      const impl = IMPLS[m.type];
      if (!impl) throw new Error(`unknown module type "${m.type}"`);
      const old = this.nodes.get(m.id);
      const node: Node =
        old && old.type === m.type
          ? old
          : {
              id: m.id,
              type: m.type,
              impl,
              state: impl.init(this.sampleRate),
              params: {},
              eff: {},
              att: [1, 1],
              map: defaultMap(impl.def),
              inp: Object.fromEntries(impl.def.inputs.map((p) => [p.name, new Float32Array(BLOCK)])),
              out: Object.fromEntries(impl.def.outputs.map((p) => [p.name, new Float32Array(BLOCK)])),
              connected: {},
              sources: [],
            };
      node.params = { ...m.params };
      node.att = m.att ? [m.att[0], m.att[1]] : [1, 1];
      node.map = Object.fromEntries(SOURCES.map((s) => [s, [...(m.map?.[s] ?? [])]])) as Record<Source, string[]>;
      node.sources = [];
      node.connected = {};
      next.set(m.id, node);
    }
    for (const c of patch.cables) {
      const to = next.get(c.to.module)!;
      const from = next.get(c.from.module)!;
      to.sources.push({ port: c.to.port, from, fromPort: c.from.port });
      to.connected[c.to.port] = true;
    }
    this.nodes = next;
    const { order } = processingOrder(patch);
    this.order = order.map((id) => next.get(id)!);
    this.outputs = this.order.filter((n) => n.type === 'output');
  }

  setParam(id: number, name: string, value: number): void {
    const n = this.nodes.get(id);
    if (n) n.params[name] = value;
  }

  setAtt(id: number, index: number, value: number): void {
    const n = this.nodes.get(id);
    if (n && (index === 0 || index === 1)) n.att[index] = value;
  }

  /** Block-rate parameter modulation: the mean of p1 and p2 over the block, times their attenuverters, added to the parameters the matrix assigns them to. */
  private applyCv(node: Node, n: number): void {
    const pins: (number | null)[] = [null, null];
    if (node.impl.def.paramInputs)
      (['p1', 'p2'] as const).forEach((name, s) => {
        if (!node.connected[name]) return;
        let sum = 0;
        for (let i = 0; i < n; i++) sum += node.inp[name][i];
        pins[s] = (sum / n) * node.att[s];
      });
    for (const pd of node.impl.def.params) {
      let v = node.params[pd.name] ?? pd.default;
      let touched = false;
      (['p1', 'p2'] as const).forEach((name, s) => {
        if (pins[s] !== null && node.map[name].includes(pd.name)) {
          v += pins[s]! * (pd.max - pd.min);
          touched = true;
        }
      });
      node.eff[pd.name] = touched ? Math.min(pd.max, Math.max(pd.min, v)) : v;
    }
  }

  noteOn(note: number, velocity = 1): void {
    this.held = this.held.filter((h) => h.note !== note);
    this.held.push({ note, vel: velocity });
    this.updateVoice(true);
  }

  noteOff(note: number): void {
    this.held = this.held.filter((h) => h.note !== note);
    this.updateVoice(false);
  }

  allNotesOff(): void {
    this.held = [];
    this.voice.gate = 0;
  }

  private updateVoice(retrigger: boolean): void {
    const top = this.held[this.held.length - 1];
    if (top) {
      this.voice.pitch = (top.note - 60) / 12;
      this.voice.velocity = top.vel;
      this.voice.gate = 1;
      void retrigger;
    } else {
      this.voice.gate = 0;
    }
  }

  /** Render `left.length` frames (any length; processed in blocks of up to BLOCK). */
  render(left: Float32Array, right: Float32Array): void {
    const total = left.length;
    left.fill(0);
    right.fill(0);
    for (let off = 0; off < total; off += BLOCK) {
      const n = Math.min(BLOCK, total - off);
      for (const node of this.order) {
        for (const name in node.inp) node.inp[name].fill(0, 0, n);
        for (const s of node.sources) {
          const src = s.from.out[s.fromPort];
          const dst = node.inp[s.port];
          for (let i = 0; i < n; i++) dst[i] += src[i];
        }
        this.applyCv(node, n);
        const ctx: Ctx = { sr: this.sampleRate, n, inp: node.inp, out: node.out, connected: node.connected, p: node.eff, voice: this.voice };
        node.impl.process(ctx, node.state);
      }
      for (const o of this.outputs) {
        const lvl = o.eff.level ?? 1;
        const l = o.inp.l;
        const r = o.connected.r ? o.inp.r : o.inp.l;
        for (let i = 0; i < n; i++) {
          left[off + i] += l[i] * lvl;
          right[off + i] += r[i] * lvl;
        }
      }
    }
    for (let i = 0; i < total; i++) {
      left[i] = Math.max(-1, Math.min(1, left[i]));
      right[i] = Math.max(-1, Math.min(1, right[i]));
    }
  }
}

export type { Cable };
