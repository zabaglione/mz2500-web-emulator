// MCP tool surface over a Session. Tool granularity follows the grilled
// design (2026-08-08): high-level typing plus named keys and the pad; text
// screen reads next to pixel screenshots; sound as decoded state plus WAV;
// memory read wide (CPU + physical), write as poke only — no raw I/O ports.
import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";
import { encodePng } from "./png.js";
import { encodeWav } from "./wav.js";
import { decodeSoundState } from "./sound.js";
import type { Session } from "./session.js";

function text(s: string) {
  return { content: [{ type: "text" as const, text: s }] };
}

/** Accept 0xF000 / F000h / plain decimal for addresses. */
function parseAddress(v: string | number): number {
  if (typeof v === "number") return v;
  const s = v.trim().toLowerCase();
  if (s.startsWith("0x")) return parseInt(s.slice(2), 16);
  if (s.endsWith("h")) return parseInt(s.slice(0, -1), 16);
  if (/^[0-9]+$/.test(s)) return parseInt(s, 10);
  if (/^[0-9a-f]+$/.test(s)) return parseInt(s, 16);
  throw new Error(`unparseable address: ${v}`);
}

const address = z.union([z.number().int(), z.string()]);

export function buildServer(session: Session, spectatorUrl?: string | null): McpServer {
  const { emu } = session;
  const server = new McpServer(
    { name: "mz2500", version: "0.1.0" },
    spectatorUrl
      ? {
          instructions:
            `A human-facing spectator view of this machine (screen, sound and ` +
            `YM2203 state, replayed at real speed) is streaming at ${spectatorUrl} — ` +
            `tell the user to open that URL in a browser when they want to watch.`,
        }
      : undefined,
  );

  // ---- input --------------------------------------------------------------

  server.registerTool(
    "type_text",
    {
      title: "Type text on the MZ-2500 keyboard",
      description:
        "Type a string on the keyboard (ASCII + newline; SHIFT is handled automatically). " +
        "Use \\n for the CR key — a BASIC line is only entered once CR is typed. " +
        "Returns the text screen afterwards so the echo is visible.",
      inputSchema: {
        text: z.string().describe("Text to type. \\n presses CR."),
        pressFrames: z.number().int().min(1).max(60).optional()
          .describe("Frames each key is held (default 4)"),
        gapFrames: z.number().int().min(1).max(60).optional()
          .describe("Frames between keys (default 4)"),
      },
    },
    async ({ text: t, pressFrames, gapFrames }) => {
      emu.typeText(t, { pressFrames, gapFrames });
      emu.run(8); // let the echo land
      return text(emu.screenText());
    },
  );

  server.registerTool(
    "press_key",
    {
      title: "Press a named key",
      description:
        "Press one named key: cr, esc, break, tab, bs, del, home, help, up/down/left/right, " +
        "space, f1-f10, kp0-kp9, graph, kana, ctrl, shift, a-z, 0-9 … " +
        "Optional shift/ctrl modifiers and hold duration (e.g. BREAK to stop a running program).",
      inputSchema: {
        key: z.string().describe("Key name (see description)"),
        holdFrames: z.number().int().min(1).max(600).optional().describe("Frames held (default 4)"),
        shift: z.boolean().optional(),
        ctrl: z.boolean().optional(),
      },
    },
    async ({ key, holdFrames, shift, ctrl }) => {
      emu.pressKey(key, holdFrames ?? 4, { shift, ctrl });
      return text(emu.screenText());
    },
  );

  server.registerTool(
    "joy",
    {
      title: "Hold the joystick",
      description:
        "Hold a joystick state for N frames then release. Bits: 1=up 2=down 4=left 8=right " +
        "16=trigger2 32=trigger1 (what a trigger does is the software's choice).",
      inputSchema: {
        mask: z.number().int().min(0).max(63).describe("Bit mask of held directions/triggers"),
        holdFrames: z.number().int().min(1).max(3600).describe("Frames to hold"),
      },
    },
    async ({ mask, holdFrames }) => {
      emu.joy(mask, holdFrames);
      return text(`held mask ${mask} for ${holdFrames} frames; now at frame ${emu.frames()}`);
    },
  );

  // ---- time ---------------------------------------------------------------

  server.registerTool(
    "run_frames",
    {
      title: "Run emulation frames",
      description:
        "Advance the machine N frames (~55.5 frames/second of machine time). The machine is " +
        "frozen between tool calls; this is how time passes.",
      inputSchema: { frames: z.number().int().min(1).max(60000) },
    },
    async ({ frames }) => {
      emu.run(frames);
      return text(`now at frame ${emu.frames()}`);
    },
  );

  // ---- screen -------------------------------------------------------------

  server.registerTool(
    "read_screen",
    {
      title: "Read the text screen",
      description:
        "Decode the text VRAM to a string (40/80 cols x 20/25 rows). Kanji cells appear as 〓, " +
        "PCG art cells as #. The cheap, precise way to read BASIC's output.",
      inputSchema: {},
    },
    async () => text(emu.screenText()),
  );

  server.registerTool(
    "wait_for_text",
    {
      title: "Wait for text to appear",
      description:
        "Run frames until a string appears anywhere on the text screen (e.g. 'Ok' after RUN " +
        "finishes, or an expected PRINT result). Returns the screen either way.",
      inputSchema: {
        text: z.string().describe("Substring to wait for"),
        timeoutFrames: z.number().int().min(1).max(120000).optional()
          .describe("Give up after this many frames (default 3000)"),
      },
    },
    async ({ text: t, timeoutFrames }) => {
      const found = emu.waitForText(t, timeoutFrames ?? 3000);
      const screen = emu.screenText();
      return text(
        (found >= 0 ? `found at frame ${found}` : `TIMEOUT after ${timeoutFrames ?? 3000} frames`) +
          `\n---\n${screen}`,
      );
    },
  );

  server.registerTool(
    "screenshot",
    {
      title: "Screenshot the display",
      description:
        "Render the current frame (640x400, text + graphics layers) to PNG. Saved into the " +
        "workdir and returned inline. Use for verifying graphics; prefer read_screen for text.",
      inputSchema: {},
    },
    async () => {
      const png = encodePng(emu.renderFrame(), 640, 400);
      const path = session.saveArtefact("screenshot", "png", png);
      return {
        content: [
          { type: "image" as const, data: png.toString("base64"), mimeType: "image/png" },
          { type: "text" as const, text: `saved: ${path} (frame ${emu.frames()})` },
        ],
      };
    },
  );

  // ---- sound --------------------------------------------------------------

  server.registerTool(
    "read_sound_state",
    {
      title: "Read the sound chip state",
      description:
        "Decode the YM2203 registers into what is sounding now: FM channels (key-on, frequency, " +
        "note name, algorithm), SSG channels (tone/noise, frequency, note, volume, envelope) and " +
        "the BEEP line. Verifies PLAY statements without listening.",
      inputSchema: {},
    },
    async () => {
      const state = decodeSoundState(Uint8Array.from(emu.opnRegs()), (ch) => emu.fmKeyon(ch), emu.beepOn());
      return text(JSON.stringify(state, null, 2));
    },
  );

  server.registerTool(
    "record_audio",
    {
      title: "Record audio to WAV",
      description:
        "Run N frames while capturing the sound output (YM2203 + BEEP) to a mono 16-bit WAV in " +
        "the workdir. Returns the path plus peak/RMS so silence is detectable without listening.",
      inputSchema: {
        frames: z.number().int().min(1).max(60000).describe("Frames to record (~55.5/s)"),
      },
    },
    async ({ frames }) => {
      const samples = emu.record(() => emu.run(frames));
      let peak = 0;
      let sumSq = 0;
      for (const s of samples) {
        const a = Math.abs(s);
        if (a > peak) peak = a;
        sumSq += s * s;
      }
      const rms = samples.length ? Math.sqrt(sumSq / samples.length) : 0;
      const path = session.saveArtefact("audio", "wav", encodeWav(samples, emu.audioRate));
      return text(
        `saved: ${path}\nsamples: ${samples.length} (${(samples.length / emu.audioRate).toFixed(2)}s)` +
          `\npeak: ${peak.toFixed(4)}  rms: ${rms.toFixed(5)}` +
          (peak < 0.001 ? "\nNOTE: essentially silent" : ""),
      );
    },
  );

  // ---- machine internals --------------------------------------------------

  server.registerTool(
    "read_memory",
    {
      title: "Read memory",
      description:
        "Hex dump of memory. space='cpu' reads the Z80's 64KB view through the bank map; " +
        "space='physical' reads the flat 512KB space (bank*0x2000+offset) ignoring the map. " +
        "Addresses accept 0xF000 / F000h / decimal.",
      inputSchema: {
        address: address,
        length: z.number().int().min(1).max(4096).optional().describe("Bytes (default 64)"),
        space: z.enum(["cpu", "physical"]).optional().describe("Default cpu"),
      },
    },
    async ({ address: a, length, space }) => {
      const start = parseAddress(a);
      const len = length ?? 64;
      const phys = space === "physical";
      const lines: string[] = [];
      for (let base = start; base < start + len; base += 16) {
        const n = Math.min(16, start + len - base);
        const bytes: number[] = [];
        for (let i = 0; i < n; i++) bytes.push(phys ? emu.readPhys(base + i) : emu.readMem(base + i));
        const hex = bytes.map((b) => b.toString(16).padStart(2, "0")).join(" ");
        const ascii = bytes.map((b) => (b >= 0x20 && b < 0x7f ? String.fromCharCode(b) : ".")).join("");
        lines.push(`${base.toString(16).padStart(phys ? 5 : 4, "0")}: ${hex.padEnd(47)} ${ascii}`);
      }
      return text(lines.join("\n"));
    },
  );

  server.registerTool(
    "write_memory",
    {
      title: "Write memory (poke)",
      description:
        "Write bytes into the CPU address space through the bank map — the same semantics as " +
        "BASIC's POKE. bytes is hex like '3E 12 C9' or 'AA55'.",
      inputSchema: {
        address: address,
        bytes: z.string().describe("Hex byte string, spaces optional"),
      },
    },
    async ({ address: a, bytes }) => {
      const start = parseAddress(a);
      const clean = bytes.replace(/[\s,]/g, "");
      if (!/^([0-9a-fA-F]{2})+$/.test(clean)) throw new Error("bytes must be hex pairs");
      const data: number[] = [];
      for (let i = 0; i < clean.length; i += 2) data.push(parseInt(clean.slice(i, i + 2), 16));
      data.forEach((b, i) => emu.poke(start + i, b));
      return text(`wrote ${data.length} byte(s) at ${start.toString(16)}h`);
    },
  );

  server.registerTool(
    "get_machine_state",
    {
      title: "Machine state snapshot",
      description:
        "CPU registers, bank map, FDC, display mode, interrupt controller, BEEP line and the " +
        "current frame counter as JSON.",
      inputSchema: {},
    },
    async () => {
      const state = { ...emu.debug(), beep: emu.beepOn(), disk_dirty: [emu.diskDirty(0), emu.diskDirty(1)] };
      return text(JSON.stringify(state, null, 2));
    },
  );

  // ---- media / lifecycle --------------------------------------------------

  server.registerTool(
    "insert_disk",
    {
      title: "Insert a floppy disk",
      description:
        "Hot-swap a D88 image (or a blank unformatted disk) into drive 0 (FD1) or 1 (FD2). " +
        "No reset — mid-session swaps work like on the real machine.",
      inputSchema: {
        drive: z.number().int().min(0).max(1),
        path: z.string().optional().describe("Host path to a .d88 (omit with blank=true)"),
        blank: z.boolean().optional().describe("Insert an unformatted blank disk"),
      },
    },
    async ({ drive, path, blank }) => {
      if (blank) emu.insertBlankDisk(drive);
      else if (path) emu.insertDisk(drive, path);
      else throw new Error("give path or blank=true");
      return text(`drive ${drive}: ${blank ? "blank disk" : path} inserted`);
    },
  );

  server.registerTool(
    "export_disk",
    {
      title: "Export a drive's disk image",
      description:
        "Write the current contents of a drive out as a .d88 into the workdir (SAVE \"FD2:NAME\" " +
        "in BASIC writes to drive 1, then export drive 1 to persist it on the host).",
      inputSchema: {
        drive: z.number().int().min(0).max(1),
      },
    },
    async ({ drive }) => {
      const image = emu.diskImage(drive);
      const path = session.saveArtefact(`disk${drive}`, "d88", image);
      return text(`saved: ${path} (${image.length} bytes, dirty=${emu.diskDirty(drive)})`);
    },
  );

  server.registerTool(
    "insert_hdd",
    {
      title: "Insert a SASI hard-disk image",
      description:
        "Mount a raw SASI image (MZ-1E30 target). blockSize 0 = auto with the EH-SASI " +
        "signature tie-break; pass 256 for canonical CP/M images. Booting from it needs " +
        "the real IPL plus the SASI option ROM (sasirom.bin) and a reset.",
      inputSchema: {
        path: z.string().describe("Host path to a raw hard-disk image"),
        blockSize: z.number().int().optional().describe("0(auto)/256/512/1024, default 256"),
      },
    },
    async ({ path, blockSize }) => {
      emu.insertHdd(path, blockSize ?? 256);
      return text(`sasi: ${path} mounted (block=${emu.hddBlockSize()})`);
    },
  );

  server.registerTool(
    "export_hdd",
    {
      title: "Export the SASI hard-disk image",
      description: "Write the current SASI image into the workdir (persists CP/M C:/D: writes).",
      inputSchema: {},
    },
    async () => {
      if (!emu.hddLoaded()) throw new Error("no SASI image mounted");
      const image = emu.hddImage();
      const path = session.saveArtefact("sasi", "hdd", image);
      return text(`saved: ${path} (${image.length} bytes, dirty=${emu.hddDirty()})`);
    },
  );

  server.registerTool(
    "reset",
    {
      title: "Reboot the machine",
      description:
        "Cold boot again: mode 'real' boots through the IPL ROM (needs --ipl-rom), 'dummy' uses " +
        "the native IPLPRO boot from the disk in drive 0. Optionally waits for text (e.g. 'Ok').",
      inputSchema: {
        mode: z.enum(["real", "dummy"]).optional().describe("Default: real if an IPL ROM is loaded"),
        waitFor: z.string().optional().describe("Wait for this text after boot ('none' to skip)"),
        timeoutFrames: z.number().int().min(1).max(120000).optional(),
      },
    },
    async ({ mode, waitFor, timeoutFrames }) => {
      const m = mode ?? (emu.hasIplRom() ? "real" : "dummy");
      const frame = session.boot(m, waitFor, timeoutFrames);
      return text(
        `${m} boot done, frame ${emu.frames()}` +
          (waitFor && waitFor !== "none" ? ` (waited for ${JSON.stringify(waitFor)}: ${frame >= 0 ? "found" : "TIMEOUT"})` : "") +
          `\n---\n${emu.screenText()}`,
      );
    },
  );

  return server;
}
