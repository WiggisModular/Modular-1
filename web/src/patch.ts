// SPDX-License-Identifier: GPL-3.0-only
// Patch data model, shared by the editor, the WebAudio engine and (later) the firmware encoder.

export type SignalKind = 'audio' | 'control' | 'gate';

export interface PortDef {
  name: string;
  kind: SignalKind;
  /** set on the two parameter inputs `p1` and `p2` (1 or 2) */
  pin?: 1 | 2;
}

/** What can change a module's parameters: its two parameter inputs and the four FM-1 knobs. */
export type Source = 'p1' | 'p2' | 'k1' | 'k2' | 'k3' | 'k4';
export const SOURCES: Source[] = ['p1', 'p2', 'k1', 'k2', 'k3', 'k4'];

export interface ParamDef {
  name: string;
  min: number;
  max: number;
  default: number;
  step?: number;
  unit?: string;
  /** names for integer-valued choice params (waveform etc.) */
  choices?: string[];
  /** logarithmic: the range is covered in equal ratios (a filter cutoff in octaves). Modulation and the knobs move the value in
   * that space: a fraction of the range is a fraction of the octaves. */
  log?: boolean;
  /** an on/off switch (0 or 1): a check box in the editor, OFF / ON on the LCD */
  toggle?: boolean;
}

/** position of a value in its range, 0..1 (logarithmic for log parameters) */
export function toUnit(pd: ParamDef, v: number): number {
  const x = Math.min(pd.max, Math.max(pd.min, v));
  return pd.log ? Math.log(x / pd.min) / Math.log(pd.max / pd.min) : (x - pd.min) / (pd.max - pd.min);
}

export function fromUnit(pd: ParamDef, u: number): number {
  const x = Math.min(1, Math.max(0, u));
  return pd.log ? pd.min * Math.pow(pd.max / pd.min, x) : pd.min + x * (pd.max - pd.min);
}

/** a parameter's base value moved by `mod` of its range (mean of a parameter input x attenuverter), clamped */
export function modulate(pd: ParamDef, base: number, mod: number): number {
  if (!pd.log) return Math.min(pd.max, Math.max(pd.min, base + mod * (pd.max - pd.min)));
  return fromUnit(pd, toUnit(pd, base) + mod);
}

export interface ModuleDef {
  type: string;
  label: string;
  inputs: PortDef[];
  outputs: PortDef[];
  params: ParamDef[];
  /** the module has the two parameter inputs p1, p2 (attenuverters) and the source matrix: every module but the FM-1 panel (the input) and the output */
  paramInputs?: boolean;
}

export interface Endpoint {
  module: number;
  port: string;
}

export interface Cable {
  from: Endpoint; // an output
  to: Endpoint; // an input
}

export interface ModuleInst {
  id: number;
  type: string;
  x: number;
  y: number;
  params: Record<string, number>;
  /** attenuverters of the parameter inputs p1 and p2, -1..1 (only on modules with paramInputs) */
  att?: [number, number];
  /** the matrix: the parameters each source controls. p1 / p2 add (input mean x attenuverter x range) to them, k1..k4 are
   * the FM-1 knobs, which nudge them while the module is selected on the device (only on modules with paramInputs) */
  map?: Record<Source, string[]>;
}

export interface Patch {
  version: 2;
  name: string;
  modules: ModuleInst[];
  cables: Cable[];
}

export const MAX_MODULES = 32;

export function emptyPatch(name = 'init'): Patch {
  return { version: 2, name, modules: [], cables: [] };
}

export function nextId(p: Patch): number {
  return p.modules.reduce((m, x) => Math.max(m, x.id), 0) + 1;
}

export function defaultParams(def: ModuleDef): Record<string, number> {
  const out: Record<string, number> = {};
  for (const p of def.params) out[p.name] = p.default;
  return out;
}

/** the default matrix: p1 and p2 control the first two parameters, knob n the n-th */
export function defaultMap(def: ModuleDef): Record<Source, string[]> {
  const at = (i: number): string[] => (def.params[i] ? [def.params[i].name] : []);
  return { p1: at(0), p2: at(1), k1: at(0), k2: at(1), k3: at(2), k4: at(3) };
}

/** att and map for a module that has parameter inputs, filled in or repaired */
export function fixMatrix(m: ModuleInst, def: ModuleDef): void {
  if (!def.paramInputs) {
    delete m.att;
    delete m.map;
    return;
  }
  const a = Array.isArray(m.att) ? m.att : [1, 1];
  m.att = [0, 1].map((i) => Math.max(-1, Math.min(1, Number.isFinite(Number(a[i])) ? Number(a[i]) : 1))) as [number, number];
  const names = new Set(def.params.map((q) => q.name));
  const dm = defaultMap(def);
  const map = {} as Record<Source, string[]>;
  for (const s of SOURCES) map[s] = Array.isArray(m.map?.[s]) ? m.map![s].filter((n) => names.has(n)) : dm[s];
  m.map = map;
}

export function addModule(p: Patch, def: ModuleDef, x: number, y: number): ModuleInst {
  if (p.modules.length >= MAX_MODULES) throw new Error(`a patch holds at most ${MAX_MODULES} modules`);
  const m: ModuleInst = { id: nextId(p), type: def.type, x, y, params: defaultParams(def) };
  fixMatrix(m, def);
  p.modules.push(m);
  return m;
}

export function removeModule(p: Patch, id: number): void {
  p.modules = p.modules.filter((m) => m.id !== id);
  p.cables = p.cables.filter((c) => c.from.module !== id && c.to.module !== id);
}

/** Connect an output to an input. Several cables into one input are summed. Returns false if rejected. */
export function connect(p: Patch, defs: Record<string, ModuleDef>, from: Endpoint, to: Endpoint): boolean {
  const mf = p.modules.find((m) => m.id === from.module);
  const mt = p.modules.find((m) => m.id === to.module);
  if (!mf || !mt || mf.id === mt.id) return false;
  if (!defs[mf.type]?.outputs.some((o) => o.name === from.port)) return false;
  if (!defs[mt.type]?.inputs.some((i) => i.name === to.port)) return false;
  if (p.cables.some((c) => sameEnd(c.from, from) && sameEnd(c.to, to))) return false;
  p.cables.push({ from: { ...from }, to: { ...to } });
  return true;
}

export function sameEnd(a: Endpoint, b: Endpoint): boolean {
  return a.module === b.module && a.port === b.port;
}

export function disconnect(p: Patch, cable: Cable): void {
  p.cables = p.cables.filter((c) => !(sameEnd(c.from, cable.from) && sameEnd(c.to, cable.to)));
}

/**
 * Processing order: every module after the modules that feed it. A cable that closes a loop is
 * a feedback cable and reads the producer's previous block (one block of delay).
 */
export function processingOrder(p: Patch): { order: number[]; feedback: Set<Cable> } {
  const ids = p.modules.map((m) => m.id);
  const state = new Map<number, 0 | 1 | 2>(); // 0 new, 1 on stack, 2 done
  const order: number[] = [];
  const feedback = new Set<Cable>();
  const incoming = new Map<number, Cable[]>();
  for (const c of p.cables) {
    const l = incoming.get(c.to.module) ?? [];
    l.push(c);
    incoming.set(c.to.module, l);
  }
  const visit = (id: number): void => {
    state.set(id, 1);
    for (const c of incoming.get(id) ?? []) {
      const s = state.get(c.from.module) ?? 0;
      if (s === 1) feedback.add(c);
      else if (s === 0) visit(c.from.module);
    }
    state.set(id, 2);
    order.push(id);
  };
  for (const id of ids) if ((state.get(id) ?? 0) === 0) visit(id);
  return { order, feedback };
}

/** Structural check of a patch loaded from JSON. Version 1 patches (one `<param>_cv` input per parameter) are converted, and a `midi` module becomes the FM-1 panel. Throws with a readable message. */
export function validatePatch(raw: unknown, defs: Record<string, ModuleDef>): Patch {
  const p = raw as Omit<Patch, 'version'> & { version: number };
  if (!p || (p.version !== 1 && p.version !== 2) || !Array.isArray(p.modules) || !Array.isArray(p.cables)) throw new Error('not a Modular-1 patch');
  if (p.modules.length > MAX_MODULES) throw new Error(`more than ${MAX_MODULES} modules`);
  const seen = new Set<number>();
  for (const m of p.modules) {
    if (m.type === 'midi') m.type = 'fm1'; // the MIDI module is gone: the FM-1 panel has the same pitch / gate / vel outputs
    const def = defs[m.type];
    if (!def) throw new Error(`unknown module type "${m.type}"`);
    if (!Number.isInteger(m.id) || seen.has(m.id)) throw new Error(`bad or duplicate module id ${m.id}`);
    seen.add(m.id);
    m.params = { ...defaultParams(def), ...(m.params ?? {}) };
    m.x = Number(m.x) || 0;
    m.y = Number(m.y) || 0;
    if (p.version === 1) {
      delete m.att;
      delete m.map;
    }
    fixMatrix(m, def);
  }
  if (p.version === 1) {
    // a cable into `<param>_cv` becomes a cable into p1 (first such parameter of the module) or p2 (second)
    const used = new Map<number, string[]>();
    for (const c of p.cables) {
      const mt = p.modules.find((m) => m.id === c.to?.module);
      const pm = /^(.*)_cv$/.exec(c.to?.port ?? '');
      if (!mt || !pm || !defs[mt.type].params.some((q) => q.name === pm[1])) continue;
      const list = used.get(mt.id) ?? [];
      if (!list.includes(pm[1])) list.push(pm[1]);
      if (list.length > 2) throw new Error(`${mt.type} uses more than two parameter inputs: this old patch cannot be converted`);
      used.set(mt.id, list);
      const pin = list.indexOf(pm[1]) === 0 ? 'p1' : 'p2';
      mt.map![pin] = [pm[1]];
      c.to.port = pin;
    }
  }
  (p as { version: number }).version = 2;
  for (const c of p.cables) {
    const mf = p.modules.find((m) => m.id === c.from?.module);
    if (mf?.type === 'filter' && c.from?.port === 'lp') c.from.port = 'lp12'; // the filter's 12 dB output used to be called lp
    const mt = p.modules.find((m) => m.id === c.to?.module);
    if (!mf || !mt) throw new Error('cable to a missing module');
    if (!defs[mf.type].outputs.some((o) => o.name === c.from.port)) throw new Error(`no output "${c.from.port}" on ${mf.type}`);
    if (!defs[mt.type].inputs.some((i) => i.name === c.to.port)) throw new Error(`no input "${c.to.port}" on ${mt.type}`);
  }
  p.name = String(p.name ?? 'patch');
  return p as Patch;
}
