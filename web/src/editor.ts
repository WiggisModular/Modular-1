// SPDX-License-Identifier: GPL-3.0-only
// Patch editor: module cards on a canvas, SVG cables, drag-to-connect.
import { DEFS } from './modules';
import {
  addModule,
  fromUnit,
  morphText,
  toUnit,
  SOURCES,
  type Source,
  connect,
  disconnect,
  removeModule,
  sameEnd,
  type Cable,
  type Endpoint,
  type ModuleDef,
  type ModuleInst,
  type Patch,
  type PortDef,
} from './patch';

const SVGNS = 'http://www.w3.org/2000/svg';

export interface EditorEvents {
  /** structure changed (modules or cables): the engine needs the whole patch */
  onStructure(p: Patch): void;
  /** a knob moved: cheap update */
  onParam(id: number, name: string, value: number): void;
  /** an attenuverter of a parameter input moved (index 0 = p1, 1 = p2) */
  onAtt(id: number, index: number, value: number): void;
  onError(msg: string): void;
}

export class Editor {
  patch: Patch;
  private modulesEl: HTMLElement;
  private svg: SVGSVGElement;
  private ports = new Map<string, HTMLElement>();
  private drag: { from: Endpoint; kind: 'out' | 'in'; path: SVGPathElement } | null = null;

  constructor(private canvas: HTMLElement, patch: Patch, private ev: EditorEvents) {
    this.patch = patch;
    this.modulesEl = canvas.querySelector('#modules')!;
    this.svg = canvas.querySelector('#cables')! as unknown as SVGSVGElement;
    window.addEventListener('resize', () => this.drawCables());
    this.rebuild();
  }

  setPatch(p: Patch): void {
    this.patch = p;
    this.rebuild();
    this.ev.onStructure(p);
  }

  add(type: string): void {
    try {
      const n = this.patch.modules.length;
      addModule(this.patch, DEFS[type], 20 + (n % 4) * 270, 20 + Math.floor(n / 4) * 560);
    } catch (e) {
      return this.ev.onError(String((e as Error).message ?? e));
    }
    this.rebuild();
    this.ev.onStructure(this.patch);
  }

  private key(id: number, port: string, dir: 'out' | 'in'): string {
    return `${id}:${dir}:${port}`;
  }

  rebuild(): void {
    this.modulesEl.replaceChildren();
    this.ports.clear();
    for (const m of this.patch.modules) this.modulesEl.append(this.card(m, DEFS[m.type]));
    this.sizeCanvas();
    requestAnimationFrame(() => this.drawCables());
  }

  private sizeCanvas(): void {
    const w = Math.max(900, ...this.patch.modules.map((m) => m.x + 270));
    const h = Math.max(520, ...this.patch.modules.map((m) => m.y + 560));
    this.canvas.style.minWidth = `${w}px`;
    this.canvas.style.minHeight = `${h}px`;
  }

  private card(m: ModuleInst, def: ModuleDef): HTMLElement {
    const el = document.createElement('div');
    el.className = 'module';
    el.style.left = `${m.x}px`;
    el.style.top = `${m.y}px`;
    el.dataset.id = String(m.id);

    const head = document.createElement('div');
    head.className = 'mhead';
    const title = document.createElement('span');
    title.textContent = def.label;
    const del = document.createElement('button');
    del.className = 'x';
    del.textContent = '×';
    del.title = 'Remove module';
    del.setAttribute('aria-label', `Remove ${def.label}`);
    del.onclick = () => {
      removeModule(this.patch, m.id);
      this.rebuild();
      this.ev.onStructure(this.patch);
    };
    head.append(title, del);
    this.makeDraggable(el, head, m);

    const body = document.createElement('div');
    body.className = 'mbody';
    const ins = document.createElement('div');
    ins.className = 'ports in';
    for (const p of def.inputs) if (!p.pin) ins.append(this.port(m, p, 'in'));
    const outs = document.createElement('div');
    outs.className = 'ports out';
    for (const p of def.outputs) outs.append(this.port(m, p, 'out'));
    const io = document.createElement('div');
    io.className = 'io';
    io.append(ins, outs);

    const params = document.createElement('div');
    params.className = 'params';
    const refresh: (() => void)[] = [];
    for (const pd of def.params) {
      const row = document.createElement('label');
      row.className = 'param';
      const name = document.createElement('span');
      name.textContent = pd.name;
      const val = document.createElement('output');
      const fmt = (v: number): string =>
        pd.altText && m.params[pd.altText.when] >= 0.5 ? pd.altText.text(v) : (pd.choices && pd.morph ? morphText(pd.choices, v) : pd.choices ? pd.choices[Math.round(v)] : `${Number(v.toFixed(3))}${pd.unit ? ' ' + pd.unit : ''}`);
      val.textContent = fmt(m.params[pd.name]);
      refresh.push(() => (val.textContent = fmt(m.params[pd.name])));
      const input = document.createElement('input');
      input.type = 'range';
      if (pd.log) {
        // a logarithmic parameter: the slider position is the fraction of the octaves
        input.min = '0';
        input.max = '1000';
        input.step = '1';
        input.value = String(Math.round(toUnit(pd, m.params[pd.name]) * 1000));
      } else {
        input.min = String(pd.min);
        input.max = String(pd.max);
        input.step = String(pd.step ?? (pd.max - pd.min) / 200);
        input.value = String(m.params[pd.name]);
      }
      input.setAttribute('aria-label', `${def.label} ${pd.name}`);
      input.oninput = () => {
        const v = pd.log ? Math.round(fromUnit(pd, Number(input.value) / 1000)) : Number(input.value);
        m.params[pd.name] = v;
        val.textContent = fmt(v);
        this.ev.onParam(m.id, pd.name, v);
      };
      if (pd.toggle) {
        input.type = 'checkbox';
        input.checked = m.params[pd.name] >= 0.5;
        input.oninput = () => {
          const v = input.checked ? 1 : 0;
          m.params[pd.name] = v;
          val.textContent = fmt(v);
          refresh.forEach((f) => f());
          this.ev.onParam(m.id, pd.name, v);
        };
      }
      row.append(name, val, input);
      params.append(row);
    }
    body.append(io);
    if (def.paramInputs) body.append(this.pins(m, def), this.matrix(m, def));
    body.append(params);
    el.append(head, body);
    return el;
  }

  /** the two parameter inputs, each with an attenuverter (-1..1: scale and invert) */
  private pins(m: ModuleInst, def: ModuleDef): HTMLElement {
    const box = document.createElement('div');
    box.className = 'pins';
    def.inputs
      .filter((p) => p.pin)
      .forEach((p, i) => {
        const row = document.createElement('div');
        row.className = 'pin';
        const jack = this.port(m, p, 'in');
        const val = document.createElement('output');
        const att = m.att ?? [1, 1];
        val.textContent = att[i].toFixed(2);
        const input = document.createElement('input');
        input.type = 'range';
        input.min = '-1';
        input.max = '1';
        input.step = '0.01';
        input.value = String(att[i]);
        input.title = `${p.name} attenuverter: -1 inverts, 0 mutes`;
        input.setAttribute('aria-label', `${def.label} ${p.name} attenuverter`);
        input.oninput = () => {
          const v = Number(input.value);
          m.att![i] = v;
          val.textContent = v.toFixed(2);
          this.ev.onAtt(m.id, i, v);
        };
        row.append(jack, input, val);
        box.append(row);
      });
    return box;
  }

  /** which parameter each source controls: columns p1 p2 and the knobs k1..k4, one row per parameter */
  private matrix(m: ModuleInst, def: ModuleDef): HTMLElement {
    const t = document.createElement('table');
    t.className = 'matrix';
    t.title = 'p1 / p2: the parameter inputs. k1..k4: the FM-1 knobs, which change these parameters while the module is selected on the device.';
    const head = t.createTHead().insertRow();
    head.insertCell();
    for (const s of SOURCES) {
      const th = document.createElement('th');
      th.textContent = s;
      head.append(th);
    }
    const body = t.createTBody();
    for (const pd of def.params) {
      const row = body.insertRow();
      const name = row.insertCell();
      name.textContent = pd.name;
      for (const s of SOURCES as Source[]) {
        const box = document.createElement('input');
        box.type = 'checkbox';
        box.checked = m.map![s].includes(pd.name);
        box.setAttribute('aria-label', `${def.label}: ${s} controls ${pd.name}`);
        box.onchange = () => {
          const list = m.map![s].filter((n) => n !== pd.name);
          if (box.checked) list.push(pd.name);
          m.map![s] = list;
          this.ev.onStructure(this.patch);
        };
        row.insertCell().append(box);
      }
    }
    return t;
  }

  private port(m: ModuleInst, p: PortDef, dir: 'out' | 'in'): HTMLElement {
    const row = document.createElement('div');
    row.className = `port ${dir}`;
    const dot = document.createElement('span');
    dot.className = `dot ${p.kind}`;
    dot.dataset.mod = String(m.id);
    dot.dataset.port = p.name;
    dot.dataset.dir = dir;
    dot.title = `${p.label ?? p.name} (${p.kind})`;
    const label = document.createElement('span');
    label.textContent = p.label ?? p.name;
    row.append(...(dir === 'in' ? [dot, label] : [label, dot]));
    dot.addEventListener('pointerdown', (e) => this.beginDrag(e, { module: m.id, port: p.name }, dir));
    this.ports.set(this.key(m.id, p.name, dir), dot);
    return row;
  }

  private makeDraggable(el: HTMLElement, handle: HTMLElement, m: ModuleInst): void {
    handle.addEventListener('pointerdown', (e) => {
      if ((e.target as HTMLElement).closest('button')) return;
      const sx = e.clientX - m.x;
      const sy = e.clientY - m.y;
      handle.setPointerCapture(e.pointerId);
      const move = (ev: PointerEvent): void => {
        m.x = Math.max(0, ev.clientX - sx);
        m.y = Math.max(0, ev.clientY - sy);
        el.style.left = `${m.x}px`;
        el.style.top = `${m.y}px`;
        this.drawCables();
      };
      const up = (): void => {
        handle.removeEventListener('pointermove', move);
        handle.removeEventListener('pointerup', up);
        this.sizeCanvas();
      };
      handle.addEventListener('pointermove', move);
      handle.addEventListener('pointerup', up);
    });
  }

  private center(el: HTMLElement): { x: number; y: number } {
    const r = el.getBoundingClientRect();
    const c = this.canvas.getBoundingClientRect();
    return { x: r.left - c.left + r.width / 2, y: r.top - c.top + r.height / 2 };
  }

  private curve(a: { x: number; y: number }, b: { x: number; y: number }): string {
    const dx = Math.max(40, Math.abs(b.x - a.x) / 2);
    return `M${a.x},${a.y} C${a.x + dx},${a.y} ${b.x - dx},${b.y} ${b.x},${b.y}`;
  }

  drawCables(): void {
    this.svg.replaceChildren();
    for (const c of this.patch.cables) {
      const a = this.ports.get(this.key(c.from.module, c.from.port, 'out'));
      const b = this.ports.get(this.key(c.to.module, c.to.port, 'in'));
      if (!a || !b) continue;
      const path = document.createElementNS(SVGNS, 'path');
      path.setAttribute('d', this.curve(this.center(a), this.center(b)));
      const kind = DEFS[this.patch.modules.find((m) => m.id === c.from.module)!.type].outputs.find((o) => o.name === c.from.port)!.kind;
      path.setAttribute('class', `cable ${kind}`);
      path.addEventListener('click', () => this.removeCable(c));
      const title = document.createElementNS(SVGNS, 'title');
      title.textContent = 'Click to remove';
      path.append(title);
      this.svg.append(path);
    }
  }

  private removeCable(c: Cable): void {
    disconnect(this.patch, c);
    this.drawCables();
    this.ev.onStructure(this.patch);
  }

  private beginDrag(e: PointerEvent, from: Endpoint, kind: 'out' | 'in'): void {
    e.preventDefault();
    e.stopPropagation();
    const start = this.ports.get(this.key(from.module, from.port, kind))!;
    const path = document.createElementNS(SVGNS, 'path');
    path.setAttribute('class', 'cable pending');
    this.svg.append(path);
    this.drag = { from, kind, path };
    const move = (ev: PointerEvent): void => {
      const c = this.canvas.getBoundingClientRect();
      const p = { x: ev.clientX - c.left, y: ev.clientY - c.top };
      const s = this.center(start);
      path.setAttribute('d', kind === 'out' ? this.curve(s, p) : this.curve(p, s));
    };
    const up = (ev: PointerEvent): void => {
      window.removeEventListener('pointermove', move);
      window.removeEventListener('pointerup', up);
      path.remove();
      this.drag = null;
      const t = document.elementFromPoint(ev.clientX, ev.clientY) as HTMLElement | null;
      const dot = t?.closest?.('.dot') as HTMLElement | null;
      if (!dot || !dot.dataset.mod) return;
      const other: Endpoint = { module: Number(dot.dataset.mod), port: dot.dataset.port! };
      const otherDir = dot.dataset.dir as 'out' | 'in';
      if (otherDir === kind) return this.ev.onError('connect an output to an input');
      const [o, i] = kind === 'out' ? [from, other] : [other, from];
      if (this.patch.cables.some((c) => sameEnd(c.from, o) && sameEnd(c.to, i))) return;
      if (!connect(this.patch, DEFS, o, i)) return this.ev.onError('cannot connect these ports');
      this.drawCables();
      this.ev.onStructure(this.patch);
    };
    window.addEventListener('pointermove', move);
    window.addEventListener('pointerup', up);
    move(e);
  }
}
