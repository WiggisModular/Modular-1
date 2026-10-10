// SPDX-License-Identifier: GPL-3.0-only
// Flash tab. The USB-MIDI update protocol and package handling come from Felucca
// (src/flasher/fm1ota.js, fm1pkg.js; Copyright (C) 2026 Leo Kuroshita, GPL-3.0-only).
// @ts-ignore untyped JS modules
import { Updater } from './flasher/fm1ota.js';
// @ts-ignore untyped JS modules
import { buildFeluccaPackage, logicalImage, productOf, validateStockPackage } from './flasher/fm1pkg.js';

const PRODUCT = 'FM-1_800'; // identity the Modular-1 firmware reports (FELUCCA_ID in the firmware build)

const ERRORS: Record<string, string> = {
  notfound: 'FM-1 not found. Check the USB data cable and close other apps that use the MIDI port.',
  model: 'This package is for a different device.',
  stopped: 'The device stopped answering. Nothing was written if this happened in the check step.',
  noloader: 'The update loader did not appear. Replug USB and press Install again; the device stays in update mode.',
  lost: 'The device was disconnected. Replug USB and press Install again to finish.',
  noreturn: 'The device did not come back: power-cycle it.',
  mismatch: 'Written, but the device reports a different identity.',
  badreq: 'The device asked for data outside the package.',
};

function el<K extends keyof HTMLElementTagNameMap>(tag: K, props: Partial<HTMLElementTagNameMap[K]> & { class?: string } = {}, ...kids: (Node | string)[]): HTMLElementTagNameMap[K] {
  const e = document.createElement(tag);
  const { class: cls, ...rest } = props as Record<string, unknown>;
  if (cls) e.className = String(cls);
  Object.assign(e, rest);
  e.append(...kids);
  return e;
}

async function readFile(f: File): Promise<Uint8Array> {
  return new Uint8Array(await f.arrayBuffer());
}

export function buildFlashTab(root: HTMLElement): void {
  const logEl = el('pre', { class: 'log', textContent: 'Ready.\n' });
  const bar = el('progress', { max: 100, value: 0 });
  const log = (s: string): void => {
    logEl.textContent += s + '\n';
    logEl.scrollTop = logEl.scrollHeight;
  };
  let busy = false;

  const warn = el(
    'div',
    { class: 'card warn' },
    el('h2', { textContent: 'Risk of bricking' }),
    el('p', {
      textContent:
        'Flashing replaces the FM-1 firmware. The FM-1 has a single firmware slot and no rollback. Felucca\'s loader can usually finish an interrupted write (the unit shows up as an update loader) and the official V15 file restores stock, but this is not guaranteed. Keep the USB cable connected, keep the battery charged, and do not close this page while writing.',
    }),
  );
  const ack = el('input', { type: 'checkbox', id: 'ack' });
  const ackLabel = el('label', {}, ack, ' I understand and accept the risk');

  const supported = 'requestMIDIAccess' in navigator;
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  const access = async (): Promise<any> => {
    const a = await navigator.requestMIDIAccess({ sysex: true });
    return new Updater(a, log);
  };

  const run = async (name: string, fn: () => Promise<void>): Promise<void> => {
    if (busy) return;
    busy = true;
    setButtons();
    bar.value = 0;
    log(`\n— ${name}`);
    try {
      await fn();
    } catch (e) {
      const err = e as Error & { code?: string; detail?: string };
      log(`ERROR: ${(err.code && ERRORS[err.code]) || err.message}${err.detail ? ` (${err.detail})` : ''}`);
    } finally {
      busy = false;
      setButtons();
    }
  };

  const step = (kind: string, a?: unknown): void => {
    if (kind === 'start') log(`starting the update on ${a}`);
    else if (kind === 'verify') {
      if (Number(a) === 1) log('checking the package…');
    }
    else if (kind === 'loader') log('switching to update mode');
    else if (kind === 'write') bar.value = Math.min(99, Number(a) / 20);
    else if (kind === 'reboot') bar.value = 100, log('restarting');
    else if (kind === 'done') log(`done: the FM-1 runs ${a}`);
  };

  // ---- device info
  const infoBtn = el('button', { textContent: 'Find FM-1' });
  infoBtn.onclick = () =>
    run('find device', async () => {
      const up = await access();
      const dev = await up.find();
      if (!dev) throw Object.assign(new Error('not found'), { code: 'notfound' });
      log(`${dev.id.text}  [${/^ota-/i.test(dev.id.model) ? 'update loader: an update was not finished' : 'running'}]  port: ${dev.name}`);
      dev.link.close();
    });

  // ---- install a package (.fwsc)
  const pkgFile = el('input', { type: 'file', accept: '.fwsc,.bin' });
  const pkgBtn = el('button', { textContent: 'Install package', class: 'primary' });
  pkgBtn.onclick = () =>
    run('install package', async () => {
      const f = pkgFile.files?.[0];
      if (!f) throw new Error('choose a .fwsc package first');
      const raw = await readFile(f);
      const product = productOf(raw);
      log(`package: ${product} (${f.name}, ${raw.length} bytes)`);
      const mark = new TextEncoder().encode('FELUCCA-LOADER-1');
      const hasLoader = raw.some((_: number, i: number) => i < raw.length - mark.length && mark.every((b, k) => raw[i + k] === b));
      if (!hasLoader) throw new Error('this package has no Felucca update loader; refusing to install it');
      await (await access()).install(logicalImage(raw), product, step);
    });

  // ---- build + install from the official V15 and an app.bin
  const stockFile = el('input', { type: 'file', accept: '.fwsc' });
  const appFile = el('input', { type: 'file', accept: '.bin' });
  const buildBtn = el('button', { textContent: 'Build and install', class: 'primary' });
  buildBtn.onclick = () =>
    run('build and install', async () => {
      const s = stockFile.files?.[0];
      const a = appFile.files?.[0];
      if (!s || !a) throw new Error('choose the official FM-1.fwsc and a Modular-1 app.bin');
      const pkg: Uint8Array = await buildFeluccaPackage(await readFile(s), await readFile(a), PRODUCT);
      log(`built package ${PRODUCT}, ${pkg.length} bytes`);
      await (await access()).install(logicalImage(pkg), PRODUCT, step);
    });

  // ---- return to stock
  const stockOnly = el('input', { type: 'file', accept: '.fwsc' });
  const stockBtn = el('button', { textContent: 'Return to official V15' });
  stockBtn.onclick = () =>
    run('return to official V15', async () => {
      const f = stockOnly.files?.[0];
      if (!f) throw new Error('choose the unmodified official FM-1.fwsc');
      const v = await validateStockPackage(await readFile(f));
      await (await access()).install(v.image, v.product, step);
    });

  // ---- resume an interrupted write
  const resumeFile = el('input', { type: 'file', accept: '.fwsc' });
  const resumeBtn = el('button', { textContent: 'Resume interrupted write' });
  resumeBtn.onclick = () =>
    run('resume', async () => {
      const f = resumeFile.files?.[0];
      if (!f) throw new Error('choose the package that was being written');
      const raw = await readFile(f);
      const ok = await (await access()).resume(logicalImage(raw), step, productOf(raw));
      log(ok ? 'resumed and finished' : 'no device in update mode found');
    });

  const writers = [pkgBtn, buildBtn, stockBtn, resumeBtn];
  function setButtons(): void {
    infoBtn.disabled = busy || !supported;
    for (const b of writers) b.disabled = busy || !supported || !ack.checked;
  }
  ack.onchange = setButtons;

  const section = (title: string, text: string, ...controls: HTMLElement[]): HTMLElement =>
    el('div', { class: 'card' }, el('h2', { textContent: title }), el('p', { class: 'muted', textContent: text }), el('div', { class: 'row' }, ...controls));

  root.replaceChildren(
    ...(supported ? [] : [el('div', { class: 'card warn', textContent: 'Web MIDI with SysEx is needed: use Chrome or Edge over https (or localhost).' })]),
    warn,
    el('div', { class: 'card' }, ackLabel, el('div', { class: 'row' }, infoBtn)),
    section('Install Modular-1 firmware', 'Needs your own copy of the official M-VAVE FM-1.fwsc (V15) and a Modular-1 app.bin from a firmware build. Neither is bundled here.', stockFile, appFile, buildBtn),
    section('Install a finished package', 'A .fwsc made by tools/build.py (it must carry the Felucca update loader).', pkgFile, pkgBtn),
    section('Return to stock', 'Needs the unmodified official FM-1.fwsc (V15); anything else is refused.', stockOnly, stockBtn),
    section('Finish an interrupted write', 'If an install was cut off the FM-1 stays in update mode. Choose the same package and resume.', resumeFile, resumeBtn),
    el('div', { class: 'card' }, bar, logEl),
  );
  setButtons();
}
