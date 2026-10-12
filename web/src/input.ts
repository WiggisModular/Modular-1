// SPDX-License-Identifier: GPL-3.0-only
// Computer-keyboard and Web MIDI note input.
import type { AudioHost } from './audio';

const KEYS = 'awsedftgyhujkolp';

export function attachKeyboard(host: Pick<AudioHost, 'send'>, isActive: () => boolean): void {
  let octave = 0;
  const down = new Map<string, number>();
  const editable = (t: EventTarget | null): boolean => t instanceof HTMLElement && (t.tagName === 'SELECT' || (t.tagName === 'INPUT' && (t as HTMLInputElement).type !== 'range') || t.isContentEditable);
  window.addEventListener('keydown', (e) => {
    if (!isActive() || e.repeat || e.ctrlKey || e.metaKey || e.altKey || editable(e.target)) return;
    const k = e.key.toLowerCase();
    if (k === 'z') octave = Math.max(-3, octave - 1);
    else if (k === 'x') octave = Math.min(3, octave + 1);
    const i = KEYS.indexOf(k);
    if (i >= 0 && !down.has(k)) {
      const note = 60 + octave * 12 + i;
      down.set(k, note);
      host.send({ t: 'on', note, vel: 0.8 });
    }
  });
  window.addEventListener('keyup', (e) => {
    const k = e.key.toLowerCase();
    const note = down.get(k);
    if (note !== undefined) {
      down.delete(k);
      host.send({ t: 'off', note });
    }
  });
  window.addEventListener('blur', () => {
    for (const note of down.values()) host.send({ t: 'off', note });
    down.clear();
  });
}

export async function attachMidi(host: Pick<AudioHost, 'send'>, status: (s: string) => void): Promise<void> {
  if (!navigator.requestMIDIAccess) return status('Web MIDI is not available in this browser');
  try {
    const access = await navigator.requestMIDIAccess();
    const hook = (): void => {
      for (const input of access.inputs.values()) {
        input.onmidimessage = (ev) => {
          const [s, a, b] = ev.data ?? [];
          if (s === 0xf8) return void host.send({ t: 'clock' });
          if (s === 0xfa || s === 0xfb || s === 0xfc) return void host.send({ t: 'clockrun', on: s !== 0xfc });
          const cmd = s & 0xf0;
          if (cmd === 0x90 && b > 0) host.send({ t: 'on', note: a, vel: b / 127 });
          else if (cmd === 0x80 || (cmd === 0x90 && b === 0)) host.send({ t: 'off', note: a });
          else if (cmd === 0xb0 && a === 123) host.send({ t: 'panic' });
        };
      }
      status(`${access.inputs.size} MIDI input(s)`);
    };
    access.onstatechange = hook;
    hook();
  } catch (e) {
    status(`MIDI access denied: ${e}`);
  }
}
