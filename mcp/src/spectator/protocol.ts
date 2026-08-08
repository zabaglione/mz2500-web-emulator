// Binary framing for the spectator stream: one JSON header line, then
// length-prefixed messages. The hub encodes; tests decode with the same
// module. The viewer page carries its own tiny JS decoder (viewer.ts) —
// keep the two in sync.
export const MSG = {
  heartbeat: 0x00,
  video: 0x01,
  repeat: 0x02,
  audio: 0x03,
  state: 0x04,
} as const;
export type MsgType = (typeof MSG)[keyof typeof MSG];

export interface StreamInfo {
  version: 1;
  width: number;
  height: number;
  audioRate: number;
}

export function encodeHeader(info: StreamInfo): Buffer {
  return Buffer.from(JSON.stringify(info) + "\n", "utf8");
}

/** [length u32LE][type u8][payload…]; length counts the type byte + payload. */
export function encodeMessage(type: MsgType, payload: Buffer = Buffer.alloc(0)): Buffer {
  const head = Buffer.alloc(5);
  head.writeUInt32LE(1 + payload.length, 0);
  head.writeUInt8(type, 4);
  return Buffer.concat([head, payload]);
}

/** Prefix a message body with its frame number. */
export function withFrameNo(frameNo: number, body: Buffer = Buffer.alloc(0)): Buffer {
  const head = Buffer.alloc(4);
  head.writeUInt32LE(frameNo >>> 0, 0);
  return Buffer.concat([head, body]);
}

/** Float32 [-1,1] to mono s16LE with clipping (same convention as wav.ts). */
export function floatTo16(samples: Float32Array): Buffer {
  const out = Buffer.alloc(samples.length * 2);
  for (let i = 0; i < samples.length; i++) {
    let v = Math.round(samples[i] * 32767);
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    out.writeInt16LE(v, i * 2);
  }
  return out;
}

/** Incremental decoder (tests + reference for the viewer's inline JS). */
export class MessageReader {
  private buf = Buffer.alloc(0);
  header: StreamInfo | null = null;

  feed(chunk: Buffer): void {
    this.buf = Buffer.concat([this.buf, chunk]);
    if (!this.header) {
      const nl = this.buf.indexOf(0x0a);
      if (nl >= 0) {
        this.header = JSON.parse(this.buf.subarray(0, nl).toString("utf8")) as StreamInfo;
        this.buf = this.buf.subarray(nl + 1);
      }
    }
  }

  /** Decode the next item, or return null until more bytes arrive. */
  next(): { type: MsgType; frameNo?: number; body: Buffer } | null {
    if (this.buf.length < 4) return null;
    const len = this.buf.readUInt32LE(0);
    if (this.buf.length < 4 + len) return null;
    const type = this.buf.readUInt8(4) as MsgType;
    const payload = Buffer.from(this.buf.subarray(5, 4 + len));
    this.buf = this.buf.subarray(4 + len);
    if (type === MSG.heartbeat) return { type, body: payload };
    return { type, frameNo: payload.readUInt32LE(0), body: payload.subarray(4) };
  }
}
