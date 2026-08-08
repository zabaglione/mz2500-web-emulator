// Spectator view: protocol framing, hub streaming, and (ROM-gated) a real
// BASIC PLAY session observed over HTTP.
import { test } from "node:test";
import assert from "node:assert/strict";
import { existsSync, mkdtempSync } from "node:fs";
import { resolve, dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { tmpdir } from "node:os";
import { request } from "node:http";
import {
  MSG,
  encodeHeader,
  encodeMessage,
  withFrameNo,
  floatTo16,
  MessageReader,
} from "../spectator/protocol.js";
import { encodePng } from "../png.js";
import { Emulator } from "../emulator.js";
import { VIEWER_HTML } from "../spectator/viewer.js";
import { SpectatorHub } from "../spectator/hub.js";
import { parseConfig } from "../config.js";
import { Session } from "../session.js";

test("protocol: header + all message kinds round-trip through MessageReader", () => {
  const reader = new MessageReader();
  const stream = Buffer.concat([
    encodeHeader({ version: 1, width: 640, height: 400, audioRate: 44100 }),
    encodeMessage(MSG.heartbeat),
    encodeMessage(MSG.video, withFrameNo(7, Buffer.from([0x89, 0x50]))),
    encodeMessage(MSG.repeat, withFrameNo(8)),
    encodeMessage(MSG.audio, withFrameNo(9, floatTo16(Float32Array.from([0, 0.5, -0.5, 2])))),
    encodeMessage(MSG.state, withFrameNo(10, Buffer.from('{"a":1}', "utf8"))),
  ]);
  // Feed byte-by-byte to prove incremental parsing.
  for (let i = 0; i < stream.length; i++) {
    reader.feed(stream.subarray(i, i + 1));
  }
  assert.deepEqual(reader.header, { version: 1, width: 640, height: 400, audioRate: 44100 });

  const m1 = reader.next()!;
  assert.equal(m1.type, MSG.heartbeat);
  assert.equal(m1.frameNo, undefined);

  const m2 = reader.next()!;
  assert.equal(m2.type, MSG.video);
  assert.equal(m2.frameNo, 7);
  assert.deepEqual([...m2.body], [0x89, 0x50]);

  const m3 = reader.next()!;
  assert.equal(m3.type, MSG.repeat);
  assert.equal(m3.frameNo, 8);
  assert.equal(m3.body.length, 0);

  const m4 = reader.next()!;
  assert.equal(m4.type, MSG.audio);
  assert.equal(m4.frameNo, 9);
  assert.equal(m4.body.readInt16LE(0), 0);
  assert.equal(m4.body.readInt16LE(2), Math.round(0.5 * 32767));
  assert.equal(m4.body.readInt16LE(4), Math.round(-0.5 * 32767));
  assert.equal(m4.body.readInt16LE(6), 32767); // clipped

  const m5 = reader.next()!;
  assert.equal(m5.type, MSG.state);
  assert.equal(m5.body.toString("utf8"), '{"a":1}');

  assert.equal(reader.next(), null); // exhausted
});

test("png: deflate level is selectable and output stays a valid PNG", () => {
  const rgba = new Uint8Array(4 * 4 * 4);
  for (let i = 0; i < rgba.length; i += 4) {
    rgba[i] = i & 0xff;
    rgba[i + 3] = 0xff;
  }
  const def = encodePng(rgba, 4, 4);
  const fast = encodePng(rgba, 4, 4, 1);
  for (const png of [def, fast]) {
    assert.deepEqual([...png.subarray(0, 8)], [0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
    assert.equal(png.subarray(12, 16).toString("ascii"), "IHDR");
  }
});

const here = dirname(fileURLToPath(import.meta.url));
const wasmJs = [resolve(here, "../../../build/wasm/mz2500w.js"), resolve(here, "../../../web/dist/mz2500w.js")].find(existsSync);

test("emulator: onFrame fires per frame and coexists with record()", { skip: !wasmJs }, async () => {
  const emu = await Emulator.create(wasmJs!);
  const seen: Array<{ frameNo: number; rgbaLen: number; audioLen: number }> = [];
  emu.setOnFrame((frameNo, rgba, audio) =>
    seen.push({ frameNo, rgbaLen: rgba.length, audioLen: audio.length }),
  );
  const recorded = emu.record(() => emu.run(3));
  emu.setOnFrame(null);
  emu.run(2); // must not fire

  assert.equal(seen.length, 3);
  assert.ok(seen[1].frameNo === seen[0].frameNo + 1);
  assert.equal(seen[0].rgbaLen, 640 * 400 * 4);
  const totalAudio = seen.reduce((a, s) => a + s.audioLen, 0);
  assert.ok(totalAudio > 0, "audio drained per frame");
  assert.equal(recorded.length, totalAudio, "record() still captures the same samples");
});

test("viewer: single self-contained page", () => {
  assert.match(VIEWER_HTML, /<canvas id="screen" width="640" height="400">/);
  assert.match(VIEWER_HTML, /fetch\("\/stream"\)/);
  assert.ok(!VIEWER_HTML.includes("http://"), "no external references");
});

function testRgba(seed: number): Uint8Array {
  const rgba = new Uint8Array(640 * 400 * 4);
  rgba.fill(seed & 0xff);
  return rgba;
}

async function readMessages(
  body: ReadableStream<Uint8Array>,
  count: number,
): Promise<Array<{ type: number; frameNo?: number; body: Buffer }>> {
  const reader = new MessageReader();
  const out: Array<{ type: number; frameNo?: number; body: Buffer }> = [];
  const r = body.getReader();
  while (out.length < count) {
    const { done, value } = await r.read();
    if (done) break;
    reader.feed(Buffer.from(value));
    for (let m = reader.next(); m; m = reader.next()) {
      if (m.type !== MSG.heartbeat) out.push(m); // heartbeats are timer-driven — excluding them keeps counts deterministic
    }
  }
  await r.cancel();
  return out;
}

test("hub: serves the viewer, streams frames, dedupes video and state", async () => {
  let stateTick = 0;
  const hub = new SpectatorHub({
    port: 0,
    audioRate: 44100,
    stateFn: () => ({ tick: stateTick }),
    log: () => {},
  });
  const port = await hub.start();
  assert.ok(port !== null && port > 0);

  const html = await fetch(`http://127.0.0.1:${port}/`).then((r) => r.text());
  assert.match(html, /観戦開始/);

  let hooked: unknown = null;
  hub.attach({ setOnFrame: (cb) => (hooked = cb) });
  assert.equal(hooked, null, "no viewers yet — hook must stay off");

  const res = await fetch(`http://127.0.0.1:${port}/stream`);
  assert.equal(res.status, 200);
  // The hook attaches when the stream request lands.
  for (let i = 0; i < 50 && hooked === null; i++) await new Promise((r) => setTimeout(r, 10));
  assert.ok(hooked !== null, "hook set once a viewer connected");

  const audio = Float32Array.from([0.1, -0.1]);
  hub.push(100, testRgba(1), audio); // video + audio + state(tick:0)
  hub.push(101, testRgba(1), audio); // repeat + audio (state unchanged)
  stateTick = 1;
  hub.push(102, testRgba(2), audio); // video + audio + state(tick:1)

  const msgs = await readMessages(res.body!, 8);
  const kinds = msgs.map((m) => m.type);
  assert.deepEqual(kinds, [
    MSG.video, MSG.audio, MSG.state,
    MSG.repeat, MSG.audio,
    MSG.video, MSG.audio, MSG.state,
  ]);
  assert.equal(msgs[0].frameNo, 100);
  assert.deepEqual([...msgs[0].body.subarray(0, 4)], [0x89, 0x50, 0x4e, 0x47]);
  assert.equal(msgs[3].frameNo, 101);
  assert.equal(msgs[4].body.readInt16LE(0), Math.round(0.1 * 32767));
  assert.equal(JSON.parse(msgs[7].body.toString("utf8")).tick, 1);

  // Viewer went away (readMessages cancelled) — hook must come off.
  for (let i = 0; i < 100 && hooked !== null; i++) await new Promise((r) => setTimeout(r, 10));
  assert.equal(hooked, null, "hook removed when the last viewer left");
  hub.close();
});

test("hub: rejects cross-origin requests", async () => {
  const hub = new SpectatorHub({ port: 0, audioRate: 44100, stateFn: () => ({}), log: () => {} });
  const port = await hub.start();
  const res = await fetch(`http://127.0.0.1:${port}/stream`, {
    headers: { origin: "http://evil.example" },
  });
  assert.equal(res.status, 403);
  const ok = await fetch(`http://127.0.0.1:${port}/`, {
    headers: { origin: `http://127.0.0.1:${port}` },
  });
  assert.equal(ok.status, 200);
  hub.close();
});

test("hub: late joiner gets a fresh keyframe even when pixels are unchanged", async () => {
  const hub = new SpectatorHub({ port: 0, audioRate: 44100, stateFn: () => ({}), log: () => {} });
  const port = await hub.start();

  const resA = await fetch(`http://127.0.0.1:${port}/stream`);
  const rgba = testRgba(9);
  const audio = Float32Array.from([0.1]);
  hub.push(1, rgba, audio); // full frame for A; sets prevFrame

  const resB = await fetch(`http://127.0.0.1:${port}/stream`);
  hub.push(2, rgba, audio); // same pixels — would normally dedupe to `repeat`

  const msgsB = await readMessages(resB.body!, 1);
  assert.equal(msgsB[0].type, MSG.video, "late joiner must get a full frame, not a repeat");

  hub.close();
});

test("hub: rejects requests with a mismatched Host header (DNS-rebinding defense)", async () => {
  const hub = new SpectatorHub({ port: 0, audioRate: 44100, stateFn: () => ({}), log: () => {} });
  const port = await hub.start();

  const status = await new Promise<number>((res, rej) => {
    const req = request(
      { host: "127.0.0.1", port: port!, path: "/stream", headers: { host: "evil.example:8425" } },
      (r) => {
        r.resume();
        res(r.statusCode!);
      },
    );
    req.on("error", rej);
    req.end();
  });
  assert.equal(status, 403);
  hub.close();
});

test("config: spectate port flag, env, default and disable", () => {
  const base = ["--wasm", wasmJs ?? "/nonexistent"];
  if (!wasmJs) return; // parseConfig requires a wasm path that exists
  assert.equal(parseConfig(base, {}).spectatePort, 8425);
  assert.equal(parseConfig([...base, "--spectate-port", "9000"], {}).spectatePort, 9000);
  assert.equal(parseConfig(base, { MZ2500_SPECTATE_PORT: "7000" }).spectatePort, 7000);
  assert.equal(parseConfig([...base, "--spectate-port", "0"], {}).spectatePort, 0);
});

const romDir = process.env.MZ2500_ROM_DIR;
const diskA = process.env.MZ2500_DISK_A;
const haveBasic = !!(romDir && diskA && existsSync(join(romDir, "ipl.rom")) && existsSync(diskA));

test("spectator e2e: doremi PLAY is observable over the stream", { skip: !haveBasic }, async () => {
  const session = await Session.start(
    parseConfig(
      ["--rom-dir", romDir!, "--disk-a", diskA!, "--workdir", mkdtempSync(join(tmpdir(), "spect-"))],
      {},
    ),
  );
  const hub = new SpectatorHub({
    port: 0,
    audioRate: session.emu.audioRate,
    stateFn: () => ({ fm1keyon: session.emu.fmKeyon(0) !== 0 }),
    log: () => {},
  });
  const port = await hub.start();
  hub.attach(session.emu);
  const res = await fetch(`http://127.0.0.1:${port}/stream`);
  // Wait for the hook (set when the stream request is handled).
  await new Promise((r) => setTimeout(r, 100));

  session.emu.typeText('play"O5L16CDE"\n');
  session.emu.run(200);

  const msgs = await readMessages(res.body!, 300);
  const audio = msgs.filter((m) => m.type === MSG.audio);
  // Video messages may be backpressure-skipped during the synchronous burst; audio is never skipped.
  assert.ok(audio.length >= 140, `audio frames (got ${audio.length})`);
  let peak = 0;
  for (const a of audio) {
    for (let i = 0; i < a.body.length; i += 2) peak = Math.max(peak, Math.abs(a.body.readInt16LE(i)));
  }
  assert.ok(peak > 500, `audible playback over the stream (peak ${peak})`);
  const states = msgs.filter((m) => m.type === MSG.state).map((m) => JSON.parse(m.body.toString("utf8")));
  assert.ok(states.some((s) => s.fm1keyon === true), "FM keyon visible in state messages");
  const video = msgs.filter((m) => m.type === MSG.video || m.type === MSG.repeat);
  assert.ok(video.length >= 30, "video/repeat messages present (backpressure may skip some)");
  hub.close();
});
