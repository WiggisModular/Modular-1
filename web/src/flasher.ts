// SPDX-License-Identifier: GPL-3.0-only
// Flash tab. The USB-MIDI update protocol and package handling come from Felucca
// (src/flasher/fm1ota.js, fm1pkg.js; Copyright (C) 2026 Leo Kuroshita, GPL-3.0-only).
// @ts-ignore untyped JS modules
import { Updater } from './flasher/fm1ota.js';
// @ts-ignore untyped JS modules
import { logicalImage, productOf, validateStockPackage } from './flasher/fm1pkg.js';


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

/** the firmware package that comes with this copy of the app (web/public/firmware, made by `npm run firmware`) */
interface Bundle {
  version: string;
  product: string;
  size: number;
  sha256: string;
}

async function loadBundle(): Promise<Bundle | null> {
  try {
    const r = await fetch('./firmware/modular-1.json', { cache: 'no-cache' });
    if (!r.ok) return null;
    const b = (await r.json()) as Bundle; // a dev server answers a missing file with the page itself: that is not JSON
    return b && typeof b.version === 'string' && typeof b.product === 'string' ? b : null;
  } catch {
    return null;
  }
}

export function buildFlashTab(root: HTMLElement): void {
  const logEl = el('pre', { class: 'log', textContent: 'Ready.\n' });
  const bar = el('progress', { max: 100, value: 0 });
  const log = (s: string): void => {
    logEl.textContent += s + '\n';
    logEl.scrollTop = logEl.scrollHeight;
  };
  let busy = false;
  let bundle: Bundle | null = null;

  const ack = el('input', { type: 'checkbox', id: 'ack' });
  const supported = 'requestMIDIAccess' in navigator;
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  const access = async (): Promise<any> => new Updater(await navigator.requestMIDIAccess({ sysex: true }), log);

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
    } else if (kind === 'loader') log('switching to update mode');
    else if (kind === 'write') bar.value = Math.min(99, Number(a) / 20);
    else if (kind === 'reboot') (bar.value = 100), log('restarting');
    else if (kind === 'done') log(`done: the FM-1 runs ${a}`);
  };

  /** install a package; if an earlier install was cut off (the FM-1 sits in its update loader) finish that write instead */
  const install = async (raw: Uint8Array): Promise<void> => {
    const product = productOf(raw);
    log(`package: ${product}, ${raw.length} bytes`);
    const mark = new TextEncoder().encode('FELUCCA-LOADER-1');
    if (!raw.some((_: number, i: number) => i < raw.length - mark.length && mark.every((b, k) => raw[i + k] === b))) throw new Error('this package has no update loader; refusing to install it');
    const up = await access();
    if (await up.resume(logicalImage(raw), step, product)) {
      log('an earlier update was unfinished: it is finished now');
      return;
    }
    await up.install(logicalImage(raw), product, step);
  };

  // ---- Modular-1
  const installBtn = el('button', { textContent: 'Install Modular-1 firmware', class: 'primary' });
  const bundleInfo = el('p', { class: 'muted', textContent: 'Looking for the firmware package…' });
  installBtn.onclick = () =>
    run('install Modular-1', async () => {
      if (!bundle) throw new Error('no firmware package is bundled with this copy of the app');
      const r = await fetch(`./firmware/modular-1.fwsc?v=${bundle.sha256}`, { cache: 'no-cache' });
      if (!r.ok) throw new Error('the bundled firmware package could not be loaded');
      const raw = new Uint8Array(await r.arrayBuffer());
      const sum = Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256', raw)), (b) => b.toString(16).padStart(2, '0')).join('');
      if (sum !== bundle.sha256) throw new Error('the bundled firmware package does not match its checksum');
      await install(raw);
    });
  void loadBundle().then((b) => {
    bundle = b;
    bundleInfo.textContent = b
      ? `Modular-1 ${b.version} (${b.product}, ${Math.round(b.size / 1024)} KB) comes with this app. GPL-3.0; three JieLi SDK files inside are Apache-2.0.`
      : 'No firmware package comes with this copy of the app. Build one (tools/build.py), run npm run firmware in web/, and reload; or choose a .fwsc file below.';
    setButtons();
  });

  const findBtn = el('button', { textContent: 'Check the connection' });
  findBtn.onclick = () =>
    run('find device', async () => {
      const dev = await (await access()).find();
      if (!dev) throw Object.assign(new Error('not found'), { code: 'notfound' });
      log(`${dev.id.text}  [${/^ota-/i.test(dev.id.model) ? 'update loader: an update was not finished' : 'running'}]  port: ${dev.name}`);
      dev.link.close();
    });

  // ---- another package
  const pkgFile = el('input', { type: 'file', accept: '.fwsc' });
  const pkgBtn = el('button', { textContent: 'Install this package' });
  pkgBtn.onclick = () =>
    run('install package', async () => {
      const f = pkgFile.files?.[0];
      if (!f) throw new Error('choose a .fwsc package first');
      await install(await readFile(f));
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

  function setButtons(): void {
    findBtn.disabled = busy || !supported;
    installBtn.disabled = busy || !supported || !ack.checked || !bundle;
    for (const b of [pkgBtn, stockBtn]) b.disabled = busy || !supported || !ack.checked;
  }
  ack.onchange = setButtons;

  const card = (title: string, ...kids: (Node | string)[]): HTMLElement => el('div', { class: 'card' }, el('h2', { textContent: title }), ...kids);

  root.replaceChildren(
    ...(supported ? [] : [el('div', { class: 'card warn', textContent: 'Web MIDI with SysEx is needed: use Chrome, Edge or Firefox over https (or localhost).' })]),
    el(
      'div',
      { class: 'card warn' },
      el('h2', { textContent: 'Before you flash' }),
      el('p', {
        textContent:
          'Flashing replaces the FM-1 firmware. It has one firmware slot and no rollback. If an update is cut off the FM-1 waits in its update loader and Install finishes it; the official V15 file brings the stock firmware back. Keep the USB cable and the battery connected and keep this page open.',
      }),
      el('p', {
        textContent:
          'The FM-1 disconnects and comes back several times. Chromium on Linux can close the whole browser when that happens (a bug in its MIDI backend): close other tabs first, or install with tools/fm1_install.py.',
      }),
      el('label', {}, ack, ' I understand and accept the risk'),
    ),
    card(
      'Install Modular-1',
      bundleInfo,
      el('div', { class: 'row' }, installBtn, findBtn),
      el('p', { class: 'muted' }, 'Licences: ', el('a', { href: './firmware/LICENSE-GPL-3.0.txt', textContent: 'GPL-3.0' }), ', ', el('a', { href: './firmware/LICENSE-Apache-2.0.txt', textContent: 'Apache-2.0' }), '. Source: ', el('a', { href: 'https://github.com/morgulbrut/Modular-1', textContent: 'github.com/morgulbrut/Modular-1' }), '.'),
    ),
    card('Return to stock', el('p', { class: 'muted', textContent: 'Needs the unmodified official FM-1.fwsc (V15); anything else is refused.' }), el('div', { class: 'row' }, stockOnly, stockBtn)),
    el(
      'details',
      { class: 'card' },
      el('summary', { textContent: 'Install another package' }),
      el('p', { class: 'muted', textContent: 'A finished .fwsc from tools/build.py (it must carry the update loader).' }),
      el('div', { class: 'row' }, pkgFile, pkgBtn),
    ),
    el('div', { class: 'card' }, bar, logEl),
  );
  setButtons();
}
