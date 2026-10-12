// SPDX-License-Identifier: GPL-3.0-only
// AudioWorklet wrapper around Engine.
import { Engine } from './engine';
import type { Patch } from './patch';

declare const sampleRate: number;
declare class AudioWorkletProcessor {
  readonly port: MessagePort;
}
declare function registerProcessor(name: string, ctor: new () => AudioWorkletProcessor): void;

export type HostMsg =
  | { t: 'patch'; patch: Patch }
  | { t: 'param'; id: number; name: string; value: number }
  | { t: 'att'; id: number; index: number; value: number }
  | { t: 'on'; note: number; vel: number }
  | { t: 'off'; note: number }
  | { t: 'clock' }
  | { t: 'clockrun'; on: boolean }
  | { t: 'panic' };

class ModularProcessor extends AudioWorkletProcessor {
  private engine = new Engine(sampleRate);
  private failed = false;
  constructor() {
    super();
    this.port.onmessage = (ev: MessageEvent<HostMsg>) => {
      const m = ev.data;
      try {
        if (m.t === 'patch') {
          this.engine.load(m.patch);
          this.failed = false;
        }
        else if (m.t === 'param') this.engine.setParam(m.id, m.name, m.value);
        else if (m.t === 'att') this.engine.setAtt(m.id, m.index, m.value);
        else if (m.t === 'on') this.engine.noteOn(m.note, m.vel);
        else if (m.t === 'off') this.engine.noteOff(m.note);
        else if (m.t === 'clock') this.engine.clockTick();
        else if (m.t === 'clockrun') this.engine.clockRun(m.on);
        else if (m.t === 'panic') this.engine.allNotesOff();
      } catch (e) {
        this.port.postMessage({ error: String(e) });
      }
    };
  }
  process(_in: Float32Array[][], outputs: Float32Array[][]): boolean {
    const out = outputs[0];
    try {
      this.engine.render(out[0], out[1] ?? out[0]);
    } catch (e) {
      if (!this.failed) this.port.postMessage({ error: `engine: ${e}` });
      this.failed = true; // stay silent instead of throwing in the audio thread on every block
      out[0].fill(0);
      out[1]?.fill(0);
    }
    return true;
  }
}
registerProcessor('modular-1', ModularProcessor as never);
