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
  | { t: 'on'; note: number; vel: number }
  | { t: 'off'; note: number }
  | { t: 'panic' };

class ModularProcessor extends AudioWorkletProcessor {
  private engine = new Engine(sampleRate);
  constructor() {
    super();
    this.port.onmessage = (ev: MessageEvent<HostMsg>) => {
      const m = ev.data;
      try {
        if (m.t === 'patch') this.engine.load(m.patch);
        else if (m.t === 'param') this.engine.setParam(m.id, m.name, m.value);
        else if (m.t === 'on') this.engine.noteOn(m.note, m.vel);
        else if (m.t === 'off') this.engine.noteOff(m.note);
        else if (m.t === 'panic') this.engine.allNotesOff();
      } catch (e) {
        this.port.postMessage({ error: String(e) });
      }
    };
  }
  process(_in: Float32Array[][], outputs: Float32Array[][]): boolean {
    const out = outputs[0];
    this.engine.render(out[0], out[1] ?? out[0]);
    return true;
  }
}
registerProcessor('modular-1', ModularProcessor as never);
