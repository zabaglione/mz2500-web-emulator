// Frontend: loads the wasm core, paces it at the MZ-2500 frame rate with a
// time accumulator, feeds the canvas and the audio worklet, and maps
// keyboard / gamepad input onto the keyboard matrix and joystick port.
"use strict";

const FPS = 6000000 / 108160; // ~55.47, must match core/timing.h
const FRAME_MS = 1000 / FPS;

// KeyboardEvent.code -> [row, bit]; matches core/keyboard.h
const KEYMAP = new Map([
  ["Space", [3, 1]],
  ["ArrowUp", [3, 3]],
  ["ArrowDown", [3, 4]],
  ["ArrowLeft", [3, 5]],
  ["ArrowRight", [3, 6]],
]);
for (let i = 0; i < 26; i++) {
  const index = i + 1; // A = position 1 of row 4
  KEYMAP.set("Key" + String.fromCharCode(65 + i), [4 + (index >> 3), index & 7]);
}

// joystick port EFh bits (active masks)
const JOY_UP = 0x01, JOY_DOWN = 0x02, JOY_LEFT = 0x04, JOY_RIGHT = 0x08;
const JOY_DASH = 0x10, JOY_JUMP = 0x20;

const canvas = document.getElementById("screen");
const ctx2d = canvas.getContext("2d");
const overlay = document.getElementById("overlay");
const statusEl = document.getElementById("status");
const fpsEl = document.getElementById("fps");
const audioStatEl = document.getElementById("audio-stat");

let Module = null;
let audioCtx = null;
let workletNode = null;
let running = false;
let acc = 0;
let last = 0;
let framesShown = 0;
let fpsWindowStart = 0;
let queuedSamples = 0;
let underruns = 0;

async function powerOn() {
  overlay.classList.add("hidden");
  statusEl.textContent = "BOOTING...";

  audioCtx = new AudioContext();
  await audioCtx.audioWorklet.addModule("audio-worklet.js");
  workletNode = new AudioWorkletNode(audioCtx, "mz-audio", { outputChannelCount: [2] });
  workletNode.connect(audioCtx.destination);
  workletNode.port.onmessage = (e) => {
    queuedSamples = e.data.queued;
    underruns = e.data.underruns;
  };

  Module = await createMZ2500();
  Module._emu_init(audioCtx.sampleRate);

  const resp = await fetch("neko_can_run_demo.d88");
  if (!resp.ok) {
    statusEl.textContent = "DISK FETCH FAILED";
    return;
  }
  bootDisk(new Uint8Array(await resp.arrayBuffer()));
}

function bootDisk(bytes) {
  const ptr = Module._malloc(bytes.length);
  Module.HEAPU8.set(bytes, ptr);
  const ok = Module._emu_load_disk(ptr, bytes.length);
  Module._free(ptr);
  if (!ok) {
    statusEl.textContent = "NOT A BOOTABLE DISK";
    return;
  }
  statusEl.textContent = "RUNNING";
  if (!running) {
    running = true;
    last = performance.now();
    fpsWindowStart = last;
    requestAnimationFrame(tick);
  }
}

function pumpAudio() {
  const n = Module._emu_read_audio();
  if (n > 0) {
    const ptr = Module._emu_audio_buffer() >> 2;
    const samples = new Float32Array(Module.HEAPF32.subarray(ptr, ptr + n));
    workletNode.port.postMessage({ samples }, [samples.buffer]);
  }
}

function blit() {
  const ptr = Module._emu_frame_buffer();
  const view = new Uint8ClampedArray(Module.HEAPU8.buffer, ptr, 640 * 400 * 4);
  ctx2d.putImageData(new ImageData(view, 640, 400), 0, 0);
}

function tick(now) {
  if (!running) return;
  acc += now - last;
  last = now;

  // audio-queue feedback: nudge pacing +-0.5% to hold 60-100ms of buffer
  const target = audioCtx.sampleRate * 0.08;
  const nudge = queuedSamples > target * 1.3 ? 1.005 : queuedSamples < target * 0.7 ? 0.995 : 1.0;

  let steps = 0;
  while (acc >= FRAME_MS * nudge && steps < 6) {
    Module._emu_run_frame();
    pumpAudio();
    acc -= FRAME_MS * nudge;
    steps++;
  }
  if (steps === 6) acc = 0; // fell behind: drop the debt instead of spiraling

  pollGamepad();
  if (steps > 0) {
    Module._emu_render();
    blit();
    framesShown += steps;
  }
  if (now - fpsWindowStart > 1000) {
    fpsEl.textContent = `${(framesShown * 1000 / (now - fpsWindowStart)).toFixed(1)} fps`;
    audioStatEl.textContent = `audio ${(queuedSamples / audioCtx.sampleRate * 1000) | 0}ms buf, ${underruns} underruns`;
    workletNode.port.postMessage({ query: 1 });
    framesShown = 0;
    fpsWindowStart = now;
  }
  requestAnimationFrame(tick);
}

// ---- input ----------------------------------------------------------------
document.addEventListener("keydown", (e) => {
  const pos = KEYMAP.get(e.code);
  if (pos && Module && running) {
    Module._emu_key(pos[0], pos[1], 1);
    e.preventDefault();
  }
});
document.addEventListener("keyup", (e) => {
  const pos = KEYMAP.get(e.code);
  if (pos && Module && running) {
    Module._emu_key(pos[0], pos[1], 0);
    e.preventDefault();
  }
});

function pollGamepad() {
  const pads = navigator.getGamepads ? navigator.getGamepads() : [];
  let mask = 0;
  for (const pad of pads) {
    if (!pad) continue;
    const ax = pad.axes[0] || 0, ay = pad.axes[1] || 0;
    if (ax < -0.4 || (pad.buttons[14] && pad.buttons[14].pressed)) mask |= JOY_LEFT;
    if (ax > 0.4 || (pad.buttons[15] && pad.buttons[15].pressed)) mask |= JOY_RIGHT;
    if (ay < -0.4 || (pad.buttons[12] && pad.buttons[12].pressed)) mask |= JOY_UP;
    if (ay > 0.4 || (pad.buttons[13] && pad.buttons[13].pressed)) mask |= JOY_DOWN;
    if (pad.buttons[0] && pad.buttons[0].pressed) mask |= JOY_JUMP; // A/cross
    if (pad.buttons[2] && pad.buttons[2].pressed) mask |= JOY_DASH; // X/square
    break;
  }
  if (Module) Module._emu_joy(mask);
}

// ---- pause when hidden ----------------------------------------------------
document.addEventListener("visibilitychange", () => {
  if (!audioCtx) return;
  if (document.hidden) {
    audioCtx.suspend();
    acc = 0;
  } else {
    audioCtx.resume();
    last = performance.now();
  }
});

// ---- drag & drop ----------------------------------------------------------
const wrap = document.getElementById("screen-wrap");
wrap.addEventListener("dragover", (e) => {
  e.preventDefault();
  wrap.classList.add("dragover");
});
wrap.addEventListener("dragleave", () => wrap.classList.remove("dragover"));
wrap.addEventListener("drop", async (e) => {
  e.preventDefault();
  wrap.classList.remove("dragover");
  const file = e.dataTransfer.files[0];
  if (!file || !Module) return;
  bootDisk(new Uint8Array(await file.arrayBuffer()));
});

document.getElementById("power").addEventListener("click", powerOn);
