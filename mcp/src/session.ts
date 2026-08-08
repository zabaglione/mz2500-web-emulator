// One emulator instance plus the working directory the tools write into.
// Owns boot orchestration so a connecting client finds BASIC already at its
// Ok prompt when the server was configured with an IPL ROM and a boot disk.
import { mkdirSync, writeFileSync } from "node:fs";
import { join, resolve } from "node:path";
import { Emulator } from "./emulator.js";
import type { Config } from "./config.js";

export type BootMode = "real" | "dummy";

export class Session {
  readonly emu: Emulator;
  readonly config: Config;
  private counters = new Map<string, number>();

  private constructor(emu: Emulator, config: Config) {
    this.emu = emu;
    this.config = config;
  }

  static async start(config: Config): Promise<Session> {
    mkdirSync(config.workdir, { recursive: true });
    const emu = await Emulator.create(config.wasmJs);
    const session = new Session(emu, config);

    if (config.iplRom) emu.loadRom("ipl", config.iplRom);
    if (config.cgRom) emu.loadRom("cg", config.cgRom);
    if (config.kanjiRom) emu.loadRom("kanji", config.kanjiRom);
    if (config.dictRom) emu.loadRom("dict", config.dictRom);
    if (config.diskA) emu.insertDisk(0, config.diskA);
    if (config.diskB === "blank") emu.insertBlankDisk(1);
    else if (config.diskB) emu.insertDisk(1, config.diskB);

    if (config.autoBoot && (config.diskA || emu.hasIplRom())) {
      const mode: BootMode = emu.hasIplRom() ? "real" : "dummy";
      session.boot(mode, config.bootWait ?? (mode === "real" ? "Ok" : undefined));
    }
    return session;
  }

  /** (Re)boot the machine. Waits for `waitFor` text when given; returns the
   * frame it appeared at, -1 on timeout, or the current frame with no wait. */
  boot(mode: BootMode, waitFor?: string, timeoutFrames?: number): number {
    if (mode === "real") this.emu.bootRealIpl();
    else this.emu.bootDummyIpl();
    if (waitFor && waitFor !== "none") {
      const found = this.emu.waitForText(waitFor, timeoutFrames ?? this.config.bootTimeoutFrames);
      // A boot disk's auto-run may still be loading/typing when the prompt
      // first appears (screen quiet during disk I/O, keystrokes eaten), so
      // "the text arrived" is not "the interpreter listens".
      this.emu.waitForStableScreen();
      if (waitFor === "Ok") this.probeBasicPrompt();
      return found;
    }
    return this.emu.frames();
  }

  /** Prove BASIC is interactive: type REM until a fresh Ok line answers.
   * A half-eaten REM still answers (syntax error, then Ok), so any new Ok
   * means the keyboard is being read again. */
  private probeBasicPrompt(): void {
    const countOk = () => (this.emu.screenText().match(/^Ok/gm) ?? []).length;
    for (let attempt = 0; attempt < 5; attempt++) {
      const before = countOk();
      this.emu.typeText("rem\n");
      let waited = 0;
      while (waited < 240 && countOk() <= before) {
        this.emu.run(8);
        waited += 8;
      }
      if (countOk() > before) {
        this.emu.waitForStableScreen(30, 300);
        return;
      }
    }
  }

  /** Next path for a numbered artefact in the workdir, e.g. screenshot-0001.png */
  artefactPath(stem: string, ext: string): string {
    const n = (this.counters.get(stem) ?? 0) + 1;
    this.counters.set(stem, n);
    return join(this.config.workdir, `${stem}-${String(n).padStart(4, "0")}.${ext}`);
  }

  saveArtefact(stem: string, ext: string, data: Buffer): string {
    const path = this.artefactPath(stem, ext);
    writeFileSync(path, data);
    return resolve(path);
  }
}
