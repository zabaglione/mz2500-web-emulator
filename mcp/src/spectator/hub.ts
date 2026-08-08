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
  audioRate: number;
  stateFn: () => unknown;
  width?: number;
  height?: number;
  log?: (msg: string) => void;
}

/** Above this many buffered bytes a client stops getting video… */
const VIDEO_SKIP_BYTES = 1 * 1024 * 1024;
/** …and above this it is disconnected (rejoin lands on the live edge). */
const MAX_BUFFERED_BYTES = 8 * 1024 * 1024;

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

  constructor(opts: HubOptions) {
    this.opts = opts;
    this.width = opts.width ?? 640;
    this.height = opts.height ?? 400;
    this.log = opts.log ?? ((m) => console.error(`[mz2500-mcp] ${m}`));
  }

  /** Listen on 127.0.0.1. Resolves the real port, or null when the port is
   * taken — the MCP server must keep running without the spectator view. */
  start(): Promise<number | null> {
    return new Promise((done) => {
      const server = createServer((req, res) => this.handle(req, res));
      server.on("error", (err) => {
        this.log(`spectator view disabled: ${(err as Error).message}`);
        this.server = null;
        done(null);
      });
      server.listen(this.opts.port, "127.0.0.1", () => {
        this.server = server;
        this.port = (server.address() as AddressInfo).port;
        this.heartbeat = setInterval(() => this.broadcast(encodeMessage(MSG.heartbeat)), 1000);
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
    if (videoMsg) this.broadcast(videoMsg, true);
    this.broadcast(encodeMessage(MSG.audio, withFrameNo(frameNo, floatTo16(audio))));
    const state = JSON.stringify(this.opts.stateFn());
    if (state !== this.prevState) {
      this.prevState = state;
      this.broadcast(encodeMessage(MSG.state, withFrameNo(frameNo, Buffer.from(state, "utf8"))));
    }
  }

  private broadcast(msg: Buffer, isVideo = false): void {
    for (const res of this.clients) {
      if (res.writableLength > MAX_BUFFERED_BYTES) {
        this.log("spectator client too slow — disconnecting");
        res.destroy(); // close handler removes it from clients
        continue;
      }
      if (isVideo && res.writableLength > VIDEO_SKIP_BYTES) {
        // This client didn't get the frame, so its next video message must
        // be a full keyframe rather than a `repeat` it never had a base for.
        // Cheap to force for everyone; clients still under pressure just
        // keep skipping until they catch up.
        this.prevFrame = null;
        continue;
      }
      res.write(msg);
    }
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

  private handle(req: IncomingMessage, res: ServerResponse): void {
    if (!this.requestAllowed(req)) {
      res.writeHead(403, { "content-type": "text/plain" });
      res.end("forbidden");
      return;
    }
    const url = req.url ?? "/";
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
