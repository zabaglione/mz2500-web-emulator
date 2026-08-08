#!/usr/bin/env node
// mz2500-mcp: MCP server for the MZ-2500 web emulator core.
//
//   node dist/index.js --rom-dir ~/roms/mz2500 --disk-a "basic-m25.d88" \
//     --workdir ./mz2500-work
//
// All logging goes to stderr; stdout carries the MCP stdio transport.
import { basename } from "node:path";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { parseConfig } from "./config.js";
import { Session } from "./session.js";
import { buildServer } from "./server.js";
import { SpectatorHub } from "./spectator/hub.js";
import { decodeSoundState } from "./sound.js";

async function main(): Promise<void> {
  const config = parseConfig(process.argv.slice(2), process.env);
  console.error(`[mz2500-mcp] wasm: ${config.wasmJs}`);
  console.error(`[mz2500-mcp] workdir: ${config.workdir}`);
  const session = await Session.start(config);
  console.error(
    `[mz2500-mcp] booted to frame ${session.emu.frames()} ` +
      `(ipl=${session.emu.hasIplRom()}, diskA=${config.diskA ?? "none"}, diskB=${config.diskB})`,
  );

  let spectatorUrl: string | null = null;
  if (config.spectatePort !== 0) {
    const hub = new SpectatorHub({
      port: config.spectatePort,
      audioRate: session.emu.audioRate,
      infoFn: () => ({
        frameNo: session.emu.frames(),
        disk: config.diskA ? basename(config.diskA) : null,
        workdir: config.workdir,
      }),
      stateFn: () =>
        decodeSoundState(
          Uint8Array.from(session.emu.opnRegs()),
          (ch) => session.emu.fmKeyon(ch),
          session.emu.beepOn(),
        ),
      snapshotFn: () => ({ frameNo: session.emu.frames(), rgba: session.emu.renderFrame() }),
    });
    if ((await hub.start()) !== null) {
      hub.attach(session.emu);
      spectatorUrl = hub.url();
      console.error(`[mz2500-mcp] spectator view: ${spectatorUrl}`);
    }
  }

  const server = buildServer(session, spectatorUrl);
  await server.connect(new StdioServerTransport());
  console.error("[mz2500-mcp] ready on stdio");
}

main().catch((err) => {
  console.error("[mz2500-mcp] fatal:", err);
  process.exit(1);
});
