// SPDX-License-Identifier: GPL-3.0-only
// Simulator tab: the firmware package bundled with this app (public/firmware/modular-1.fwsc) running on an emulated FM-1, in an iframe.
// The emulator is fm1-emu by Joakim Eriksson (MIT, https://github.com/joakimeriksson/fm1-emu): the FM-1's CPU, screen, keys, knobs and audio
// in WebAssembly, copied to public/sim/ by tools/build_simulator.sh. It runs the real firmware, not a model of it. It is loaded only when
// the tab is opened, and it starts the machine (and the audio) on its own Start button.

const SIM = 'sim/index.html?embed=1&fw=modular-1';

function el<K extends keyof HTMLElementTagNameMap>(tag: K, props: Partial<HTMLElementTagNameMap[K]> & { class?: string } = {}, ...kids: (Node | string)[]): HTMLElementTagNameMap[K] {
  const e = document.createElement(tag);
  const { class: cls, ...rest } = props as Record<string, unknown>;
  if (cls) e.className = String(cls);
  Object.assign(e, rest);
  e.append(...kids);
  return e;
}

/** Builds the tab; the iframe is created by the returned function, which main.ts calls when the tab is first shown. */
export function buildSimTab(root: HTMLElement): () => void {
  let loaded = false;
  const frameBox = el('div', { class: 'sim-box' });
  const version = el('span', { class: 'muted', textContent: '' });
  root.append(
    el(
      'div',
      { class: 'card' },
      el('h2', { textContent: 'Simulator' }),
      el(
        'p',
        {},
        'The FM-1 firmware of this app, running on an emulated FM-1: the screen, the 27 keys, the knobs and buttons, the audio. It is the same code that runs on the device, so what you see here is what the device does (the editor link over USB is not part of it). Click ',
        el('b', { textContent: 'Start' }),
        ' in the panel to boot it. ',
        version,
      ),
      el(
        'p',
        { class: 'muted' },
        'Keys: A … ; play the keyboard, Z / X the octave, Space is PLAY. Drag or scroll a knob. SEL steps through the voices, GLO opens the mixer, HOME held opens the settings menu with the manual. The emulator keeps the saved state (SAVE) in this browser. If the sound breaks up, lower the clock under the panel. ',
        el('a', { href: 'sim/index.html?fw=modular-1', target: '_blank', rel: 'noopener', textContent: 'Open it in its own page ↗' }),
      ),
      el(
        'p',
        { class: 'muted' },
        'Emulator: ',
        el('a', { href: 'https://github.com/joakimeriksson/fm1-emu', textContent: 'fm1-emu', target: '_blank', rel: 'noopener' }),
        ' by Joakim Eriksson (MIT). Not the M-VAVE firmware: it runs Modular-1 only (a different .fwsc can be opened in the panel).',
      ),
    ),
    frameBox,
  );
  fetch('firmware/modular-1.json')
    .then((r) => r.json())
    .then((j: { version?: string }) => (version.textContent = j.version ? `Firmware ${j.version}.` : ''))
    .catch(() => undefined);
  return () => {
    if (loaded) return;
    loaded = true;
    fetch('sim/firmwares.json', { method: 'HEAD' })
      .then((r) => {
        if (!r.ok) throw new Error(String(r.status));
        frameBox.append(el('iframe', { src: SIM, class: 'sim-frame', title: 'FM-1 simulator', allow: 'autoplay; midi' }));
      })
      .catch(() => {
        frameBox.append(el('p', { class: 'muted', textContent: 'The simulator is not part of this build (public/sim/ is missing): run tools/build_simulator.sh.' }));
      });
  };
}
