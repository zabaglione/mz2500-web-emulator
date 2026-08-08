// Spectator hub: a 127.0.0.1 HTTP server broadcasting the machine's frames,
// audio and decoded sound state to any number of browser viewers. The
// emulator's onFrame hook is attached only while viewers are connected, so
// with nobody watching the tool-driven model is byte-for-byte unchanged.
// The emulator must never block on a slow viewer: laggards lose video
// first, then get disconnected outright.
import { createServer, type IncomingMessage, type Server, type ServerResponse } from "node:http";
import type { AddressInfo } from "node:net";
import { encodePng } from "../png.js";
import type { FrameCallback } from "../emulator.js";
import { MSG, encodeHeader, encodeMessage, floatTo16, withFrameNo } from "./protocol.js";
import { VIEWER_HTML } from "./viewer.js";

export interface FrameSink {
  setOnFrame(cb: FrameCallback | null): void;
}

export interface HubOptions {
  port: number; // 0 = OS-assigned ephemeral port (tests); disabling is the caller's job
  /** How many consecutive ports (port, port+1, …) to try when the base
   * port is taken by another instance. Ignored for port 0. Default 10. */
  portAttempts?: number;
  audioRate: number;
  /** Extra per-session facts (current frame, disk name, …) merged into the
   * GET /info response, which sibling viewers use to build a session list. */
  infoFn?: () => Record<string, unknown>;
  stateFn: () => unknown;
  /** Current screen for a just-connected viewer. Without it a viewer that
   * joins while no tool is running stares at black until the next tool
   * call pushes a frame — with the tool-driven time model that can be
   * arbitrarily far away. */
  snapshotFn?: () => { frameNo: number; rgba: Uint8Array } | null;
  width?: number;
  height?: number;
  log?: (msg: string) => void;
}

/** Above this many buffered bytes a client stops getting video, audio and
 * state — only heartbeats, until it drains. The emulator regularly runs
 * many times faster than real time inside a tool call, so a real-time
 * viewer falling behind is the NORMAL case, not a defect: degrade, never
 * disconnect. (An earlier build disconnected at 8MB, which made the view
 * flap 切断/再生中 on every long tool call.) */
const PRESSURE_SKIP_BYTES = 1 * 1024 * 1024;
/** Hard safety valve for a socket that has stopped draining entirely. */
const MAX_BUFFERED_BYTES = 32 * 1024 * 1024;

export class SpectatorHub {
  private readonly opts: HubOptions;
  private readonly width: number;
  private readonly height: number;
  private readonly log: (msg: string) => void;
  private server: Server | null = null;
  private port = 0;
  private clients = new Set<ServerResponse>();
  private sink: FrameSink | null = null;
  private prevFrame: Buffer | null = null;
  private prevState = "";
  private heartbeat: ReturnType<typeof setInterval> | null = null;
  private lastBeatMs = 0;
  private startedAt = 0;
  /** Set when a pressured client had messages skipped. Frames only flow
   * while a tool call runs the machine, so whatever was skipped near the
   * end of a call would otherwise never be re-sent — the machine pauses
   * and the view freezes on a stale frame. The heartbeat timer (which can
   * only fire between tool calls, when the event loop is free) answers
   * this flag by re-sending the machine's current screen. */
  private resyncNeeded = false;

  constructor(opts: HubOptions) {
    this.opts = opts;
    this.width = opts.width ?? 640;
    this.height = opts.height ?? 400;
    this.log = opts.log ?? ((m) => console.error(`[mz2500-mcp] ${m}`));
  }

  /** Listen on 127.0.0.1, walking up from the base port when it is taken by
   * a sibling instance. Resolves the real port, or null when every attempt
   * failed — the MCP server must keep running without the spectator view. */
  start(): Promise<number | null> {
    const attempts = this.opts.port === 0 ? 1 : Math.max(1, this.opts.portAttempts ?? 10);
    return this.tryListen(this.opts.port, attempts);
  }

  private tryListen(port: number, attemptsLeft: number): Promise<number | null> {
    return new Promise((done) => {
      const server = createServer((req, res) => this.handle(req, res));
      server.on("error", (err) => {
        const code = (err as NodeJS.ErrnoException).code;
        if (code === "EADDRINUSE" && attemptsLeft > 1) {
          this.log(`spectator port ${port} taken by another instance — trying ${port + 1}`);
          done(this.tryListen(port + 1, attemptsLeft - 1));
          return;
        }
        this.log(`spectator view disabled: ${(err as Error).message}`);
        this.server = null;
        done(null);
      });
      server.listen(port, "127.0.0.1", () => {
        this.server = server;
        this.port = (server.address() as AddressInfo).port;
        this.startedAt = Date.now();
        this.heartbeat = setInterval(() => {
          this.lastBeatMs = Date.now();
          this.broadcast(encodeMessage(MSG.heartbeat), "must");
          this.maybeResync();
        }, 1000);
        this.heartbeat.unref();
        done(this.port);
      });
    });
  }

  url(): string | null {
    return this.server ? `http://127.0.0.1:${this.port}/` : null;
  }

  /** Wire an emulator (or anything frame-shaped). The hook goes on with the
   * first viewer and off with the last. */
  attach(sink: FrameSink): void {
    this.sink = sink;
    this.syncHook();
  }

  private syncHook(): void {
    if (!this.sink) return;
    if (this.clients.size > 0) {
      this.sink.setOnFrame((frameNo, rgba, audio) => this.push(frameNo, rgba, audio));
    } else {
      this.sink.setOnFrame(null);
      this.prevFrame = null; // next viewer starts with a full frame
      this.prevState = "";
    }
  }

  /** Broadcast one emulated frame: video (or repeat when pixels are
   * unchanged), always audio, state only when it changed. */
  push(frameNo: number, rgba: Uint8Array, audio: Float32Array): void {
    if (this.clients.size === 0) return;
    // Wall-clock heartbeat from inside the frame loop: a long tool call
    // blocks the event loop, so the setInterval heartbeat cannot fire even
    // though frames are flowing — and a pressured client that is having its
    // data skipped would see total silence and think the server died.
    const now = Date.now();
    if (now - this.lastBeatMs >= 1000) {
      this.lastBeatMs = now;
      this.broadcast(encodeMessage(MSG.heartbeat), "must");
    }
    let videoMsg: Buffer | null = null;
    if (this.prevFrame && this.prevFrame.length === rgba.length && this.prevFrame.compare(rgba) === 0) {
      videoMsg = encodeMessage(MSG.repeat, withFrameNo(frameNo));
    } else {
      try {
        videoMsg = encodeMessage(
          MSG.video,
          withFrameNo(frameNo, encodePng(rgba, this.width, this.height, 1)),
        );
        this.prevFrame = Buffer.from(rgba); // copy — rgba is a WASM heap view
      } catch (err) {
        this.log(`spectator frame encode failed: ${err}`);
      }
    }
    if (videoMsg) this.broadcast(videoMsg, "video");
    this.broadcast(encodeMessage(MSG.audio, withFrameNo(frameNo, floatTo16(audio))));
    const state = JSON.stringify(this.opts.stateFn());
    if (state !== this.prevState) {
      this.prevState = state;
      this.broadcast(encodeMessage(MSG.state, withFrameNo(frameNo, Buffer.from(state, "utf8"))));
    }
  }

  private broadcast(msg: Buffer, kind: "must" | "video" | "data" = "data"): void {
    for (const res of this.clients) {
      if (res.writableLength > MAX_BUFFERED_BYTES) {
        this.log("spectator client socket stopped draining — disconnecting");
        res.destroy(); // close handler removes it from clients
        continue;
      }
      if (kind !== "must" && res.writableLength > PRESSURE_SKIP_BYTES) {
        // This client missed the message, so the dedupe baselines are no
        // longer something it has: its next video must be a full keyframe
        // (not a `repeat` it never had a base for) and its next state a
        // full resend. Cheap to force for everyone; clients still under
        // pressure just keep skipping until they catch up.
        this.prevFrame = null;
        this.prevState = "";
        this.resyncNeeded = true;
        continue;
      }
      res.write(msg);
    }
  }

  /** Re-send the machine's current screen and state after pressure skips.
   * Runs from the heartbeat timer, i.e. only between tool calls: the
   * machine is paused, so one snapshot per stall converges the view. A
   * client still above the pressure threshold skips this too — broadcast()
   * then re-raises the flag and the next beat retries. */
  private maybeResync(): void {
    if (!this.resyncNeeded || this.clients.size === 0) return;
    const snap = this.opts.snapshotFn?.();
    if (!snap) {
      this.resyncNeeded = false;
      return;
    }
    let videoMsg: Buffer;
    try {
      videoMsg = encodeMessage(
        MSG.video,
        withFrameNo(snap.frameNo, encodePng(snap.rgba, this.width, this.height, 1)),
      );
    } catch (err) {
      this.log(`spectator resync encode failed: ${err}`);
      return;
    }
    const state = JSON.stringify(this.opts.stateFn());
    this.resyncNeeded = false;
    this.prevFrame = Buffer.from(snap.rgba);
    this.prevState = state;
    this.broadcast(videoMsg, "video");
    this.broadcast(encodeMessage(MSG.state, withFrameNo(snap.frameNo, Buffer.from(state, "utf8"))));
  }

  /** Origin must match this server (or be absent), and Host must name this
   * server explicitly — the latter defends against DNS rebinding, where a
   * page served from an attacker's domain resolves to 127.0.0.1 and would
   * otherwise sail through the Origin check with no Origin header at all. */
  private requestAllowed(req: IncomingMessage): boolean {
    const host = req.headers.host;
    if (host !== `127.0.0.1:${this.port}` && host !== `localhost:${this.port}`) return false;
    const origin = req.headers.origin;
    if (!origin) return true; // direct navigation / same-origin fetch
    return origin === `http://127.0.0.1:${this.port}` || origin === `http://localhost:${this.port}`;
  }

  /** /info may be fetched by a viewer page served from a SIBLING port
   * (session list), so any localhost origin is fine there — the Host check
   * still applies, and the response carries no secrets. */
  private infoRequestAllowed(req: IncomingMessage): boolean {
    const host = req.headers.host;
    if (host !== `127.0.0.1:${this.port}` && host !== `localhost:${this.port}`) return false;
    const origin = req.headers.origin;
    if (!origin) return true;
    return /^http:\/\/(127\.0\.0\.1|localhost)(:\d+)?$/.test(origin);
  }

  private handle(req: IncomingMessage, res: ServerResponse): void {
    const url = req.url ?? "/";
    if (!(url === "/info" ? this.infoRequestAllowed(req) : this.requestAllowed(req))) {
      res.writeHead(403, { "content-type": "text/plain" });
      res.end("forbidden");
      return;
    }
    if (url === "/info") {
      const headers: Record<string, string> = {
        "content-type": "application/json",
        "cache-control": "no-store",
      };
      const origin = req.headers.origin;
      if (origin) headers["access-control-allow-origin"] = origin; // validated above
      res.writeHead(200, headers);
      res.end(
        JSON.stringify({
          app: "mz2500-mcp",
          pid: process.pid,
          port: this.port,
          startedAt: this.startedAt,
          viewers: this.clients.size,
          ...(this.opts.infoFn?.() ?? {}),
        }),
      );
      return;
    }
    if (url === "/") {
      res.writeHead(200, { "content-type": "text/html; charset=utf-8" });
      res.end(VIEWER_HTML);
      return;
    }
    if (url === "/stream") {
      res.on("error", () => {}); // an async socket error must never crash the MCP server
      res.writeHead(200, {
        "content-type": "application/octet-stream",
        "cache-control": "no-store",
        "x-content-type-options": "nosniff",
      });
      res.write(
        encodeHeader({ version: 1, width: this.width, height: this.height, audioRate: this.opts.audioRate }),
      );
      // Every new viewer needs a full frame (and current state) to start
      // from, so drop the dedupe baselines — the next push() re-sends both
      // to everyone. One extra keyframe broadcast is a cheap price for a
      // late joiner not staring at a blank canvas.
      this.prevFrame = null;
      this.prevState = "";
      // And since the next push() only happens once a tool call advances
      // the machine, hand this viewer the machine's current screen right
      // now — between tool calls "the next push" can be minutes away.
      const snap = this.opts.snapshotFn?.();
      if (snap) {
        try {
          res.write(
            encodeMessage(
              MSG.video,
              withFrameNo(snap.frameNo, encodePng(snap.rgba, this.width, this.height, 1)),
            ),
          );
          res.write(
            encodeMessage(
              MSG.state,
              withFrameNo(snap.frameNo, Buffer.from(JSON.stringify(this.opts.stateFn()), "utf8")),
            ),
          );
        } catch (err) {
          this.log(`spectator snapshot failed: ${err}`);
        }
      }
      this.clients.add(res);
      this.syncHook();
      res.on("close", () => {
        this.clients.delete(res);
        this.syncHook();
      });
      return;
    }
    res.writeHead(404, { "content-type": "text/plain" });
    res.end("not found");
  }

  close(): void {
    if (this.heartbeat) clearInterval(this.heartbeat);
    for (const res of this.clients) res.destroy();
    this.clients.clear();
    this.syncHook();
    this.server?.close();
    this.server = null;
  }
}
