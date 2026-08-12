// Queue-based audio output: the main thread posts Float32Array chunks; this
// processor drains them at the hardware rate. No SharedArrayBuffer (GitHub
// Pages cannot serve COOP/COEP headers).
//
// Latency discipline: the queue is HARD-CAPPED. If the producer ever gets
// ahead (tab hiccups, clock drift), the oldest samples are dropped so
// audible delay can never exceed CAP_SECONDS - the main thread's depth
// gating keeps it far below that in normal play.
const CAP_SECONDS = 0.25;

class MZAudioProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.queue = [];
    this.queued = 0; // total samples queued
    this.offset = 0; // read offset into queue[0]
    this.underruns = 0;
    this.dropped = 0;
    this.capture = false;
    this.captureBuffer = new Float32Array(1024);
    this.captureUsed = 0;
    this.port.onmessage = (e) => {
      if (e.data.samples) {
        this.queue.push(e.data.samples);
        this.queued += e.data.samples.length;
        const cap = sampleRate * CAP_SECONDS;
        while (this.queued > cap && this.queue.length > 1) {
          const old = this.queue.shift();
          this.queued -= old.length - this.offset;
          this.dropped += old.length - this.offset;
          this.offset = 0;
        }
      } else if (e.data.flush) {
        this.queue = [];
        this.queued = 0;
        this.offset = 0;
      } else if (e.data.query) {
        this.port.postMessage({
          queued: this.queued,
          underruns: this.underruns,
          dropped: this.dropped,
        });
      } else if (e.data.capture !== undefined) {
        this.capture = !!e.data.capture;
        this.captureUsed = 0;
      }
    };
  }

  process(inputs, outputs) {
    const input = inputs[0] && inputs[0][0];
    if (this.capture && input) {
      for (let i = 0; i < input.length; i++) {
        this.captureBuffer[this.captureUsed++] = input[i];
        if (this.captureUsed === this.captureBuffer.length) {
          const chunk = this.captureBuffer;
          this.captureBuffer = new Float32Array(1024);
          this.captureUsed = 0;
          this.port.postMessage({ input: chunk, rate: sampleRate }, [chunk.buffer]);
        }
      }
    }
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
