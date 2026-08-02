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

// ---- persistence (IndexedDB): inserted disks survive reloads ------------
function idb() {
  return new Promise((res, rej) => {
    const r = indexedDB.open("mz2500w", 2);
    r.onupgradeneeded = () => {
      const db = r.result;
      if (!db.objectStoreNames.contains("drives")) db.createObjectStore("drives");
      if (!db.objectStoreNames.contains("roms")) db.createObjectStore("roms");
    };
    r.onsuccess = () => res(r.result);
    r.onerror = () => rej(r.error);
  });
}
async function saveDriveToStore(drive, name, bytes, current) {
  try {
    const db = await idb();
    db.transaction("drives", "readwrite").objectStore("drives")
      .put({ name, buffer: bytes.slice().buffer, current: current | 0 }, drive);
  } catch (e) { /* private mode etc.: persistence is best-effort */ }
}
async function loadDriveFromStore(drive) {
  try {
    const db = await idb();
    return await new Promise((res) => {
      const rq = db.transaction("drives").objectStore("drives").get(drive);
      rq.onsuccess = () => res(rq.result || null);
      rq.onerror = () => res(null);
    });
  } catch (e) { return null; }
}
async function clearDriveStore(drive) {
  try {
    const db = await idb();
    db.transaction("drives", "readwrite").objectStore("drives").delete(drive);
  } catch (e) { /* ignore */ }
}

// ---- user ROM slots (browser-local only; nothing is ever uploaded) -------
const ROM_KINDS = [
  { key: "ipl", kind: 0, label: "ipl.rom", note: "32KB / 実IPLブート用" },
  { key: "cg", kind: 1, label: "cg.rom", note: "2KB / 保管のみ（未結線）" },
  { key: "kanji", kind: 2, label: "kanji.rom", note: "256KB / バンク39h窓" },
  { key: "dict", kind: 3, label: "dict.rom", note: "256KB / バンク3Ah窓" },
];
async function saveRomToStore(key, name, bytes) {
  try {
    const db = await idb();
    db.transaction("roms", "readwrite").objectStore("roms")
      .put({ name, buffer: bytes.slice().buffer }, key);
  } catch (e) { /* best-effort */ }
}
async function loadRomFromStore(key) {
  try {
    const db = await idb();
    return await new Promise((res) => {
      const rq = db.transaction("roms").objectStore("roms").get(key);
      rq.onsuccess = () => res(rq.result || null);
      rq.onerror = () => res(null);
    });
  } catch (e) { return null; }
}
async function clearRomStore(key) {
  try {
    const db = await idb();
    db.transaction("roms", "readwrite").objectStore("roms").delete(key);
  } catch (e) { /* ignore */ }
}

async function applyRomsToMachine() {
  for (const slot of ROM_KINDS) {
    const saved = await loadRomFromStore(slot.key);
    if (!saved || !Module) continue;
    const bytes = new Uint8Array(saved.buffer);
    const ptr = Module._malloc(bytes.length);
    Module.HEAPU8.set(bytes, ptr);
    Module._emu_set_rom(slot.kind, ptr, bytes.length);
    Module._free(ptr);
  }
}

// ---- expansion-board configuration (persisted, default all installed) ----
const HW_OPTIONS = [
  { id: "hw-expram", kind: 0, key: "mzw_hw_expram" },
  { id: "hw-expgram", kind: 1, key: "mzw_hw_expgram" },
  { id: "hw-mz1m10", kind: 2, key: "mzw_hw_mz1m10" },
];
function applyHwOptionsToMachine() {
  if (!Module) return;
  for (const o of HW_OPTIONS) {
    Module._emu_set_hw_option(o.kind, document.getElementById(o.id).checked ? 1 : 0);
  }
}
for (const o of HW_OPTIONS) {
  const el = document.getElementById(o.id);
  el.checked = localStorage.getItem(o.key) !== "0"; // default: installed
  el.addEventListener("change", () => {
    localStorage.setItem(o.key, el.checked ? "1" : "0");
    applyHwOptionsToMachine(); // RAM/GRAM changes settle at the next RESET
  });
}

const realIplEl = document.getElementById("real-ipl-mode");
realIplEl.checked = localStorage.getItem("mzw_real_ipl") === "1";
realIplEl.addEventListener("change", () => {
  localStorage.setItem("mzw_real_ipl", realIplEl.checked ? "1" : "0");
});

async function refreshRomSlots() {
  const box = document.getElementById("rom-slots");
  box.innerHTML = "";
  for (const slot of ROM_KINDS) {
    const saved = await loadRomFromStore(slot.key);
    const row = document.createElement("div");
    row.className = "rom-slot";
    const state = saved
      ? `登録済: ${saved.name} (${(saved.buffer.byteLength / 1024) | 0}KB)`
      : "未登録";
    row.innerHTML =
      `<span class="rname">${slot.label}</span>` +
      `<span class="rstate">${state}</span>` +
      `<button class="insbtn" data-act="reg">登録</button>` +
      `<button class="insbtn" data-act="del">消去</button>` +
      `<span class="fine">${slot.note}</span>`;
    row.querySelector('[data-act="reg"]').addEventListener("click", () => {
      const input = document.createElement("input");
      input.type = "file";
      input.onchange = async () => {
        const file = input.files[0];
        if (!file) return;
        const bytes = new Uint8Array(await file.arrayBuffer());
        await saveRomToStore(slot.key, file.name, bytes);
        if (Module) {
          const ptr = Module._malloc(bytes.length);
          Module.HEAPU8.set(bytes, ptr);
          Module._emu_set_rom(slot.kind, ptr, bytes.length);
          Module._free(ptr);
        }
        refreshRomSlots();
      };
      input.click();
    });
    row.querySelector('[data-act="del"]').addEventListener("click", async () => {
      await clearRomStore(slot.key);
      refreshRomSlots();
    });
    box.appendChild(row);
  }
}
refreshRomSlots();

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
function insertFile(drive, name, bytes, opts) {
  const volumes = d88Volumes(bytes);
  if (volumes.length === 0) {
    statusEl.textContent = "NOT A D88 IMAGE";
    return false;
  }
  drives[drive] = { name, volumes, current: (opts && opts.current) || 0 };
  if (!opts || !opts.noSave) saveDriveToStore(drive, name, bytes, drives[drive].current);
  return pushVolume(drive);
}

function restartAudio() {
  workletNode.port.postMessage({ flush: 1 });
  produced = 0;
  audioT0 = audioCtx.currentTime;
}

// Watchdog for the experimental real-IPL boot: if the firmware parks in a
// trap loop (constant PC while frames advance, or HALT), fall back to the
// dummy IPL automatically so a stored setting can never leave the screen
// black across reloads.
let iplWatchTimer = null;
let iplWatchLastPc = -1;
let iplWatchLastFrame = -1;
let iplWatchSame = 0;
let iplWatchTicks = 0;

function stopIplWatchdog() {
  if (iplWatchTimer) clearInterval(iplWatchTimer);
  iplWatchTimer = null;
}

function checkRealIplStall() {
  if (!Module || !running) return false;
  const j = JSON.parse(Module.UTF8ToString(Module._emu_debug_json()));
  const advanced = j.frames !== iplWatchLastFrame;
  iplWatchLastFrame = j.frames;
  if (!advanced) return false; // paused/hidden: no verdict
  if (j.cpu.halted || j.cpu.pc === iplWatchLastPc) iplWatchSame++;
  else iplWatchSame = 0;
  iplWatchLastPc = j.cpu.pc;
  iplWatchTicks++;
  if (iplWatchSame >= 3) {
    stopIplWatchdog();
    realIplEl.checked = false;
    localStorage.setItem("mzw_real_ipl", "0");
    coldBoot();
    statusEl.textContent =
      `IPL停止検出 (PC=${j.cpu.pc.toString(16).toUpperCase()}h) → ダミーIPLで再起動しました`;
    return true;
  }
  if (iplWatchTicks > 20) stopIplWatchdog(); // firmware looks alive
  return false;
}

function startIplWatchdog() {
  stopIplWatchdog();
  iplWatchLastPc = -1;
  iplWatchLastFrame = -1;
  iplWatchSame = 0;
  iplWatchTicks = 0;
  iplWatchTimer = setInterval(checkRealIplStall, 1000);
}

function coldBoot() {
  if (!Module) return;
  const wantRealIpl = realIplEl.checked && Module._emu_has_ipl();
  if (!drives[0] && !wantRealIpl) return;
  if (drives[0]) pushVolume(0);
  if (drives[1]) pushVolume(1);
  const ok = wantRealIpl ? Module._emu_boot_real_ipl()
                         : (drives[0] ? Module._emu_boot() : 0);
  if (!ok) {
    statusEl.textContent = "NOT A BOOTABLE DISK";
    return;
  }
  if (wantRealIpl) {
    statusEl.textContent = "RUNNING (REAL IPL)";
    startIplWatchdog();
  } else {
    statusEl.textContent = "RUNNING";
    stopIplWatchdog();
  }
  restartAudio();
  if (!running) {
    running = true;
    fpsWindowStart = performance.now();
    requestAnimationFrame(tick);
  }
}

// canvas drop / initial load: mount into FD1 (volume 2 goes to FD2) and boot
function bootFromFile(name, bytes, opts) {
  const volumes = d88Volumes(bytes);
  if (volumes.length === 0) {
    statusEl.textContent = "NOT A D88 IMAGE";
    return;
  }
  drives[0] = { name, volumes, current: 0 };
  if (volumes.length > 1) drives[1] = { name, volumes, current: 1 };
  if (!opts || !opts.noSave) {
    saveDriveToStore(0, name, bytes, 0);
    if (volumes.length > 1) saveDriveToStore(1, name, bytes, 1);
  }
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
  applyHwOptionsToMachine();
  await applyRomsToMachine();

  // restore disks saved in this browser (IndexedDB); FD1 boots in place of
  // the bundled demo, FD2 is remounted alongside
  const saved0 = await loadDriveFromStore(0);
  const saved1 = await loadDriveFromStore(1);
  if (saved1) {
    insertFile(1, saved1.name, new Uint8Array(saved1.buffer),
               { noSave: true, current: saved1.current });
  }
  if (saved0) {
    const bytes = new Uint8Array(saved0.buffer);
    const volumes = d88Volumes(bytes);
    if (volumes.length > 0) {
      drives[0] = { name: saved0.name, volumes, current: saved0.current || 0 };
      coldBoot();
      return;
    }
  }

  const resp = await fetch("neko_can_run_demo.d88?v=" + v);
  if (!resp.ok) {
    statusEl.textContent = "DISK FETCH FAILED";
    return;
  }
  // the bundled demo is not persisted; only user-inserted disks are
  bootFromFile("neko_can_run_demo.d88", new Uint8Array(await resp.arrayBuffer()),
               { noSave: true });
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
    loadDriveFromStore(drive).then((saved) => {
      if (saved && saved.name === d.name)
        saveDriveToStore(drive, saved.name, new Uint8Array(saved.buffer), d.current);
    });
  });
  document.getElementById("eject" + drive).addEventListener("click", () => {
    clearDriveStore(drive);
    dnameEls[drive].textContent = drives[drive]
      ? dnameEls[drive].textContent + " (保存消去)"
      : "(empty)";
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

// ---- debug panel ----------------------------------------------------------
const debugToggle = document.getElementById("debug-toggle");
const debugPanel = document.getElementById("debug-panel");
const debugText = document.getElementById("debug-text");
const debugWatch = document.getElementById("debug-watch");
const debugWatchOut = document.getElementById("debug-watch-out");

let debugVisible = localStorage.getItem("mzw_debug") === "1";
debugPanel.hidden = !debugVisible;
debugToggle.classList.toggle("active", debugVisible);
debugWatch.value = localStorage.getItem("mzw_watch") || "";

debugToggle.addEventListener("click", () => {
  debugVisible = !debugVisible;
  debugPanel.hidden = !debugVisible;
  debugToggle.classList.toggle("active", debugVisible);
  localStorage.setItem("mzw_debug", debugVisible ? "1" : "0");
});
debugWatch.addEventListener("change", () => {
  localStorage.setItem("mzw_watch", debugWatch.value);
});

const hex2 = (v) => v.toString(16).toUpperCase().padStart(2, "0");
const hex4 = (v) => v.toString(16).toUpperCase().padStart(4, "0");

setInterval(() => {
  if (!debugVisible || !Module || !running) return;
  try {
    const j = JSON.parse(Module.UTF8ToString(Module._emu_debug_json()));
    const c = j.cpu;
    debugText.textContent =
      `frame ${j.frames}  cyc ${j.cycles}\n` +
      `CPU  PC=${hex4(c.pc)} SP=${hex4(c.sp)} A=${hex2(c.a)} ` +
      `BC=${hex4(c.bc)} DE=${hex4(c.de)} HL=${hex4(c.hl)}  ` +
      `IM${c.im} IFF${c.iff1}${c.halted ? " HALT" : ""}\n` +
      `BANK ${j.bank.map(hex2).join(" ")}   TEXT ${j.text80 ? 80 : 40}col  ` +
      `KANJI ${hex2(j.kanji)}  IPL-ROM ${j.ipl_rom ? "loaded" : "-"}\n` +
      `GDE  mode=${hex2(j.gde.mode)} SAD0=${hex4(j.gde.sad0)} HDSC=${j.gde.hdsc}\n` +
      `FDC  ${j.fdc.motor ? "MOTOR" : "idle"} drv${j.fdc.drive} cyl${j.fdc.cyl} ` +
      `reads=${j.fdc.reads} seeks=${j.fdc.seeks}\n` +
      `INT  sel=${hex2(j.int.select)} vec=${hex2(j.int.vector)} ` +
      `PIT=${j.int.pit_reload}${j.int.pit_on ? " on" : " off"}`;
    const parts = [];
    for (const tok of debugWatch.value.split(",")) {
      const t = tok.trim();
      if (!t) continue;
      const addr = parseInt(t, 16);
      if (!isNaN(addr)) parts.push(`${hex4(addr & 0xffff)}=${hex2(Module._emu_read_mem(addr & 0xffff))}`);
    }
    debugWatchOut.textContent = parts.join("  ");
  } catch (e) {
    debugText.textContent = String(e);
  }
}, 250);
