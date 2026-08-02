// Queue-based audio output: the main thread posts Float32Array chunks after
// each emulated frame; this processor drains them at the hardware rate.
// No SharedArrayBuffer (GitHub Pages cannot serve COOP/COEP headers).
class MZAudioProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.queue = [];
    this.queued = 0; // total samples queued
    this.offset = 0; // read offset into queue[0]
    this.underruns = 0;
    this.port.onmessage = (e) => {
      if (e.data.samples) {
        this.queue.push(e.data.samples);
        this.queued += e.data.samples.length;
      } else if (e.data.query) {
        this.port.postMessage({ queued: this.queued, underruns: this.underruns });
      }
    };
  }

  process(inputs, outputs) {
    const out = outputs[0][0];
    let i = 0;
    while (i < out.length && this.queue.length > 0) {
      const head = this.queue[0];
      out[i++] = head[this.offset++];
      this.queued--;
      if (this.offset >= head.length) {
        this.queue.shift();
        this.offset = 0;
      }
    }
    if (i < out.length) {
      this.underruns++;
      while (i < out.length) out[i++] = 0;
    }
    if (outputs[0].length > 1) outputs[0][1].set(out);
    return true;
  }
}
registerProcessor("mz-audio", MZAudioProcessor);
