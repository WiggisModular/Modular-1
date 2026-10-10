// SPDX-License-Identifier: GPL-3.0-only
import workletUrl from './worklet.ts?worker&url';
import type { Patch } from './patch';
import type { HostMsg } from './worklet';

export class AudioHost {
  private ctx: AudioContext | null = null;
  private node: AudioWorkletNode | null = null;
  private pending: Patch | null = null;
  onError: (msg: string) => void = () => {};

  get running(): boolean {
    return this.ctx?.state === 'running';
  }

  async start(): Promise<void> {
    if (!this.ctx) {
      const ctx = new AudioContext({ latencyHint: 'interactive' });
      await ctx.audioWorklet.addModule(workletUrl);
      const node = new AudioWorkletNode(ctx, 'modular-1', { numberOfInputs: 0, outputChannelCount: [2] });
      node.port.onmessage = (ev) => {
        if (ev.data?.error) this.onError(ev.data.error);
      };
      node.connect(ctx.destination);
      this.ctx = ctx;
      this.node = node;
      if (this.pending) this.send({ t: 'patch', patch: this.pending });
    }
    await this.ctx.resume();
  }

  /** Last patch is replayed when the context starts. */
  setPatch(patch: Patch): void {
    this.pending = patch;
    this.send({ t: 'patch', patch });
  }

  send(msg: HostMsg): void {
    this.node?.port.postMessage(msg);
  }
}
