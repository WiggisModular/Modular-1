// SPDX-License-Identifier: GPL-3.0-only
import { DEFS } from './modules';
import { addModule, connect, emptyPatch, validatePatch, type Patch, type Source } from './patch';

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

/** The factory presets: web/presets/NN-name.json (NN = slot in the bank, the order the firmware compiles them in). Each call returns a fresh copy. */
const files = import.meta.glob<unknown>('../presets/*.json', { eager: true, import: 'default' });

export const PRESETS: Record<string, () => Patch> = Object.fromEntries(
  Object.entries(files)
    .sort(([a], [b]) => (a < b ? -1 : 1))
    .map(([path, raw]): [string, () => Patch] => {
      const p = validatePatch(structuredClone(raw), DEFS);
      return [p.name, () => structuredClone(p)];
    }),
);
