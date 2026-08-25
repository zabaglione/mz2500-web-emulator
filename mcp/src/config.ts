// Server configuration: CLI flags first, MZ2500_* environment variables as
// fallback. ROMs and disks are the user's own files — never bundled.
import { existsSync } from "node:fs";
import { resolve, dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

export interface Config {
  wasmJs: string;
  iplRom?: string;
  kanjiRom?: string;
  dictRom?: string;
  sasiRom?: string;
  diskA?: string;
  diskB?: string; // path, or "blank"
  sasiHdd?: string;
  workdir: string;
  autoBoot: boolean;
  bootWait?: string; // text to wait for after boot; default "Ok" on real IPL
  bootTimeoutFrames: number;
  spectatePort: number; // 8425 default; 0 disables the spectator view
}

function arg(argv: string[], name: string): string | undefined {
  const i = argv.indexOf(name);
  return i >= 0 && i + 1 < argv.length ? argv[i + 1] : undefined;
}

export function parseConfig(argv: string[], env: NodeJS.ProcessEnv): Config {
  const here = dirname(fileURLToPath(import.meta.url));
  const romDir = arg(argv, "--rom-dir") ?? env.MZ2500_ROM_DIR;
  const fromRomDir = (file: string): string | undefined => {
    if (!romDir) return undefined;
    const p = join(romDir, file);
    return existsSync(p) ? p : undefined;
  };

  const wasmCandidates = [
    arg(argv, "--wasm") ?? env.MZ2500_WASM_JS,
    resolve(here, "../../build/wasm/mz2500w.js"),
    resolve(here, "../../web/dist/mz2500w.js"),
  ].filter((p): p is string => !!p);
  const wasmJs = wasmCandidates.find((p) => existsSync(p));
  if (!wasmJs)
    throw new Error(
      `WASM core not found (tried: ${wasmCandidates.join(", ")}). ` +
        `Build it with web_emulator/tools/build_wasm.sh or pass --wasm.`,
    );

  const diskB = arg(argv, "--disk-b") ?? env.MZ2500_DISK_B ?? "blank";

  const spectatePort = Number(arg(argv, "--spectate-port") ?? env.MZ2500_SPECTATE_PORT ?? 8425);
  if (!Number.isInteger(spectatePort) || spectatePort < 0 || spectatePort > 65535) {
    throw new Error(
      `--spectate-port (or MZ2500_SPECTATE_PORT) must be an integer in [0, 65535], got: ${spectatePort}`,
    );
  }

  return {
    wasmJs,
    iplRom: arg(argv, "--ipl-rom") ?? env.MZ2500_IPL_ROM ?? fromRomDir("ipl.rom"),
    kanjiRom: arg(argv, "--kanji-rom") ?? env.MZ2500_KANJI_ROM ?? fromRomDir("kanji.rom"),
    dictRom: arg(argv, "--dict-rom") ?? env.MZ2500_DICT_ROM ?? fromRomDir("dict.rom"),
    // Opt-in only: an option ROM changes the real-IPL boot order (the ROM
    // boots before the floppy), so it must never ride in via --rom-dir.
    sasiRom: arg(argv, "--sasi-rom") ?? env.MZ2500_SASI_ROM,
    diskA: arg(argv, "--disk-a") ?? env.MZ2500_DISK_A,
    diskB,
    sasiHdd: arg(argv, "--sasi-hdd") ?? env.MZ2500_SASI_HDD,
    workdir: resolve(arg(argv, "--workdir") ?? env.MZ2500_WORKDIR ?? "mz2500-work"),
    autoBoot: !argv.includes("--no-auto-boot"),
    bootWait: arg(argv, "--boot-wait") ?? env.MZ2500_BOOT_WAIT,
    bootTimeoutFrames: Number(arg(argv, "--boot-timeout-frames") ?? env.MZ2500_BOOT_TIMEOUT ?? 6000),
    spectatePort,
  };
}
