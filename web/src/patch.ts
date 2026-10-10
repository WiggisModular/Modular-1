// SPDX-License-Identifier: GPL-3.0-only
// Patch data model, shared by the editor, the WebAudio engine and (later) the firmware encoder.

export type SignalKind = 'audio' | 'control' | 'gate';

export interface PortDef {
  name: string;
  kind: SignalKind;
}

export interface ParamDef {
  name: string;
  min: number;
  max: number;
  default: number;
  step?: number;
  unit?: string;
  /** names for integer-valued choice params (waveform etc.) */
  choices?: string[];
}

export interface ModuleDef {
  type: string;
  label: string;
  inputs: PortDef[];
  outputs: PortDef[];
  params: ParamDef[];
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
}

export interface Patch {
  version: 1;
  name: string;
  modules: ModuleInst[];
  cables: Cable[];
}

export const MAX_MODULES = 32;

export function emptyPatch(name = 'init'): Patch {
  return { version: 1, name, modules: [], cables: [] };
}

export function nextId(p: Patch): number {
  return p.modules.reduce((m, x) => Math.max(m, x.id), 0) + 1;
}

export function defaultParams(def: ModuleDef): Record<string, number> {
  const out: Record<string, number> = {};
  for (const p of def.params) out[p.name] = p.default;
  return out;
}

export function addModule(p: Patch, def: ModuleDef, x: number, y: number): ModuleInst {
  if (p.modules.length >= MAX_MODULES) throw new Error(`a patch holds at most ${MAX_MODULES} modules`);
  const m: ModuleInst = { id: nextId(p), type: def.type, x, y, params: defaultParams(def) };
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

/** Structural check of a patch loaded from JSON. Throws with a readable message. */
export function validatePatch(raw: unknown, defs: Record<string, ModuleDef>): Patch {
  const p = raw as Patch;
  if (!p || p.version !== 1 || !Array.isArray(p.modules) || !Array.isArray(p.cables)) throw new Error('not a Modular-1 patch');
  if (p.modules.length > MAX_MODULES) throw new Error(`more than ${MAX_MODULES} modules`);
  const seen = new Set<number>();
  for (const m of p.modules) {
    const def = defs[m.type];
    if (!def) throw new Error(`unknown module type "${m.type}"`);
    if (!Number.isInteger(m.id) || seen.has(m.id)) throw new Error(`bad or duplicate module id ${m.id}`);
    seen.add(m.id);
    m.params = { ...defaultParams(def), ...(m.params ?? {}) };
    m.x = Number(m.x) || 0;
    m.y = Number(m.y) || 0;
  }
  for (const c of p.cables) {
    const mf = p.modules.find((m) => m.id === c.from?.module);
    const mt = p.modules.find((m) => m.id === c.to?.module);
    if (!mf || !mt) throw new Error('cable to a missing module');
    if (!defs[mf.type].outputs.some((o) => o.name === c.from.port)) throw new Error(`no output "${c.from.port}" on ${mf.type}`);
    if (!defs[mt.type].inputs.some((i) => i.name === c.to.port)) throw new Error(`no input "${c.to.port}" on ${mt.type}`);
  }
  p.name = String(p.name ?? 'patch');
  return p;
}
