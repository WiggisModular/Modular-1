// SPDX-License-Identifier: GPL-3.0-only
// The Patches page: choose up to 32 patches and put them into the FM-1's patch bank (flash). The PRESET knob of the
// FM-1 steps through the bank; with an empty bank it steps through the factory patches built into the firmware.
import type { DeviceLink } from './device';
import { DEFS } from './modules';
import { validatePatch, type Patch } from './patch';
import { PRESETS } from './presets';
import { SLOTS } from './protocol';

interface Entry {
  name: string;
  /** a patch of the editor, kept as a copy; factory patches are rebuilt from PRESETS */
  patch?: Patch;
}

const STORE = 'modular-1.bank';

function load(): Entry[] {
  try {
    const raw = JSON.parse(localStorage.getItem(STORE) ?? 'null') as Entry[] | null;
    if (Array.isArray(raw)) {
      const ok = raw
        .filter((e) => typeof e?.name === 'string' && (e.patch ? true : e.name in PRESETS))
        .map((e) => (e.patch ? { name: e.name, patch: validatePatch(e.patch, DEFS) } : { name: e.name }))
        .slice(0, SLOTS);
      return ok;
    }
  } catch {
    /* fall through to the default */
  }
  return Object.keys(PRESETS).slice(0, SLOTS).map((name) => ({ name }));
}

function save(list: Entry[]): void {
  try {
    localStorage.setItem(STORE, JSON.stringify(list));
  } catch {
    /* storage blocked: the list is not remembered */
  }
}

const patchOf = (e: Entry): Patch => (e.patch ? e.patch : PRESETS[e.name]());

function el<K extends keyof HTMLElementTagNameMap>(tag: K, props: Partial<HTMLElementTagNameMap[K]> & { class?: string } = {}, ...kids: (Node | string)[]): HTMLElementTagNameMap[K] {
  const e = document.createElement(tag);
  const { class: cls, ...rest } = props as Record<string, unknown>;
  if (cls) e.className = String(cls);
  Object.assign(e, rest);
  e.append(...kids);
  return e;
}

export function buildBankTab(root: HTMLElement, device: DeviceLink, currentPatch: () => Patch, onConnected: (msg: string) => void): void {
  let list = load();
  const libEl = el('div', { class: 'bank-list' });
  const bankEl = el('div', { class: 'bank-list' });
  const countEl = el('span', { class: 'muted' });
  const status = el('p', { class: 'muted', textContent: 'Not connected.' });
  const bar = el('progress', { max: 100, value: 0 });
  const onDevice = el('pre', { class: 'log', textContent: '' });
  const connect = el('button', { textContent: 'Connect FM-1' });
  const upload = el('button', { class: 'primary', textContent: 'Upload bank to FM-1', disabled: true });
  const read = el('button', { textContent: 'Read FM-1 bank', disabled: true });
  const clear = el('button', { textContent: 'Empty the FM-1 bank', disabled: true });
  let busy = false;

  const refresh = (): void => {
    const full = list.length >= SLOTS;
    libEl.replaceChildren(
      ...Object.keys(PRESETS).map((name) => {
        const used = list.some((e) => !e.patch && e.name === name);
        return el('div', { class: 'bank-row' }, el('span', { textContent: name }), el('button', { textContent: '+', title: used ? 'already in the bank' : 'add to the bank', disabled: full, onclick: () => { list.push({ name }); changed(); } }));
      }),
    );
    bankEl.replaceChildren(
      ...list.map((e, i) =>
        el(
          'div',
          { class: 'bank-row' },
          el('span', { class: 'slot', textContent: String(i + 1).padStart(2, '0') }),
          el('span', { class: 'grow', textContent: e.patch ? `${e.name} (editor)` : e.name }),
          el('button', { textContent: '▲', title: 'move up', disabled: i === 0, onclick: () => { [list[i - 1], list[i]] = [list[i], list[i - 1]]; changed(); } }),
          el('button', { textContent: '▼', title: 'move down', disabled: i === list.length - 1, onclick: () => { [list[i + 1], list[i]] = [list[i], list[i + 1]]; changed(); } }),
          el('button', { textContent: '×', title: 'remove', onclick: () => { list.splice(i, 1); changed(); } }),
        ),
      ),
    );
    countEl.textContent = `${list.length} of ${SLOTS} slots`;
    const ready = device.connected && !busy;
    upload.disabled = !ready || list.length === 0;
    read.disabled = !ready;
    clear.disabled = !ready;
  };
  const changed = (): void => {
    save(list);
    refresh();
  };

  const run = async (what: string, fn: () => Promise<void>): Promise<void> => {
    if (busy) return;
    busy = true;
    refresh();
    try {
      await fn();
    } catch (e) {
      status.textContent = `${what} failed: ${(e as Error).message}`;
    } finally {
      busy = false;
      refresh();
    }
  };

  connect.onclick = () =>
    void run('connect', async () => {
      const info = await device.connect();
      status.textContent = `FM-1 connected (flash ${info.flashOk ? 'ok' : 'not available: the bank cannot be stored'}).`;
      onConnected(status.textContent);
    });

  upload.onclick = () =>
    void run('upload', async () => {
      const total = list.length;
      status.textContent = 'Reading the bank on the FM-1…';
      const have = await device.readBank();
      for (let i = 0; i < total; i++) {
        status.textContent = `Uploading ${i + 1} of ${total}: ${list[i].name}`;
        bar.value = (100 * i) / total;
        await device.uploadSlot(i, list[i].name.toUpperCase(), patchOf(list[i]));
      }
      for (let i = total; i < SLOTS; i++)
        if (have[i].used) {
          status.textContent = `Emptying slot ${i + 1}`;
          await device.eraseSlot(i);
        }
      bar.value = 100;
      status.textContent = `${total} patches are on the FM-1. Turn its PRESET knob to step through them.`;
    });

  read.onclick = () =>
    void run('read', async () => {
      status.textContent = 'Reading the bank on the FM-1…';
      const slots = await device.readBank();
      const used = slots.filter((s) => s.used);
      onDevice.textContent = used.length ? used.map((s) => `${String(s.slot + 1).padStart(2, '0')}  ${s.name}  (${s.length} bytes)`).join('\n') : 'The bank is empty: the PRESET knob steps through the factory patches built into the firmware.';
      status.textContent = `${used.length} of ${SLOTS} slots are used on the FM-1.`;
    });

  clear.onclick = () =>
    void run('clear', async () => {
      const slots = await device.readBank();
      for (const s of slots) if (s.used) await device.eraseSlot(s.slot);
      onDevice.textContent = 'The bank is empty: the PRESET knob steps through the factory patches built into the firmware.';
      status.textContent = 'The FM-1 bank is empty.';
    });

  const addCurrent = el('button', {
    textContent: 'Add the patch from the editor',
    onclick: () => {
      if (list.length >= SLOTS) return;
      const p = JSON.parse(JSON.stringify(currentPatch())) as Patch;
      list.push({ name: p.name || 'PATCH', patch: p });
      changed();
    },
  });
  const reset = el('button', { textContent: 'All 32 factory patches', title: 'Replace the list with the factory patches', onclick: () => { list = Object.keys(PRESETS).slice(0, SLOTS).map((name) => ({ name })); changed(); } });
  const empty = el('button', { textContent: 'Empty the list', onclick: () => { list = []; changed(); } });

  root.append(
    el('div', { class: 'card' }, el('h2', { textContent: 'Patch bank' }), el('p', { textContent: 'Choose up to 32 patches and send them to the FM-1. They are written to its flash: the PRESET knob steps through them, a program change selects one, and they stay when the power is off. Uploading replaces the whole bank.' }), el('div', { class: 'toolbar' }, connect, upload, read, clear), bar, status),
    el(
      'div',
      { class: 'bank-cols' },
      el('div', { class: 'card' }, el('h2', { textContent: 'Factory patches' }), el('div', { class: 'toolbar' }, addCurrent), libEl),
      el('div', { class: 'card' }, el('h2', { textContent: 'Bank to upload ' }, countEl), el('div', { class: 'toolbar' }, reset, empty), bankEl),
    ),
    el('div', { class: 'card' }, el('h2', { textContent: 'On the FM-1' }), onDevice),
  );
  refresh();
}
