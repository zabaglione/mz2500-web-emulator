// Node host for the MZ-2500 WASM core: loads build/wasm/mz2500w.js, wraps
// the emu_* C ABI (wasm/bindings.cpp) and layers the tool-facing behaviour
// on top — typed key schedules, text waits, audio capture.
//
// Time only advances inside these methods (the agreed tool-driven model):
// between MCP tool calls the machine is frozen, so identical call sequences
// give identical machines.
import { createRequire } from "node:module";
import { readFileSync } from "node:fs";
import { keyForChar, keyFromName, SHIFT_POS, type KeyPos } from "./keymap.js";

interface WasmModule {
  _emu_init(rate: number): number;
  _emu_insert_disk(drive: number, ptr: number, size: number): number;
  _emu_insert_blank_disk(drive: number): number;
  _emu_disk_dirty(drive: number): number;
  _emu_disk_snapshot(drive: number): number;
  _emu_disk_data(): number;
  _emu_disk_clear_dirty(drive: number): void;
  _emu_boot(): number;
  _emu_boot_real_ipl(): number;
  _emu_has_ipl(): number;
  _emu_set_rom(kind: number, ptr: number, size: number): number;
  _emu_run_frame(): void;
  _emu_render(): void;
  _emu_frame_buffer(): number;
  _emu_audio_buffer(): number;
  _emu_audio_capacity(): number;
  _emu_read_audio(): number;
  _emu_key(row: number, bit: number, down: number): void;
  _emu_joy(mask: number): void;
  _emu_frames(): number;
  _emu_debug_json(): number;
  _emu_read_mem(addr: number): number;
  _emu_poke(addr: number, value: number): void;
  _emu_read_phys(phys: number): number;
  _emu_screen_text(): number;
  _emu_opn_regs(): number;
  _emu_fm_keyon(ch: number): number;
  _emu_beep(): number;
  _malloc(size: number): number;
  _free(ptr: number): void;
  HEAPU8: Uint8Array;
  HEAPF32: Float32Array;
  UTF8ToString(ptr: number): string;
}

export const ROM_KIND = { ipl: 0, kanji: 2, dict: 3 } as const;
export type RomKind = keyof typeof ROM_KIND;

export interface TypeOptions {
  pressFrames?: number; // frames a key is held (default 4 — BASIC's scan needs it)
  gapFrames?: number; // frames between characters (default 4)
}

export type FrameCallback = (frameNo: number, rgba: Uint8Array, audio: Float32Array) => void;

export class Emulator {
  private m: WasmModule;
  readonly audioRate: number;
  /** Non-null while record_audio is capturing; run() appends drained samples. */
  private capture: number[] | null = null;
  /** While set, run() reports every frame: rgba is a VIEW into the WASM heap
   * (copy before keeping) plus that frame's drained audio. The spectator hub
   * sets this only while viewers are connected. */
  private onFrame: FrameCallback | null = null;

  private constructor(m: WasmModule, audioRate: number) {
    this.m = m;
    this.audioRate = audioRate;
  }

  static async create(wasmJsPath: string, audioRate = 44100): Promise<Emulator> {
    const require = createRequire(import.meta.url);
    const factory = require(wasmJsPath);
    const m: WasmModule = await factory();
    m._emu_init(audioRate);
    return new Emulator(m, audioRate);
  }

  // ---- time ---------------------------------------------------------------

  setOnFrame(cb: FrameCallback | null): void {
    this.onFrame = cb;
  }

  frames(): number {
    return this.m._emu_frames();
  }

  /** Advance N frames, draining the audio ring (into the capture buffer if
   * one is active, otherwise discarded — the ring grows unbounded if left). */
  run(frames: number): void {
    for (let i = 0; i < frames; i++) {
      this.m._emu_run_frame();
      const audio = this.drainAudio();
      if (this.onFrame) this.onFrame(this.frames(), this.renderFrame(), audio ?? new Float32Array(0));
    }
  }

  /** Drain the audio ring; collect this frame's samples when anyone (the
   * record() capture or the onFrame hook) wants them, else discard. */
  private drainAudio(): Float32Array | null {
    const want = this.capture !== null || this.onFrame !== null;
    const out: number[] | null = want ? [] : null;
    for (;;) {
      const n = this.m._emu_read_audio();
      if (n <= 0) break;
      if (out) {
        const base = this.m._emu_audio_buffer() >> 2;
        for (let i = 0; i < n; i++) out.push(this.m.HEAPF32[base + i]);
      }
      if (n < this.m._emu_audio_capacity()) break;
    }
    if (out && this.capture) for (const v of out) this.capture.push(v);
    return out ? Float32Array.from(out) : null;
  }

  /** Capture audio while body() advances time; returns the drained samples. */
  record(body: () => void): Float32Array {
    this.capture = [];
    try {
      body();
      return Float32Array.from(this.capture);
    } finally {
      this.capture = null;
    }
  }

  // ---- media / boot -------------------------------------------------------

  loadRom(kind: RomKind, path: string): void {
    const bytes = readFileSync(path);
    this.withBuffer(bytes, (ptr) => {
      if (!this.m._emu_set_rom(ROM_KIND[kind], ptr, bytes.length))
        throw new Error(`ROM load failed: ${kind} from ${path}`);
    });
  }

  insertDisk(drive: number, path: string): void {
    const bytes = readFileSync(path);
    this.withBuffer(bytes, (ptr) => {
      if (!this.m._emu_insert_disk(drive, ptr, bytes.length))
        throw new Error(`D88 parse failed: ${path}`);
    });
  }

  insertBlankDisk(drive: number): void {
    if (!this.m._emu_insert_blank_disk(drive)) throw new Error("blank insert failed");
  }

  diskImage(drive: number): Buffer {
    const size = this.m._emu_disk_snapshot(drive);
    const ptr = this.m._emu_disk_data();
    return Buffer.from(this.m.HEAPU8.subarray(ptr, ptr + size));
  }

  diskDirty(drive: number): boolean {
    return this.m._emu_disk_dirty(drive) !== 0;
  }

  hasIplRom(): boolean {
    return this.m._emu_has_ipl() !== 0;
  }

  bootRealIpl(): void {
    if (!this.m._emu_boot_real_ipl()) throw new Error("real-IPL boot failed (IPL ROM loaded?)");
  }

  bootDummyIpl(): void {
    if (!this.m._emu_boot()) throw new Error("dummy-IPL boot failed (disk in drive 0?)");
  }

  private withBuffer(bytes: Uint8Array, body: (ptr: number) => void): void {
    const ptr = this.m._malloc(bytes.length);
    try {
      this.m.HEAPU8.set(bytes, ptr);
      body(ptr);
    } finally {
      this.m._free(ptr);
    }
  }

  // ---- input --------------------------------------------------------------

  setKey(pos: KeyPos, down: boolean): void {
    this.m._emu_key(pos.row, pos.bit, down ? 1 : 0);
  }

  /** Type a string with the CLI --type cadence: each character held
   * pressFrames with its SHIFT, then gapFrames of silence. */
  typeText(text: string, opts: TypeOptions = {}): void {
    const press = opts.pressFrames ?? 4;
    const gap = opts.gapFrames ?? 4;
    for (const ch of text) {
      const key = keyForChar(ch);
      if (!key) throw new Error(`untypeable character: ${JSON.stringify(ch)}`);
      if (key.shift) this.setKey(SHIFT_POS, true);
      this.setKey(key.pos, true);
      this.run(press);
      this.setKey(key.pos, false);
      if (key.shift) this.setKey(SHIFT_POS, false);
      this.run(gap);
    }
  }

  pressKey(name: string, holdFrames = 4, mods: { shift?: boolean; ctrl?: boolean } = {}): void {
    const pos = keyFromName(name);
    if (!pos) throw new Error(`unknown key name: ${name}`);
    const held: KeyPos[] = [];
    if (mods.shift) held.push(SHIFT_POS);
    if (mods.ctrl) {
      const ctrl = keyFromName("ctrl")!;
      held.push(ctrl);
    }
    for (const h of held) this.setKey(h, true);
    this.setKey(pos, true);
    this.run(holdFrames);
    this.setKey(pos, false);
    for (const h of held) this.setKey(h, false);
    this.run(2);
  }

  joy(mask: number, holdFrames: number): void {
    this.m._emu_joy(mask);
    this.run(holdFrames);
    this.m._emu_joy(0);
  }

  // ---- observation --------------------------------------------------------

  screenText(): string {
    return this.m.UTF8ToString(this.m._emu_screen_text());
  }

  /** Run frames until `text` appears on the text screen. Checks every
   * `stride` frames. Returns the frame it was found at, or -1 on timeout. */
  waitForText(text: string, timeoutFrames: number, stride = 4): number {
    if (this.screenText().includes(text)) return this.frames();
    let waited = 0;
    while (waited < timeoutFrames) {
      const step = Math.min(stride, timeoutFrames - waited);
      this.run(step);
      waited += step;
      if (this.screenText().includes(text)) return this.frames();
    }
    return -1;
  }

  /** Run frames until the text screen stops changing for `stableFrames`
   * in a row (a boot disk's auto-run may still be typing after the prompt
   * first appears — keystrokes sent then race it and get eaten). */
  waitForStableScreen(stableFrames = 60, timeoutFrames = 1800): void {
    let last = this.screenText();
    let stable = 0;
    let waited = 0;
    while (stable < stableFrames && waited < timeoutFrames) {
      this.run(4);
      waited += 4;
      const now = this.screenText();
      if (now === last) stable += 4;
      else {
        stable = 0;
        last = now;
      }
    }
  }

  /** 640x400 RGBA of the current frame. */
  renderFrame(): Uint8Array {
    this.m._emu_render();
    const ptr = this.m._emu_frame_buffer();
    return this.m.HEAPU8.subarray(ptr, ptr + 640 * 400 * 4);
  }

  debug(): Record<string, unknown> {
    return JSON.parse(this.m.UTF8ToString(this.m._emu_debug_json()));
  }

  readMem(addr: number): number {
    return this.m._emu_read_mem(addr & 0xffff);
  }

  readPhys(addr: number): number {
    return this.m._emu_read_phys(addr);
  }

  poke(addr: number, value: number): void {
    this.m._emu_poke(addr & 0xffff, value & 0xff);
  }

  opnRegs(): Uint8Array {
    const ptr = this.m._emu_opn_regs();
    return this.m.HEAPU8.subarray(ptr, ptr + 256);
  }

  fmKeyon(ch: number): number {
    return this.m._emu_fm_keyon(ch);
  }

  beepOn(): boolean {
    return this.m._emu_beep() !== 0;
  }
}
