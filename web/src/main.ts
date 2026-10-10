// SPDX-License-Identifier: GPL-3.0-only
import { AudioHost } from './audio';
import type { HostMsg } from './worklet';
import { DeviceLink } from './device';
import { Editor } from './editor';
import { buildBankTab } from './bank';
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

const device = new DeviceLink();
const live = (): boolean => device.connected && $<HTMLInputElement>('dev-live').checked;
let uploadTimer = 0;
let uploading = false;
let dirty = false;
const devStatus = (s: string): void => {
  $('dev-status').textContent = s;
};
device.onStatus = devStatus;
/** upload the current patch; changes made meanwhile cause one more upload */
async function sendPatch(): Promise<void> {
  if (uploading) {
    dirty = true;
    return;
  }
  uploading = true;
  try {
    do {
      dirty = false;
      devStatus('sending patch…');
      await device.upload(editor.patch);
      devStatus('patch on the FM-1');
    } while (dirty);
  } catch (e) {
    devStatus(`${(e as Error).message}`);
  } finally {
    uploading = false;
  }
}
const queueUpload = (): void => {
  clearTimeout(uploadTimer);
  uploadTimer = window.setTimeout(() => void sendPatch(), 150);
};
/** notes go to the browser engine and, with Live on, to the FM-1 */
const sink = {
  send(msg: HostMsg): void {
    host.send(msg);
    if (live()) device.note(msg as { t: string; note?: number; vel?: number });
  },
};

const editor = new Editor($('canvas'), loadSaved() ?? PRESETS['Subtractive saw'](), {
  onStructure(p) {
    host.setPatch(p);
    queueSave(p);
    if (live()) queueUpload();
  },
  onAtt(id, index, value) {
    host.send({ t: 'att', id, index, value });
    queueSave(editor.patch);
    if (live() && !uploading) device.att(editor.patch, id, index, value);
    else if (live()) dirty = true;
  },
  onParam(id, name, value) {
    host.send({ t: 'param', id, name, value });
    queueSave(editor.patch);
    if (live() && !uploading) device.param(editor.patch, id, name, value);
    else if (live()) dirty = true;
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
$('panic-btn').onclick = () => sink.send({ t: 'panic' });

// FM-1 link
$('dev-btn').onclick = async () => {
  try {
    const info = await device.connect();
    devStatus(`FM-1 connected (protocol ${info.version}, up to ${info.maxModules} modules, flash ${info.flashOk ? 'ok' : 'not available'})`);
    for (const id of ['dev-live', 'dev-send', 'dev-store']) $<HTMLInputElement>(id).disabled = false;
    $('dev-btn').textContent = 'Reconnect';
  } catch (e) {
    devStatus((e as Error).message);
  }
};
$('dev-send').onclick = () => void sendPatch();
$('dev-live').onchange = () => {
  if ($<HTMLInputElement>('dev-live').checked) void sendPatch();
};
$('dev-store').onclick = async () => {
  try {
    await sendPatch();
    await device.store();
    devStatus('stored: the FM-1 starts with this patch');
  } catch (e) {
    devStatus((e as Error).message);
  }
};

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
attachKeyboard(sink, () => currentTab === 'patch');
attachMidi(sink, (s) => {
  if (!host.running) status.textContent = s;
});

// tabs
buildBankTab($('tab-presets'), device, () => editor.patch, (msg) => {
  devStatus(msg);
  for (const id of ['dev-live', 'dev-send', 'dev-store']) $<HTMLInputElement>(id).disabled = false;
});
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

// open the tab named in the address (#presets, #flash, #about)
document.querySelector<HTMLButtonElement>(`#tabs button[data-tab="${location.hash.slice(1)}"]`)?.click();
