// Decode the YM2203 register shadow into "what is sounding right now".
//
// Clocking (chip at 2 MHz, default prescaler 6 — vendor/ymfm/ymfm_opn.h):
//   FM sample rate fs = clock / 72 = 27778 Hz,
//     freq = fnum * fs * 2^(block-1) / 2^20
//   SSG effective clock = clock * 3 / 6 = 1 MHz (AY-style divider 16),
//     freq = 1e6 / (16 * period) = 62500 / period

const OPN_CLOCK_HZ = 2_000_000;
const FM_FS = OPN_CLOCK_HZ / 72;
const SSG_TONE_HZ = (OPN_CLOCK_HZ * 3) / 6 / 16; // 62500

const NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];

export function noteName(freq: number): string {
  if (!(freq > 0)) return "-";
  const n = Math.round(12 * Math.log2(freq / 440) + 69); // MIDI note
  if (n < 0 || n > 127) return "-";
  const cents = Math.round(1200 * Math.log2(freq / (440 * Math.pow(2, (n - 69) / 12))));
  const name = `${NOTE_NAMES[n % 12]}${Math.floor(n / 12) - 1}`;
  return cents === 0 ? name : `${name}${cents > 0 ? "+" : ""}${cents}c`;
}

export interface FmChannelState {
  ch: number;
  keyOn: boolean;
  slotMask: number;
  freqHz: number;
  note: string;
  algorithm: number;
  feedback: number;
  carrierTotalLevel: number; // reg 4Ch+ch (operator 4, the algorithm-7 carrier proxy)
}

export interface SsgChannelState {
  ch: number;
  toneEnabled: boolean;
  noiseEnabled: boolean;
  period: number;
  freqHz: number;
  note: string;
  volume: number; // 0-15
  envelope: boolean;
}

export interface SoundState {
  fm: FmChannelState[];
  ssg: SsgChannelState[];
  noisePeriod: number;
  envelopePeriod: number;
  envelopeShape: number;
  beep: boolean;
}

export function decodeSoundState(
  regs: Uint8Array,
  fmKeyon: (ch: number) => number,
  beep: boolean,
): SoundState {
  const fm: FmChannelState[] = [];
  for (let ch = 0; ch < 3; ch++) {
    const fnum = regs[0xa0 + ch] | ((regs[0xa4 + ch] & 0x07) << 8);
    const block = (regs[0xa4 + ch] >> 3) & 0x07;
    const freq = block > 0 ? (fnum * FM_FS * Math.pow(2, block - 1)) / Math.pow(2, 20) : (fnum * FM_FS) / Math.pow(2, 21);
    const slots = fmKeyon(ch) & 0x0f;
    fm.push({
      ch,
      keyOn: slots !== 0,
      slotMask: slots,
      freqHz: Math.round(freq * 10) / 10,
      note: noteName(freq),
      algorithm: regs[0xb0 + ch] & 0x07,
      feedback: (regs[0xb0 + ch] >> 3) & 0x07,
      carrierTotalLevel: regs[0x4c + ch] & 0x7f,
    });
  }

  const mixer = regs[0x07];
  const ssg: SsgChannelState[] = [];
  for (let ch = 0; ch < 3; ch++) {
    const period = regs[ch * 2] | ((regs[ch * 2 + 1] & 0x0f) << 8);
    const freq = period > 0 ? SSG_TONE_HZ / period : 0;
    ssg.push({
      ch,
      toneEnabled: (mixer & (1 << ch)) === 0, // active-low
      noiseEnabled: (mixer & (1 << (ch + 3))) === 0,
      period,
      freqHz: Math.round(freq * 10) / 10,
      note: noteName(freq),
      volume: regs[0x08 + ch] & 0x0f,
      envelope: (regs[0x08 + ch] & 0x10) !== 0,
    });
  }

  return {
    fm,
    ssg,
    noisePeriod: regs[0x06] & 0x1f,
    envelopePeriod: regs[0x0b] | (regs[0x0c] << 8),
    envelopeShape: regs[0x0d] & 0x0f,
    beep,
  };
}
