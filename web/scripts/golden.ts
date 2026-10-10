// SPDX-License-Identifier: GPL-3.0-only
// Golden vectors for the firmware engine: for each test patch, the binary patch (.m1p), the note events (.ev) and the
// reference output of web/src/engine.ts (.f32, float32 stereo interleaved) at 44.1 kHz, written to build/golden/.
// Run from web/:  npx vite-node scripts/golden.ts   (tools/golden.sh does this and runs the C comparison)
import { mkdirSync, writeFileSync } from 'node:fs';
import { Engine } from '../src/engine';
import { encodePatch, toQ16 } from '../src/patchbin';
import { DEFS } from '../src/modules';
import { attFrame, chunkFrames, commitFrame, paramFrame, slotEraseFrame, slotFrame, slotInfoFrame } from '../src/protocol';
import { make, PRESETS, type Build } from '../src/presets';
import type { Patch } from '../src/patch';

const SR = 44100;
const OUT = new URL('../../build/golden/', import.meta.url);
mkdirSync(OUT, { recursive: true });

const EVENTS: [number, 'on' | 'off', number][] = [
  [0, 'on', 60],
  [128 * 40, 'on', 67],
  [128 * 80, 'off', 67],
  [128 * 120, 'off', 60],
];
const END = 128 * 180;

const tests: Record<string, Patch> = {};
for (const [n, p] of Object.entries(PRESETS)) tests[n] = p();

const kit = (name: string, b: Build): void => {
  tests[name] = make(name, b);
};
kit('waves', (add, wire) => {
  const kb = add('fm1', 0, 0);
  const mix = add('mixer', 0, 0, { l1: 0.25, l2: 0.25, l3: 0.25, l4: 0.25 });
  const out = add('output', 0, 0);
  [0, 1, 2, 3].forEach((w, i) => {
    const o = add('osc', 0, 0, { wave: w, pw: 0.3, coarse: i * 3 - 12, fine: i * 7, level: 0.9 });
    wire(kb, 'pitch', o, 'pitch');
    wire(o, 'out', mix, `in${i + 1}`);
  });
  wire(mix, 'out', out, 'l');
});
kit('lfo-sh', (add, wire) => {
  const kb = add('fm1', 0, 0);
  const sh = add('lfo', 0, 0, { wave: 4, rate: 9, depth: 0.8 });
  const tri = add('lfo', 0, 0, { wave: 1, rate: 2.5, depth: 0.6 });
  const osc = add('osc', 0, 0, { wave: 3, pw: 0.5, level: 0.8 }, { map: { p1: ['level'], p2: ['pw'] } });
  const sc = add('scale', 0, 0, { gain: 0.5, offset: 0.1 });
  const out = add('output', 0, 0);
  wire(kb, 'pitch', osc, 'pitch');
  wire(sh, 'out', osc, 'p2');
  wire(tri, 'out', sc, 'in');
  wire(sc, 'out', osc, 'p1');
  wire(osc, 'out', out, 'l');
});
kit('noise-svf', (add, wire) => {
  const nz = add('noise', 0, 0, { level: 0.6 });
  const flt = add('filter', 0, 0, { cutoff: 900, res: 0.7 });
  const mix = add('mixer', 0, 0, { l1: 0.5, l2: 0.5, l3: 0.5 });
  const out = add('output', 0, 0, { level: 0.8 });
  wire(nz, 'out', flt, 'in');
  wire(flt, 'lp', mix, 'in1');
  wire(flt, 'bp', mix, 'in2');
  wire(flt, 'hp', mix, 'in3');
  wire(mix, 'out', out, 'l');
});
kit('fm1-panel', (add, wire) => {
  const pn = add('fm1', 0, 0, { k1: 0.2, k3: 0.4 });
  const osc = add('osc', 0, 0, { wave: 2 }, { map: { p1: ['level'], p2: ['fine'] } });
  const out = add('output', 0, 0);
  wire(pn, 'pitch', osc, 'pitch');
  wire(pn, 'k3', osc, 'p1');
  wire(pn, 'k1', osc, 'p2');
  wire(osc, 'out', out, 'l');
});
kit('delay-fb', (add, wire) => {
  const kb = add('fm1', 0, 0);
  const osc = add('osc', 0, 0, { wave: 1, level: 0.5 });
  const env = add('adsr', 0, 0, { a: 0.002, d: 0.05, s: 0, r: 0.05 });
  const vca = add('vca', 0, 0);
  const dly = add('delay', 0, 0, { time: 100, fb: 0.6, mix: 0.5 });
  const out = add('output', 0, 0);
  wire(kb, 'pitch', osc, 'pitch');
  wire(kb, 'gate', env, 'gate');
  wire(osc, 'out', vca, 'in');
  wire(env, 'env', vca, 'cv');
  wire(vca, 'out', dly, 'in');
  wire(dly, 'out', out, 'l');
  wire(dly, 'out', out, 'r');
});
kit('feedback-pm', (add, wire) => {
  const kb = add('fm1', 0, 0);
  const osc = add('osc', 0, 0, { wave: 0, pm: 2 });
  const vca = add('vca', 0, 0, { gain: 0.7 });
  const env = add('adsr', 0, 0, { a: 0.01, d: 0.2, s: 0.6, r: 0.2 });
  const out = add('output', 0, 0);
  wire(kb, 'pitch', osc, 'pitch');
  wire(kb, 'gate', env, 'gate');
  wire(osc, 'out', vca, 'in');
  wire(env, 'env', vca, 'cv');
  wire(vca, 'out', osc, 'pm');
  wire(vca, 'out', out, 'l');
});
kit('summed-inputs', (add, wire) => {
  const kb = add('fm1', 0, 0);
  const a = add('osc', 0, 0, { wave: 2, coarse: -12, level: 0.4 });
  const b = add('osc', 0, 0, { wave: 3, coarse: 7, level: 0.4 });
  const k = add('const', 0, 0, { value: 0.25 });
  const vca = add('vca', 0, 0);
  const out = add('output', 0, 0);
  wire(kb, 'pitch', a, 'pitch');
  wire(kb, 'pitch', b, 'pitch');
  wire(a, 'out', vca, 'in');
  wire(b, 'out', vca, 'in');
  wire(k, 'out', vca, 'cv');
  wire(vca, 'out', out, 'l');
});

kit('matrix-att', (add, wire) => {
  const kb = add('fm1', 0, 0);
  const lfo = add('lfo', 0, 0, { wave: 0, rate: 3, depth: 1 });
  const k = add('const', 0, 0, { value: 0.5 });
  const osc = add('osc', 0, 0, { wave: 0, level: 0.6 }, { map: { p1: ['level'], p2: ['level'] }, att: [-0.5, 0.3] });
  const flt = add('filter', 0, 0, { cutoff: 800, res: 0.3 }, { map: { p1: ['cutoff', 'res'], p2: ['cvamt'] }, att: [0.3, -1] });
  const out = add('output', 0, 0);
  wire(kb, 'pitch', osc, 'pitch');
  wire(lfo, 'out', osc, 'p1');
  wire(k, 'out', osc, 'p2');
  wire(lfo, 'out', flt, 'p1');
  wire(k, 'out', flt, 'p2');
  wire(osc, 'out', flt, 'in');
  wire(flt, 'lp', out, 'l');
});

kit('reverb', (add, wire) => {
  const kb = add('fm1', 0, 0);
  const osc = add('osc', 0, 0, { wave: 0, level: 0.8 });
  const env = add('adsr', 0, 0, { a: 0.002, d: 0.05, s: 0, r: 0.05 });
  const vca = add('vca', 0, 0);
  const rv = add('reverb', 0, 0, { size: 0.8, damp: 0.3, mix: 0.6 });
  const out = add('output', 0, 0);
  wire(kb, 'pitch', osc, 'pitch');
  wire(kb, 'gate', env, 'gate');
  wire(osc, 'out', vca, 'in');
  wire(env, 'env', vca, 'cv');
  wire(vca, 'out', rv, 'in');
  wire(rv, 'out', out, 'l');
});

const slug = (s: string): string => s.toLowerCase().replace(/[^a-z0-9]+/g, '-');
for (const [name, patch] of Object.entries(tests)) {
  const e = new Engine(SR);
  e.load(patch);
  const l = new Float32Array(END);
  const r = new Float32Array(END);
  let pos = 0;
  const seg = (to: number): void => {
    if (to > pos) {
      const a = new Float32Array(to - pos);
      const b = new Float32Array(to - pos);
      e.render(a, b);
      l.set(a, pos);
      r.set(b, pos);
      pos = to;
    }
  };
  for (const [fr, kind, note] of EVENTS) {
    seg(fr);
    if (kind === 'on') e.noteOn(note, 1);
    else e.noteOff(note);
  }
  seg(END);
  const st = new Float32Array(END * 2);
  for (let i = 0; i < END; i++) {
    st[2 * i] = l[i];
    st[2 * i + 1] = r[i];
  }
  const f = slug(name);
  writeFileSync(new URL(`${f}.m1p`, OUT), encodePatch(patch));
  writeFileSync(new URL(`${f}.f32`, OUT), new Uint8Array(st.buffer));
  writeFileSync(new URL(`${f}.ev`, OUT), EVENTS.map(([fr, k, n]) => (k === 'on' ? `${fr} on ${n} 127` : `${fr} off ${n}`)).join('\n') + `\n${END} end\n`);
}
// protocol test input: upload of 303 Acid, then a cutoff change on its filter
{
  const p = PRESETS['303 Acid']();
  const bin = encodePatch(p);
  const mod = p.modules.findIndex((m) => m.type === 'filter');
  const par = DEFS.filter.params.findIndex((x) => x.name === 'cutoff');
  const val = toQ16(900);
  const attVal = toQ16(-0.5);
  const frames = [...chunkFrames(bin, 50), commitFrame(bin), paramFrame(mod, par, val), attFrame(mod, 0, attVal)];
  writeFileSync(new URL('proto.syx', OUT), Buffer.concat(frames));
  // preset bank: the patch into slot 5 as '303 ACID', an erase of slot 7 and an info request for slot 5, then a slot
  // command with a wrong CRC (must be refused)
  const bad = Uint8Array.from(bin);
  const slotFrames = [...chunkFrames(bin, 90), slotFrame(5, '303 ACID', bin), slotInfoFrame(5), slotEraseFrame(7), ...chunkFrames(bad, 90), slotFrame(6, 'BAD', bad, 0x1234)];
  writeFileSync(new URL('proto-slot.syx', OUT), Buffer.concat(slotFrames));
  writeFileSync(new URL('proto.args', OUT), `${mod} ${par} ${val} ${mod} 0 ${attVal}\n`);
}
console.log(`golden vectors for ${Object.keys(tests).length} patches in build/golden/`);
