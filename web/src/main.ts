// SPDX-License-Identifier: GPL-3.0-only
import { AudioHost } from './audio';
import { Editor } from './editor';
import { buildFlashTab } from './flasher';
import { attachKeyboard, attachMidi } from './input';
import { DEFS } from './modules';
import { emptyPatch, validatePatch, type Patch } from './patch';
import { PRESETS } from './presets';

const $ = <T extends HTMLElement>(id: string): T => document.getElementById(id) as T;
const STORE = 'modular-1.patch';

function loadSaved(): Patch | null {
  try {
    const s = localStorage.getItem(STORE);
    return s ? validatePatch(JSON.parse(s), DEFS) : null;
  } catch {
    return null;
  }
}

function save(p: Patch): void {
  try {
    localStorage.setItem(STORE, JSON.stringify(p));
  } catch {
    /* private mode or storage blocked: the patch just isn't remembered */
  }
}

const host = new AudioHost();
const status = $('audio-status');
host.onError = (m) => (status.textContent = `engine: ${m}`);

let currentTab = 'patch';
let saveTimer = 0;
const queueSave = (p: Patch): void => {
  clearTimeout(saveTimer);
  saveTimer = window.setTimeout(() => save(p), 300);
};

const editor = new Editor($('canvas'), loadSaved() ?? PRESETS['Subtractive saw'](), {
  onStructure(p) {
    host.setPatch(p);
    queueSave(p);
  },
  onParam(id, name, value) {
    host.send({ t: 'param', id, name, value });
    queueSave(editor.patch);
  },
  onError(msg) {
    status.textContent = msg;
  },
});
host.setPatch(editor.patch);

// toolbar
const presetSel = $<HTMLSelectElement>('preset');
presetSel.append(new Option('Load preset…', ''), ...Object.keys(PRESETS).map((n) => new Option(n, n)), new Option('Empty patch', '__empty'));
presetSel.onchange = () => {
  const v = presetSel.value;
  if (v) editor.setPatch(v === '__empty' ? emptyPatch() : PRESETS[v]());
  presetSel.value = '';
  $<HTMLInputElement>('patch-name').value = editor.patch.name;
};
const nameInput = $<HTMLInputElement>('patch-name');
nameInput.value = editor.patch.name;
nameInput.oninput = () => {
  editor.patch.name = nameInput.value;
  queueSave(editor.patch);
};
const typeSel = $<HTMLSelectElement>('add-type');
typeSel.append(...Object.values(DEFS).map((d) => new Option(d.label, d.type)));
$('add-btn').onclick = () => editor.add(typeSel.value);
$('panic-btn').onclick = () => host.send({ t: 'panic' });

$('save-btn').onclick = () => {
  const blob = new Blob([JSON.stringify(editor.patch, null, 2)], { type: 'application/json' });
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = `${editor.patch.name.replace(/[^\w.-]+/g, '_') || 'patch'}.json`;
  a.click();
  URL.revokeObjectURL(a.href);
};
$<HTMLInputElement>('load-file').onchange = async (e) => {
  const input = e.target as HTMLInputElement;
  const f = input.files?.[0];
  input.value = '';
  if (!f) return;
  try {
    editor.setPatch(validatePatch(JSON.parse(await f.text()), DEFS));
    nameInput.value = editor.patch.name;
  } catch (err) {
    status.textContent = `load failed: ${(err as Error).message}`;
  }
};

// audio + input
$('audio-btn').onclick = async () => {
  try {
    await host.start();
    status.textContent = 'audio running';
    $('audio-btn').textContent = 'Audio on';
  } catch (e) {
    status.textContent = `audio failed: ${e}`;
  }
};
attachKeyboard(host, () => currentTab === 'patch');
attachMidi(host, (s) => {
  if (!host.running) status.textContent = s;
});

// tabs
buildFlashTab($('tab-flash'));
$('tab-about').innerHTML = `
  <div class="card"><h2>Modular-1</h2>
  <p>A patchable modular synth for the M-VAVE FM-1, with a browser editor. The module engine runs in your browser here and (in a later milestone) on the FM-1 itself.</p>
  <p class="muted">GPL-3.0-only. The flasher, loader and hardware layer are derived from <a href="https://github.com/hugelton/Felucca">Felucca</a> by Leo Kuroshita / Hügelton Instruments. Not affiliated with M-VAVE or Clavia.</p></div>`;
for (const b of document.querySelectorAll<HTMLButtonElement>('#tabs button')) {
  b.onclick = () => {
    currentTab = b.dataset.tab!;
    for (const x of document.querySelectorAll<HTMLButtonElement>('#tabs button')) x.setAttribute('aria-selected', String(x === b));
    for (const t of document.querySelectorAll<HTMLElement>('.tab')) t.hidden = t.id !== `tab-${currentTab}`;
    if (currentTab === 'patch') editor.drawCables();
  };
}
