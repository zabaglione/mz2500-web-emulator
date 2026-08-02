// Frontend: loads the wasm core and paces it off the AUDIO CLOCK - frames
// are emulated only while the audio queue sits below a small target depth,
// so audible latency stays pinned near TARGET_MS instead of accumulating.
// (The worklet additionally hard-caps its queue and drops the oldest
// samples, so delay is bounded even if this estimate ever drifts.)
//
// Two floppy drives are exposed (FD1/FD2, matching the real cabinet):
// per-drive hot insert never resets the machine, RESET reboots from FD1,
// and concatenated multi-volume D88 files are split into a per-drive
// volume selector so multi-disk software can swap sides mid-game.
"use strict";

const TARGET_MS = 50;         // audio depth the pacing loop maintains
const MAX_STEPS_PER_TICK = 12; // catch-up bound after a stalled tab

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
const lampEls = [document.getElementById("lamp0"), document.getElementById("lamp1")];
const dnameEls = [document.getElementById("dname0"), document.getElementById("dname1")];
const volEls = [document.getElementById("vol0"), document.getElementById("vol1")];
const fileEls = [document.getElementById("file0"), document.getElementById("file1")];

let Module = null;
let audioCtx = null;
let workletNode = null;
let running = false;

// audio-clock bookkeeping: samples handed to the worklet vs. samples the
// hardware has consumed (audioCtx.currentTime is the consumption clock)
let produced = 0;
let audioT0 = 0;
let underruns = 0;
let dropped = 0;
let lastReportedQueued = 0;

let framesShown = 0;
let fpsWindowStart = 0;

// ---- disks ----------------------------------------------------------------
// A .d88 file may hold several concatenated volumes (multi-disk releases).
function d88Volumes(bytes) {
  const volumes = [];
  let off = 0;
  while (off + 0x2b0 <= bytes.length && volumes.length < 16) {
    const size = bytes[off + 0x1c] | (bytes[off + 0x1d] << 8) |
                 (bytes[off + 0x1e] << 16) | (bytes[off + 0x1f] << 24);
    if (size < 0x2b0 || off + size > bytes.length) break;
    let title = "";
    for (let i = 0; i < 17; i++) {
      const c = bytes[off + i];
      if (c >= 0x20 && c < 0x7f) title += String.fromCharCode(c);
      else if (c === 0) break;
    }
    volumes.push({ title: title.trim(), bytes: bytes.slice(off, off + size) });
    off += size;
  }
  return volumes;
}

// drive state: null or { name, volumes, current }
const drives = [null, null];

function refreshDriveUI(drive) {
  const d = drives[drive];
  if (!d) {
    dnameEls[drive].textContent = "(empty)";
    volEls[drive].hidden = true;
    return;
  }
  const multi = d.volumes.length > 1;
  dnameEls[drive].textContent = multi ? d.name : (d.volumes[0].title || d.name);
  volEls[drive].hidden = !multi;
  if (multi) {
    volEls[drive].innerHTML = "";
    d.volumes.forEach((v, i) => {
      const o = document.createElement("option");
      o.value = i;
      o.textContent = `${i + 1}: ${v.title || "disk " + (i + 1)}`;
      volEls[drive].appendChild(o);
    });
    volEls[drive].value = d.current;
  }
}

function pushVolume(drive) {
  const d = drives[drive];
  if (!d || !Module) return false;
  const bytes = d.volumes[d.current].bytes;
  const ptr = Module._malloc(bytes.length);
  Module.HEAPU8.set(bytes, ptr);
  const ok = Module._emu_insert_disk(drive, ptr, bytes.length);
  Module._free(ptr);
  refreshDriveUI(drive);
  return ok !== 0;
}

// Hot swap (no reset). Returns true when the image parsed.
function insertFile(drive, name, bytes) {
  const volumes = d88Volumes(bytes);
  if (volumes.length === 0) {
    statusEl.textContent = "NOT A D88 IMAGE";
    return false;
  }
  drives[drive] = { name, volumes, current: 0 };
  return pushVolume(drive);
}

function restartAudio() {
  workletNode.port.postMessage({ flush: 1 });
  produced = 0;
  audioT0 = audioCtx.currentTime;
}

function coldBoot() {
  if (!Module || !drives[0]) return;
  if (!pushVolume(0)) return;
  if (drives[1]) pushVolume(1);
  if (!Module._emu_boot()) {
    statusEl.textContent = "NOT A BOOTABLE DISK";
    return;
  }
  statusEl.textContent = "RUNNING";
  restartAudio();
  if (!running) {
    running = true;
    fpsWindowStart = performance.now();
    requestAnimationFrame(tick);
  }
}

// canvas drop / initial load: mount into FD1 (volume 2 goes to FD2) and boot
function bootFromFile(name, bytes) {
  const volumes = d88Volumes(bytes);
  if (volumes.length === 0) {
    statusEl.textContent = "NOT A D88 IMAGE";
    return;
  }
  drives[0] = { name, volumes, current: 0 };
  if (volumes.length > 1) drives[1] = { name, volumes, current: 1 };
  coldBoot();
}

async function powerOn() {
  overlay.classList.add("hidden");
  statusEl.textContent = "BOOTING...";

  const v = encodeURIComponent(window.BUILD_ID || "dev");
  audioCtx = new AudioContext();
  await audioCtx.audioWorklet.addModule("audio-worklet.js?v=" + v);
  workletNode = new AudioWorkletNode(audioCtx, "mz-audio", { outputChannelCount: [2] });
  workletNode.connect(audioCtx.destination);
  workletNode.port.onmessage = (e) => {
    underruns = e.data.underruns;
    dropped = e.data.dropped;
    lastReportedQueued = e.data.queued;
    // snap the depth estimate to the worklet's ground truth (heals both
    // underrun silence-fill and any counter drift)
    produced = (audioCtx.currentTime - audioT0) * audioCtx.sampleRate + e.data.queued;
  };
  await audioCtx.resume();

  Module = await createMZ2500();
  Module._emu_init(audioCtx.sampleRate);

  const resp = await fetch("neko_can_run_demo.d88?v=" + v);
  if (!resp.ok) {
    statusEl.textContent = "DISK FETCH FAILED";
    return;
  }
  bootFromFile("neko_can_run_demo.d88", new Uint8Array(await resp.arrayBuffer()));
}

function pumpAudio() {
  let total = 0;
  for (;;) {
    const n = Module._emu_read_audio();
    if (n <= 0) break;
    const ptr = Module._emu_audio_buffer() >> 2;
    const samples = new Float32Array(Module.HEAPF32.subarray(ptr, ptr + n));
    workletNode.port.postMessage({ samples }, [samples.buffer]);
    total += n;
  }
  return total;
}

function blit() {
  const ptr = Module._emu_frame_buffer();
  const view = new Uint8ClampedArray(Module.HEAPU8.buffer, ptr, 640 * 400 * 4);
  ctx2d.putImageData(new ImageData(view, 640, 400), 0, 0);
}

function tick(now) {
  if (!running) return;
  const rate = audioCtx.sampleRate;
  const target = (TARGET_MS / 1000) * rate;

  // While the tab is hidden the context is suspended: currentTime freezes,
  // depth stays at target, and emulation pauses by itself.
  let steps = 0;
  while (steps < MAX_STEPS_PER_TICK) {
    const consumed = (audioCtx.currentTime - audioT0) * rate;
    if (produced - consumed >= target) break;
    Module._emu_run_frame();
    produced += pumpAudio();
    steps++;
  }

  pollGamepad();
  if (steps > 0) {
    Module._emu_render();
    blit();
    framesShown += steps;
  }
  const lamps = Module._emu_fdd_lamps();
  lampEls[0].classList.toggle("on", (lamps & 1) !== 0);
  lampEls[1].classList.toggle("on", (lamps & 2) !== 0);

  if (now - fpsWindowStart > 1000) {
    const bufMs = (lastReportedQueued / rate * 1000) | 0;
    fpsEl.textContent = `${(framesShown * 1000 / (now - fpsWindowStart)).toFixed(1)} fps`;
    audioStatEl.textContent =
      `audio ${bufMs}ms buf, ${underruns} underruns` + (dropped ? `, ${dropped} dropped` : "");
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
  if (!audioCtx || !workletNode) return;
  if (document.hidden) {
    audioCtx.suspend();
  } else {
    // drop anything queued while frozen and restart from a clean depth
    workletNode.port.postMessage({ flush: 1 });
    produced = (audioCtx.currentTime - audioT0) * audioCtx.sampleRate;
    audioCtx.resume();
  }
});

// ---- drive bay ------------------------------------------------------------
for (const drive of [0, 1]) {
  document.getElementById("ins" + drive).addEventListener("click", () => {
    if (Module) fileEls[drive].click();
  });
  fileEls[drive].addEventListener("change", async () => {
    const file = fileEls[drive].files[0];
    fileEls[drive].value = "";
    if (!file || !Module) return;
    insertFile(drive, file.name, new Uint8Array(await file.arrayBuffer()));
  });
  volEls[drive].addEventListener("change", () => {
    const d = drives[drive];
    if (!d) return;
    d.current = parseInt(volEls[drive].value, 10) || 0;
    pushVolume(drive); // hot swap to the chosen volume
  });
  const box = document.getElementById("ins" + drive).parentElement;
  box.addEventListener("dragover", (e) => {
    e.preventDefault();
    e.stopPropagation();
    box.classList.add("dragover");
  });
  box.addEventListener("dragleave", () => box.classList.remove("dragover"));
  box.addEventListener("drop", async (e) => {
    e.preventDefault();
    e.stopPropagation();
    box.classList.remove("dragover");
    const file = e.dataTransfer.files[0];
    if (!file || !Module) return;
    insertFile(drive, file.name, new Uint8Array(await file.arrayBuffer()));
  });
}

document.getElementById("reset-btn").addEventListener("click", () => coldBoot());

// ---- canvas drag & drop: boot from FD1 ------------------------------------
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
  bootFromFile(file.name, new Uint8Array(await file.arrayBuffer()));
});

document.getElementById("power").addEventListener("click", powerOn);
