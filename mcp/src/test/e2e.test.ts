// End-to-end acceptance: a real MCP client drives the real server binary
// over stdio, exactly as an AI agent would.
//
// The BASIC demos need the user's own ROMs and BASIC disk:
//   MZ2500_ROM_DIR   directory holding ipl.rom / kanji.rom / dict.rom
//   MZ2500_DISK_A    the BASIC-M25 boot disk (.d88)
// They are skipped (not failed) when those are absent. The ROM-less smoke
// test only needs the bundled NEKO demo disk from tools/build_wasm.sh.
import { test } from "node:test";
import assert from "node:assert/strict";
import { existsSync, mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";

const here = dirname(fileURLToPath(import.meta.url));
const serverJs = resolve(here, "../index.js");
const webEmu = resolve(here, "../../..");

const romDir = process.env.MZ2500_ROM_DIR;
const diskA = process.env.MZ2500_DISK_A;
const formattedBlank = resolve(webEmu, "tests/fixtures/blank_2dd_formatted.d88");
const haveBasic = !!(
  romDir &&
  diskA &&
  existsSync(join(romDir, "ipl.rom")) &&
  existsSync(diskA) &&
  existsSync(formattedBlank)
);
const nekoDemo = ["web/dist/neko_can_run.d88", "build/neko_can_run_demo.d88"]
  .map((p) => resolve(webEmu, p))
  .find(existsSync);

interface ToolResult {
  content: Array<{ type: string; text?: string; data?: string; mimeType?: string }>;
  isError?: boolean;
}

async function connect(args: string[]): Promise<Client> {
  const client = new Client({ name: "e2e", version: "0.0.0" });
  await client.connect(
    new StdioClientTransport({
      command: process.execPath,
      args: [serverJs, ...args],
      env: { ...process.env } as Record<string, string>,
      stderr: "ignore",
    }),
  );
  return client;
}

function textOf(result: ToolResult): string {
  assert.ok(!result.isError, `tool error: ${JSON.stringify(result.content)}`);
  return result.content
    .filter((c) => c.type === "text")
    .map((c) => c.text)
    .join("\n");
}

async function call(client: Client, name: string, args: Record<string, unknown> = {}): Promise<ToolResult> {
  return (await client.callTool({ name, arguments: args })) as ToolResult;
}

function basicArgs(workdir: string, diskB: string = formattedBlank): string[] {
  return ["--rom-dir", romDir!, "--disk-a", diskA!, "--disk-b", diskB, "--workdir", workdir];
}

test("demo 1: type a BASIC program, RUN, verify its output", { skip: !haveBasic, timeout: 300000 }, async () => {
  const client = await connect(basicArgs(mkdtempSync(join(tmpdir(), "mz-"))));
  try {
    await call(client, "type_text", { text: "new\n" });
    await call(client, "type_text", { text: '10 for i=1 to 3\n20 print "HELLO";i\n30 next\n' });
    await call(client, "type_text", { text: "run\n" });
    const out = textOf(await call(client, "wait_for_text", { text: "HELLO 3", timeoutFrames: 600 }));
    assert.match(out, /found at frame/);
    assert.match(out, /HELLO 1/);
    assert.match(out, /HELLO 2/);
  } finally {
    await client.close();
  }
});

test("demo 2: LINE draws pixels visible in screenshots", { skip: !haveBasic, timeout: 300000 }, async () => {
  const client = await connect(basicArgs(mkdtempSync(join(tmpdir(), "mz-"))));
  try {
    const before = (await call(client, "screenshot")) as ToolResult;
    const pngBefore = before.content.find((c) => c.type === "image")?.data;
    assert.ok(pngBefore, "screenshot returns an image");
    const echo = textOf(await call(client, "type_text", { text: "line(0,0)-(300,180),6\n" }));
    assert.doesNotMatch(echo, /error/i);
    await call(client, "run_frames", { frames: 30 });
    const after = (await call(client, "screenshot")) as ToolResult;
    const pngAfter = after.content.find((c) => c.type === "image")?.data;
    assert.ok(pngAfter && pngAfter !== pngBefore, "the frame changed after LINE");
    const savedPath = textOf(after).match(/saved: (\S+\.png)/)?.[1];
    assert.ok(savedPath && existsSync(savedPath), "png written into the workdir");
  } finally {
    await client.close();
  }
});

test("demo 3: PLAY is observable as sound state and recorded audio", { skip: !haveBasic, timeout: 300000 }, async () => {
  const client = await connect(basicArgs(mkdtempSync(join(tmpdir(), "mz-"))));
  try {
    await call(client, "type_text", { text: 'play "o4a1a1a1a1"\n' });
    let sounding = false;
    for (let i = 0; i < 30 && !sounding; i++) {
      await call(client, "run_frames", { frames: 8 });
      const state = JSON.parse(textOf(await call(client, "read_sound_state")));
      sounding = state.fm.some((c: { keyOn: boolean }) => c.keyOn);
      if (sounding) {
        const a2 = state.fm.find((c: { keyOn: boolean }) => c.keyOn);
        assert.equal(a2.note, "A2"); // BASIC-M25's o4a — 110 Hz, verified by autocorrelation
      }
    }
    assert.ok(sounding, "an FM channel keys on");
    const rec = textOf(await call(client, "record_audio", { frames: 90 }));
    const peak = Number(rec.match(/peak: ([\d.]+)/)?.[1]);
    assert.ok(peak > 0.001, `audible peak (got ${peak})`);
    const wavPath = rec.match(/saved: (\S+\.wav)/)?.[1];
    assert.ok(wavPath && existsSync(wavPath), "wav written into the workdir");
  } finally {
    await client.close();
  }
});

test("persistence: SAVE -> export_disk -> reboot -> LOAD -> RUN", { skip: !haveBasic, timeout: 300000 }, async () => {
  const workdir = mkdtempSync(join(tmpdir(), "mz-"));
  const client = await connect(basicArgs(workdir));
  try {
    await call(client, "type_text", { text: '10 print "ROUNDTRIP"\nsave "FD2:TEST"\n' });
    await call(client, "run_frames", { frames: 600 });
    const exported = textOf(await call(client, "export_disk", { drive: 1 }));
    const d88 = exported.match(/saved: (\S+\.d88)/)?.[1];
    assert.ok(d88 && existsSync(d88), "d88 exported");

    const reset = textOf(await call(client, "reset", { waitFor: "Ok" }));
    assert.match(reset, /found/);
    await call(client, "insert_disk", { drive: 1, path: d88 });
    await call(client, "type_text", { text: 'load "FD2:TEST"\n' });
    await call(client, "run_frames", { frames: 600 });
    await call(client, "type_text", { text: "run\n" });
    const out = textOf(await call(client, "wait_for_text", { text: "ROUNDTRIP", timeoutFrames: 600 }));
    assert.match(out, /found at frame/);
  } finally {
    await client.close();
  }
});

test("ROM-less smoke: NEKO demo boots via dummy IPL and renders", { skip: !nekoDemo, timeout: 300000 }, async () => {
  const workdir = mkdtempSync(join(tmpdir(), "mz-"));
  const client = await connect(["--disk-a", nekoDemo!, "--workdir", workdir, "--no-auto-boot"]);
  try {
    textOf(await call(client, "reset", { mode: "dummy" }));
    await call(client, "run_frames", { frames: 1900 }); // through boot into the title
    const shot = (await call(client, "screenshot")) as ToolResult;
    assert.ok(shot.content.some((c) => c.type === "image"), "title screen renders");
    const state = JSON.parse(textOf(await call(client, "get_machine_state")));
    assert.ok((state.frames as number) >= 1900);
  } finally {
    await client.close();
  }
});
