// SPDX-License-Identifier: GPL-3.0-only
// The FM-1 as a Web MIDI SysEx peer: patch upload, store, live parameters (wire format: protocol.ts).
import { DEFS } from './modules';
import type { Patch } from './patch';
import { encodePatch, toQ16 } from './patchbin';
import { attFrame, chunkFrames, CMD, parseSlot, slotEraseFrame, slotFrame, slotInfoFrame, SLOTS, type SlotInfo, commitFrame, helloFrame, paramFrame, parse, parseAck, parseInfo, STATUS_TEXT, storeFrame, type Ack, type Info } from './protocol';

const NAME = /felucca|fm-?1|modular/i;
const ACK_TIMEOUT = 700;
const FLASH_TIMEOUT = 2500; // a flash sector erase and program
const TRIES = 3;

export class DeviceLink {
  private out: MIDIOutput | null = null;
  private inp: MIDIInput | null = null;
  private waiter: { match: (p: ReturnType<typeof parse>) => boolean; done: (p: NonNullable<ReturnType<typeof parse>>) => void } | null = null;
  private chain: Promise<unknown> = Promise.resolve();
  private paramQueue = new Map<string, [number, number, number]>();
  private paramTimer = 0;
  info: Info | null = null;
  onStatus: (s: string) => void = () => {};

  get connected(): boolean {
    return !!this.out && this.out.state === 'connected';
  }

  async connect(): Promise<Info> {
    if (!navigator.requestMIDIAccess) throw new Error('Web MIDI is not available in this browser (use Chrome or Edge)');
    if (this.inp) this.inp.onmidimessage = null; // a reconnect: let go of the old port
    const access = await navigator.requestMIDIAccess({ sysex: true });
    const pick = <T extends MIDIPort>(ports: Iterable<T>): T | null => [...ports].find((p) => NAME.test(p.name ?? '') && !/ota-/i.test(p.name ?? '')) ?? null;
    this.out = pick(access.outputs.values());
    this.inp = pick(access.inputs.values());
    if (!this.out || !this.inp) throw new Error('FM-1 not found. Is it connected and running the Modular-1 firmware?');
    this.inp.onmidimessage = (ev) => {
      const p = ev.data ? parse(ev.data) : null;
      if (p && this.waiter?.match(p)) {
        const w = this.waiter;
        this.waiter = null;
        w.done(p);
      }
    };
    access.onstatechange = () => {
      if (this.out?.state !== 'connected') this.onStatus('FM-1 disconnected');
    };
    return this.hello();
  }

  private request(frame: Uint8Array, match: (p: ReturnType<typeof parse>) => boolean, timeout = ACK_TIMEOUT): Promise<NonNullable<ReturnType<typeof parse>>> {
    return new Promise((resolve, reject) => {
      let tries = 0;
      const attempt = (): void => {
        tries++;
        const t = window.setTimeout(() => {
          this.waiter = null;
          if (tries < TRIES) attempt();
          else reject(new Error('no answer from the FM-1'));
        }, timeout);
        this.waiter = {
          match,
          done: (p) => {
            clearTimeout(t);
            resolve(p);
          },
        };
        this.out!.send(frame);
      };
      attempt();
    });
  }

  async hello(): Promise<Info> {
    const p = await this.request(helloFrame(), (x) => x?.cmd === CMD.info);
    this.info = parseInfo(p);
    if (!this.info) throw new Error('unexpected answer to HELLO');
    return this.info;
  }

  private async ackFor(frame: Uint8Array, cmd: number, seq?: number, timeout = ACK_TIMEOUT): Promise<Ack> {
    const p = await this.request(frame, (x) => x?.cmd === CMD.ack && x.payload[0] === cmd && (seq === undefined || x.payload[1] === seq), timeout);
    const a = parseAck(p)!;
    if (a.status) throw new Error(`FM-1: ${STATUS_TEXT[a.status] ?? `error ${a.status}`}${a.aux ? ` (${a.aux})` : ''}`);
    return a;
  }

  /** Upload and switch to the patch. Calls are serialised. */
  upload(patch: Patch): Promise<void> {
    const run = async (): Promise<void> => {
      const bin = encodePatch(patch);
      await this.sendChunks(bin);
      await this.ackFor(commitFrame(bin), CMD.commit);
    };
    return this.serial(run);
  }

  private async sendChunks(bin: Uint8Array): Promise<void> {
    const frames = chunkFrames(bin);
    for (let i = 0; i < frames.length; i++) await this.ackFor(frames[i], CMD.chunk, i & 0x7f);
  }

  /** Put a patch into patch slot 0..31 of the FM-1's bank (flash); it is not loaded. */
  uploadSlot(slot: number, name: string, patch: Patch): Promise<void> {
    return this.serial(async () => {
      const bin = encodePatch(patch);
      await this.sendChunks(bin);
      await this.ackFor(slotFrame(slot, name, bin), CMD.slot, slot, FLASH_TIMEOUT);
    });
  }

  eraseSlot(slot: number): Promise<void> {
    return this.serial(async () => {
      await this.ackFor(slotEraseFrame(slot), CMD.slotErase, slot, FLASH_TIMEOUT);
    });
  }

  readSlot(slot: number): Promise<SlotInfo> {
    return this.serial(async () => {
      const p = await this.request(slotInfoFrame(slot), (x) => x?.cmd === CMD.slotReply && x.payload[0] === slot);
      const s = parseSlot(p);
      if (!s) throw new Error('unexpected answer to SLOT_INFO');
      return s;
    });
  }

  /** all 32 slots of the bank */
  async readBank(): Promise<SlotInfo[]> {
    const out: SlotInfo[] = [];
    for (let s = 0; s < SLOTS; s++) out.push(await this.readSlot(s));
    return out;
  }

  store(): Promise<void> {
    return this.serial(async () => {
      await this.ackFor(storeFrame(), CMD.store);
    });
  }

  private backlog = 0;

  private serial<T>(fn: () => Promise<T>): Promise<T> {
    if (this.backlog > 40) return Promise.reject(new Error('too many requests waiting for the FM-1: is it answering?'));
    this.backlog++;
    const next = this.chain.then(fn, fn).finally(() => this.backlog--);
    this.chain = next.catch(() => {});
    return next;
  }

  /** A live parameter change; the latest value per parameter is sent every 25 ms. */
  param(patch: Patch, moduleId: number, name: string, value: number): void {
    if (!this.connected) return;
    const mi = patch.modules.findIndex((m) => m.id === moduleId);
    if (mi < 0) return;
    const pi = DEFS[patch.modules[mi].type].params.findIndex((p) => p.name === name);
    if (pi < 0) return;
    this.paramQueue.set(`${mi}.${pi}`, [mi, pi, toQ16(value)]);
    if (!this.paramTimer)
      this.paramTimer = window.setTimeout(() => {
        this.paramTimer = 0;
        const q = [...this.paramQueue.values()];
        this.paramQueue.clear();
        for (const [m, p, v] of q) this.out?.send(paramFrame(m, p, v));
      }, 25);
  }

  /** a live attenuverter change (input 0 = p1, 1 = p2) */
  att(patch: Patch, moduleId: number, index: number, value: number): void {
    if (!this.connected) return;
    const mi = patch.modules.findIndex((m) => m.id === moduleId);
    if (mi >= 0) this.out!.send(attFrame(mi, index, toQ16(value)));
  }

  note(msg: { t: string; note?: number; vel?: number }): void {
    if (!this.connected) return;
    if (msg.t === 'on') this.out!.send([0x90, msg.note! & 0x7f, Math.max(1, Math.round((msg.vel ?? 1) * 127))]);
    else if (msg.t === 'off') this.out!.send([0x80, msg.note! & 0x7f, 0]);
    else if (msg.t === 'panic') this.out!.send([0xb0, 123, 0]);
  }
}
