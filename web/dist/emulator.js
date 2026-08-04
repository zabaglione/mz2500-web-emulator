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

// KeyboardEvent.code -> [row, bit]; matches core/keyboard.h (I/O map key
// matrix; SHIFT/'-'/';'/':' verified by typing into BASIC-M25 and reading
// the echo). Punctuation follows the JIS layout the real machine uses.
const KEYMAP = new Map([
  ["Space", [3, 1]],
  ["Tab", [3, 0]],
  ["Enter", [3, 2]],
  ["ArrowUp", [3, 3]],
  ["ArrowDown", [3, 4]],
  ["ArrowLeft", [3, 5]],
  ["ArrowRight", [3, 6]],
  ["Minus", [9, 4]],        // JIS: the key right of 0
  ["Equal", [7, 3]],        // JIS: ^
  ["IntlYen", [7, 4]],      // JIS: yen
  ["IntlRo", [7, 5]],       // JIS: _
  ["Semicolon", [9, 3]],
  ["Quote", [9, 2]],        // JIS: :
  ["BracketLeft", [9, 5]],  // JIS: @
  ["BracketRight", [9, 6]], // JIS: [
  ["Backslash", [10, 0]],   // JIS: ]
  ["Comma", [7, 7]],
  ["Period", [7, 6]],
  // (4,0), not (9,7): the IO map's key matrix puts "/" on strobe 4 bit 0,
  // and pulsing the two cells at BASIC-M25's Ok prompt agrees - (4,0)
  // echoes "/" and (9,7) echoes nothing at all (no key sits there). The
  // old (9,7) binding meant the host "/" key silently did nothing.
  ["Slash", [4, 0]],
  ["Backspace", [10, 4]],
  ["Delete", [10, 3]],
  ["Escape", [10, 5]],
  ["ShiftLeft", [11, 2]],
  ["ShiftRight", [11, 2]],
  ["ControlLeft", [11, 4]],
  ["ControlRight", [11, 4]],
  ["NumpadAdd", [1, 6]],
  ["Numpad8", [1, 2]],
  ["Numpad9", [1, 3]],
]);
for (let i = 0; i < 26; i++) {
  const index = i + 1; // A = position 1 of row 4
  KEYMAP.set("Key" + String.fromCharCode(65 + i), [4 + (index >> 3), index & 7]);
}
for (let d = 0; d <= 9; d++) {
  KEYMAP.set("Digit" + d, [8 + (d >> 3), d & 7]); // main row digits
  if (d < 8) KEYMAP.set("Numpad" + d, [2, d]);    // tenkey 0-7
}
for (let f = 1; f <= 10; f++) {
  KEYMAP.set("F" + f, [f <= 8 ? 0 : 1, (f - 1) & 7]);
}

// Joystick port EFh bits (active masks). Trigger 1 is bit5 and trigger 2 is
// bit4 - that is the machine's own numbering, from the port EFh table in
// Oh!MZ's hardware analysis. What a trigger does is the software's
// business, never this layer's.
const JOY_UP = 0x01, JOY_DOWN = 0x02, JOY_LEFT = 0x04, JOY_RIGHT = 0x08;
const JOY_TRIG1 = 0x20, JOY_TRIG2 = 0x10;

// ---- gamepad button assignment (persisted) --------------------------------
// A real MZ-2500 stick has two buttons, so the pad has to say which of its
// own drives which. Standard-mapping indices; the labels name the face
// buttons the way an Xbox and a PlayStation pad print them.
const PAD_BUTTONS = [
  [0, "A / ×"], [1, "B / ○"], [2, "X / □"], [3, "Y / △"],
  [4, "L1 / L"], [5, "R1 / R"], [6, "L2 / LT"], [7, "R2 / RT"],
];
// Default: the pad's primary button is trigger 1, the one beside it trigger
// 2. Fixed - see docs/gamepad-mapping.md before ever moving it.
const PAD_DEFAULT = { trig1: 0, trig2: 1 };
const padAssign = { ...PAD_DEFAULT };

function padSelects() {
  return [["trig1", document.getElementById("pad-trig1")],
          ["trig2", document.getElementById("pad-trig2")]];
}
for (const [role, sel] of padSelects()) {
  for (const [index, label] of PAD_BUTTONS) {
    const o = document.createElement("option");
    o.value = index;
    o.textContent = label;
    sel.appendChild(o);
  }
  const saved = parseInt(localStorage.getItem("mzw_pad_" + role), 10);
  padAssign[role] = Number.isInteger(saved) ? saved : PAD_DEFAULT[role];
  sel.value = padAssign[role];
  sel.addEventListener("change", () => {
    padAssign[role] = parseInt(sel.value, 10);
    localStorage.setItem("mzw_pad_" + role, String(padAssign[role]));
  });
}
document.getElementById("pad-reset").addEventListener("click", () => {
  for (const [role, sel] of padSelects()) {
    padAssign[role] = PAD_DEFAULT[role];
    sel.value = padAssign[role];
    localStorage.removeItem("mzw_pad_" + role);
  }
});

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

// Inverse of d88Volumes(): lay every volume's bytes back to back in order.
// For a single-volume file this round-trips byte-identically; for a
// multi-volume file it reproduces the concatenated original (up to the last
// volume d88Volumes() could parse - trailing garbage past that point was
// already dropped when the file was first split, same as before this).
function concatVolumes(volumes) {
  let total = 0;
  for (const v of volumes) total += v.bytes.length;
  const out = new Uint8Array(total);
  let off = 0;
  for (const v of volumes) {
    out.set(v.bytes, off);
    off += v.bytes.length;
  }
  return out;
}

// Defensive clamp for a stored `current` index: a record written by a buggy
// persist path (or hand-edited storage) could point past the volume list it
// is paired with, which would otherwise throw in pushVolume() at boot.
function clampVolumeIndex(current, count) {
  current = current | 0;
  if (count <= 0) return 0;
  if (current < 0) return 0;
  if (current >= count) return count - 1;
  return current;
}

// drive state: null or { name, volumes, current }
const drives = [null, null];

// ---- persistence (IndexedDB): inserted disks survive reloads ------------
// The connection is opened once and cached (both as a promise for callers
// that can await, and as the resolved IDBDatabase itself in dbConn) so a
// pagehide flush (Finding 5) never has to pay for a fresh indexedDB.open()
// round trip - by the time anything could be dirty, refreshRomSlots() at
// startup has already opened it.
let dbConn = null;
let dbPromise = null;
function idb() {
  if (!dbPromise) {
    dbPromise = new Promise((res, rej) => {
      const r = indexedDB.open("mz2500w", 2);
      r.onupgradeneeded = () => {
        const db = r.result;
        if (!db.objectStoreNames.contains("drives")) db.createObjectStore("drives");
        if (!db.objectStoreNames.contains("roms")) db.createObjectStore("roms");
      };
      r.onsuccess = () => { dbConn = r.result; res(r.result); };
      r.onerror = () => { dbPromise = null; rej(r.error); };
    });
  }
  return dbPromise;
}

// Synchronous fast path: issues the transaction's put() with zero `await` in
// between, so a pagehide/visibilitychange handler reaches the actual
// IndexedDB call within the same task as the event itself, rather than
// deferring it to a microtask that a page teardown might never run. Returns
// true once the call has been made (not once it has committed - that part is
// out of JS's control either way). Falls back to false when the connection
// has not finished opening yet, so the caller can fall back to the async path.
function putDriveRecordSync(drive, name, bytes, current) {
  if (!dbConn) return false;
  try {
    dbConn.transaction("drives", "readwrite").objectStore("drives")
      .put({ name, buffer: bytes.slice().buffer, current: current | 0 }, drive);
    return true;
  } catch (e) {
    return false;
  }
}

async function saveDriveToStore(drive, name, bytes, current) {
  if (putDriveRecordSync(drive, name, bytes, current)) return;
  try {
    await idb();
    putDriveRecordSync(drive, name, bytes, current);
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
async function insertFile(drive, name, bytes, opts) {
  const volumes = d88Volumes(bytes);
  if (volumes.length === 0) {
    statusEl.textContent = "NOT A D88 IMAGE";
    return false;
  }
  const current = clampVolumeIndex((opts && opts.current) || 0, volumes.length);
  // Flush any write still sitting in the debounce for the OUTGOING disk
  // before this drive record is replaced - persistDisk() resolves what to
  // save (and under what name) through drives[drive], so a pending flush has
  // to land while that still points at the disk actually sitting in the
  // core. Same ordering bug as bootFromFile()/coldBoot() (see finding 1):
  // reassigning first would let the flush snapshot the CORE's still-old
  // bytes into the NEW disk's record and name.
  await flushDiskPersist(drive);
  drives[drive] = { name, volumes, current };
  if (!opts || !opts.noSave) saveDriveToStore(drive, name, bytes, drives[drive].current);
  if (!Module) {
    // power still off: keep the selection, it is mounted at POWER ON
    refreshDriveUI(drive);
    return true;
  }
  return pushVolume(drive);
}

// ---- blank disks, saving, and the write-protect tab ---------------------

// The machine has written to a disk: pull the image back out and keep it.
// Debounced, because a format writes every track in a burst.
const diskSaveTimers = [null, null];

// Snapshot the CURRENT volume out of the core and fold it back into the
// drive's full multi-volume buffer before storing - storing just the current
// volume would silently discard every other volume of a multi-disk release,
// and corrupt the record for the next cold boot (current would then point
// past a one-element volume list). Returns the saveDriveToStore() promise (or
// undefined when there was nothing to persist) so callers that care about
// completion - e.g. a flush before switching volumes - can wait on it.
function persistDisk(drive) {
  if (!Module) return;
  if (!drives[drive]) {
    // No disk mounted, but the core's dirty flag may still be set (e.g. WP
    // was toggled on an empty drive). Clear it here too, or the per-frame
    // dirty poll would keep rescheduling this timer forever.
    Module._emu_disk_clear_dirty(drive);
    return;
  }
  const size = Module._emu_disk_snapshot(drive);
  if (size <= 0) {
    Module._emu_disk_clear_dirty(drive);
    return;
  }
  const ptr = Module._emu_disk_data();
  const bytes = Module.HEAPU8.slice(ptr, ptr + size); // copy out before any other snapshot call
  const d = drives[drive];
  d.volumes[d.current].bytes = bytes;
  Module._emu_disk_clear_dirty(drive);
  return saveDriveToStore(drive, d.name, concatVolumes(d.volumes), d.current);
}

function scheduleDiskPersist(drive) {
  if (diskSaveTimers[drive]) return;
  diskSaveTimers[drive] = setTimeout(() => {
    diskSaveTimers[drive] = null;
    persistDisk(drive);
  }, 1000);
}

// Cancel and run a pending persist right now (no debounce), returning a
// promise that resolves once the store write has been issued. Used before
// anything that would make the core forget the in-progress write (switching
// volumes, ejecting, the page going away).
function flushDiskPersist(drive) {
  if (!diskSaveTimers[drive]) return Promise.resolve();
  clearTimeout(diskSaveTimers[drive]);
  diskSaveTimers[drive] = null;
  return Promise.resolve(persistDisk(drive));
}

async function insertBlank(drive) {
  if (!Module) {
    statusEl.textContent = "POWER ON FIRST";
    return;
  }
  // Same ordering requirement as insertFile(): flush the outgoing disk's
  // pending write before _emu_insert_blank_disk() replaces the core's disk
  // out from under it.
  await flushDiskPersist(drive);
  if (!Module._emu_insert_blank_disk(drive)) return;
  const size = Module._emu_disk_snapshot(drive);
  const ptr = Module._emu_disk_data();
  const bytes = Module.HEAPU8.slice(ptr, ptr + size);
  drives[drive] = { name: "BLANK", volumes: [{ bytes, title: "BLANK" }], current: 0 };
  saveDriveToStore(drive, "BLANK", bytes, 0);
  refreshDriveUI(drive);
  refreshWpUI(drive);
  statusEl.textContent = `FD${drive + 1}: BLANK DISK (UNFORMATTED)`;
}

function downloadDisk(drive) {
  if (!Module) {
    statusEl.textContent = "POWER ON FIRST";
    return;
  }
  if (!drives[drive]) {
    statusEl.textContent = `FD${drive + 1}: NO DISK`;
    return;
  }
  const size = Module._emu_disk_snapshot(drive);
  if (size <= 0) return;
  const ptr = Module._emu_disk_data();
  const bytes = Module.HEAPU8.slice(ptr, ptr + size);
  const url = URL.createObjectURL(new Blob([bytes], { type: "application/octet-stream" }));
  const a = document.createElement("a");
  a.href = url;
  const name = drives[drive].name || "disk";
  // A disk loaded from a file already carries the .d88 extension in `name`;
  // do not double it up into "foo.d88.d88".
  a.download = /\.d88$/i.test(name) ? name : `${name}.d88`;
  a.click();
  URL.revokeObjectURL(url);
}

function refreshWpUI(drive) {
  const btn = document.getElementById(`wp${drive}`);
  if (!btn) return;
  const on = Module ? Module._emu_disk_wp(drive) !== 0 : false;
  btn.setAttribute("aria-pressed", on ? "true" : "false");
}

for (const drive of [0, 1]) {
  document.getElementById(`blank${drive}`).addEventListener("click", () => insertBlank(drive));
  document.getElementById(`save${drive}`).addEventListener("click", () => downloadDisk(drive));
  document.getElementById(`wp${drive}`).addEventListener("click", () => {
    if (!Module) {
      statusEl.textContent = "POWER ON FIRST";
      return;
    }
    if (!drives[drive]) {
      statusEl.textContent = `FD${drive + 1}: NO DISK`;
      return;
    }
    const next = Module._emu_disk_wp(drive) === 0 ? 1 : 0;
    Module._emu_disk_set_wp(drive, next);
    refreshWpUI(drive);
    scheduleDiskPersist(drive);
  });
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
    // Only rescue a genuinely BLANK screen (bricked boot). If the firmware
    // has drawn anything, a tight loop is normal behaviour (device search,
    // error beeper) - report it and keep the real IPL running.
    if (Module._emu_frame_nonblack() > 0) {
      stopIplWatchdog();
      statusEl.textContent =
        `RUNNING (REAL IPL) - 待機ループ検出 PC=${j.cpu.pc.toString(16).toUpperCase()}h`;
      return false;
    }
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

async function coldBoot() {
  if (!Module) return;
  // Land any write still sitting in the persist debounce BEFORE reloading -
  // pushVolume() -> _emu_insert_disk -> D88Disk::load() clears the core's
  // dirty flag and replaces the tracks with whatever was last persisted, so
  // a save still inside the ~1s debounce window would be silently reloaded
  // away here, and the stale timer would then snapshot that reloaded
  // (pre-write) image right back over the good copy in both the running
  // machine and IndexedDB a second later. Same precedent as the volume
  // selector and eject handlers below.
  await Promise.all([flushDiskPersist(0), flushDiskPersist(1)]);
  // The rAF loop (tick()) keeps running across the await above, so its
  // per-frame dirty poll (scheduleDiskPersist()) can arm a FRESH one-second
  // timer for a drive in the ~1 frame between the flush landing and
  // pushVolume() reloading that drive's disk below. Left alone, that timer
  // would fire ~1s from now and snapshot the just-reloaded (pre-write) image
  // right back over the good copy this just flushed. Cancel any such timer
  // and clear the core's dirty flag before reloading - both are free (there
  // is nothing left worth persisting once the reload below replaces the
  // disk anyway).
  for (const drive of [0, 1]) {
    if (diskSaveTimers[drive]) {
      clearTimeout(diskSaveTimers[drive]);
      diskSaveTimers[drive] = null;
    }
    Module._emu_disk_clear_dirty(drive);
  }
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
//
// Finding 1: this used to assign drives[0]/drives[1] for the INCOMING disk
// and only then call coldBoot(), which flushes any pending persist. But
// persistDisk() resolves both "which bytes" (core snapshot) and "which
// record to save them under" (drives[n].name/volumes) - the core snapshot
// side is only correct while the core still holds the OUTGOING disk, and the
// record side is only correct while drives[n] still refers to the OUTGOING
// disk. Reassigning drives[] first left the record side pointing at the
// INCOMING disk while the core snapshot still held the OUTGOING disk's
// bytes, so a write still inside the debounce window got saved under the
// new disk's name, and pushVolume() then loaded those stale bytes into the
// machine in place of the disk that was just dropped in. Flushing here,
// before the reassignment, is the fix: made async so the flush can be
// awaited to completion first.
async function bootFromFile(name, bytes, opts) {
  const volumes = d88Volumes(bytes);
  if (volumes.length === 0) {
    statusEl.textContent = "NOT A D88 IMAGE";
    return;
  }
  await flushDiskPersist(0);
  if (volumes.length > 1) await flushDiskPersist(1);
  drives[0] = { name, volumes, current: 0 };
  if (volumes.length > 1) drives[1] = { name, volumes, current: 1 };
  if (!opts || !opts.noSave) {
    saveDriveToStore(0, name, bytes, 0);
    if (volumes.length > 1) saveDriveToStore(1, name, bytes, 1);
  }
  await coldBoot();
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

  // core stderr = diagnostics (unimplemented-port notes etc.), not errors;
  // keep them out of the browser's error channel
  Module = await createMZ2500({ printErr: (t) => console.log("[core]", t) });
  Module._emu_init(audioCtx.sampleRate);
  applyHwOptionsToMachine();
  await applyRomsToMachine();

  // restore disks saved in this browser (IndexedDB); FD1 boots in place of
  // the bundled demo, FD2 is remounted alongside. Disks chosen while the
  // power was still off are already in drives[] and take precedence.
  const saved0 = await loadDriveFromStore(0);
  const saved1 = await loadDriveFromStore(1);
  if (drives[1]) {
    pushVolume(1);
  } else if (saved1) {
    await insertFile(1, saved1.name, new Uint8Array(saved1.buffer),
                      { noSave: true, current: saved1.current });
  }
  if (!drives[0] && saved0) {
    const bytes = new Uint8Array(saved0.buffer);
    const volumes = d88Volumes(bytes);
    if (volumes.length > 0)
      drives[0] = { name: saved0.name, volumes, current: clampVolumeIndex(saved0.current, volumes.length) };
  }
  if (drives[0]) {
    coldBoot();
    return;
  }

  const resp = await fetch("neko_can_run_demo.d88?v=" + v);
  if (!resp.ok) {
    statusEl.textContent = "DISK FETCH FAILED";
    return;
  }
  // the bundled demo is not persisted; only user-inserted disks are
  await bootFromFile("neko_can_run_demo.d88", new Uint8Array(await resp.arrayBuffer()),
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
  for (const drive of [0, 1]) {
    if (Module._emu_disk_dirty(drive)) scheduleDiskPersist(drive);
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

// ---- cursor keys as the tenkey --------------------------------------------
// Software that reads 8-direction movement off the tenkey is unplayable on a
// keyboard that has none. With this on the cursor keys drive the tenkey
// positions instead, and a diagonal becomes the one key the tenkey has for
// it rather than two keys held together.
const CURSOR_DIR = {
  ArrowUp: "up", ArrowDown: "down", ArrowLeft: "left", ArrowRight: "right",
};
const TENKEY_POS = { // MZ tenkey digit -> key matrix position
  1: [2, 1], 2: [2, 2], 3: [2, 3], 4: [2, 4],
  6: [2, 6], 7: [2, 7], 8: [1, 2], 9: [1, 3],
};
const KEY_OPTIONS = [
  { id: "key-tenkey", key: "mzw_key_tenkey", def: false },
  { id: "key-diag", key: "mzw_key_diag", def: true },
  { id: "key-both", key: "mzw_key_both", def: false },
];
const keyOpt = {};
for (const o of KEY_OPTIONS) {
  const el = document.getElementById(o.id);
  const saved = localStorage.getItem(o.key);
  el.checked = saved === null ? o.def : saved === "1";
  keyOpt[o.id] = el.checked;
  el.addEventListener("change", () => {
    keyOpt[o.id] = el.checked;
    localStorage.setItem(o.key, el.checked ? "1" : "0");
    if (o.id === "key-tenkey") refreshTenkeyButton();
    applyTenkey();
  });
}

const cursorHeld = { up: false, down: false, left: false, right: false };
let tenkeyDown = new Set();

function tenkeyWanted() {
  if (!keyOpt["key-tenkey"]) return [];
  const { up, down, left, right } = cursorHeld;
  if (keyOpt["key-diag"]) {
    if (up && left) return [7];
    if (up && right) return [9];
    if (down && left) return [1];
    if (down && right) return [3];
  }
  const out = [];
  if (up) out.push(8);
  if (down) out.push(2);
  if (left) out.push(4);
  if (right) out.push(6);
  return out;
}

function applyTenkey() {
  const want = new Set(tenkeyWanted());
  keySources.tenkey.clear();
  for (const d of want) keySources.tenkey.set(cellId(TENKEY_POS[d]), TENKEY_POS[d]);
  tenkeyDown = want;
  syncKeys();
}

const tenkeyBtn = document.getElementById("tenkey-btn");
function refreshTenkeyButton() {
  const on = keyOpt["key-tenkey"];
  tenkeyBtn.setAttribute("aria-pressed", on ? "true" : "false");
  tenkeyBtn.classList.toggle("on", on);
}
tenkeyBtn.addEventListener("click", () => {
  const el = document.getElementById("key-tenkey");
  el.checked = !el.checked;
  el.dispatchEvent(new Event("change"));
});
refreshTenkeyButton();

// ---- input ----------------------------------------------------------------
// Matrix positions this page has pressed and not yet released. The browser
// stops delivering keyup the moment the window loses focus, so a key that is
// still down when the user switches to another application - to fetch the
// next disk image, say - is released into that application and never here,
// and the machine goes on seeing it held for the rest of the session. That
// outlives a RESET, because a reboot resets the machine, not the host's
// keyboard, so freshly booted software reads the stuck key too. Software
// that reads a key held at startup as a hardware option - "hold SHIFT to run
// without the 4096-colour palette board" is a common one - then comes up in
// the wrong mode, which is exactly what a disk swapped in by drag and drop
// looked like. Releasing everything once the page can no longer observe a
// release keeps the matrix honest.
// Three independent things can hold the same matrix cell down at once: the
// physical keyboard, the cursor->tenkey translation above, and the on-screen
// keyboard (a latched SHIFT there while the real SHIFT key is held is the
// obvious case). So none of them talks to the core directly - the machine
// sees the UNION, and a cell is released only once the last holder lets go.
// Driving _emu_key from each source separately would let the first release
// clear a cell somebody else is still pressing.
const keySources = { phys: new Map(), tenkey: new Map(), vkbd: new Map() };
let appliedKeys = new Map(); // cells currently pressed in the core

function cellId(pos) { return pos[0] + "," + pos[1]; }

function syncKeys() {
  if (!Module) { appliedKeys = new Map(); return; }
  const want = new Map();
  for (const src of Object.values(keySources))
    for (const [id, pos] of src) want.set(id, pos);
  for (const [id, pos] of appliedKeys)
    if (!want.has(id)) Module._emu_key(pos[0], pos[1], 0);
  for (const [id, pos] of want)
    if (!appliedKeys.has(id)) Module._emu_key(pos[0], pos[1], 1);
  appliedKeys = want;
}

function setKey(source, pos, down) {
  if (down) keySources[source].set(cellId(pos), pos);
  else keySources[source].delete(cellId(pos));
  syncKeys();
}

function releaseAllKeys() {
  keySources.phys.clear();
  keySources.tenkey.clear();
  // The on-screen keyboard's momentary keys go too - a pointer that leaves
  // for another window never delivers its pointerup here either. Its
  // latched modifiers stay: they are deliberate, visible on screen, and the
  // user turns them off by clicking them again.
  releaseVkbdMomentary();
  tenkeyDown = new Set();
  for (const dir of Object.keys(cursorHeld)) cursorHeld[dir] = false;
  syncKeys();
  if (Module) Module._emu_joy(0);
}
window.addEventListener("blur", releaseAllKeys);

const MZ_ESC = [10, 5];

function handleKey(e, down) {
  // A field on the page owns its own keystrokes. Without this the machine
  // swallowed them and preventDefault kept them out of the field entirely,
  // which is why the debug watch box could not be typed into.
  const el = e.target;
  if (el && (el.tagName === "INPUT" || el.tagName === "TEXTAREA" ||
             el.tagName === "SELECT" || el.isContentEditable)) return;
  // Esc releases the pointer lock; the browser eats that keypress, and the
  // one that ends a lock should not reach the machine either
  if (e.code === "Escape" && document.pointerLockElement) return;
  // ...which leaves the machine's own ESC unreachable for as long as the
  // mouse is captured: the browser reserves Esc and cannot be talked out of
  // it, and a captured pointer cannot click the on-screen keyboard either.
  // Option/Alt stands in for ESC for exactly that window. Outside the lock
  // it stays unbound and Esc itself is untouched, so nothing that already
  // worked moves.
  if ((e.code === "AltLeft" || e.code === "AltRight") && document.pointerLockElement) {
    if (!Module || !running) return;
    setKey("phys", MZ_ESC, down);
    e.preventDefault();
    return;
  }
  if (!Module || !running) return;
  const dir = CURSOR_DIR[e.code];
  if (dir && keyOpt["key-tenkey"]) {
    cursorHeld[dir] = down;
    applyTenkey();
    if (!keyOpt["key-both"]) {
      e.preventDefault();
      return;
    }
  } else if (dir) {
    cursorHeld[dir] = down;
  }
  const pos = KEYMAP.get(e.code);
  if (pos) {
    setKey("phys", pos, down);
    e.preventDefault();
  }
}
document.addEventListener("keydown", (e) => handleKey(e, true));
document.addEventListener("keyup", (e) => handleKey(e, false));

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
    const t1 = pad.buttons[padAssign.trig1], t2 = pad.buttons[padAssign.trig2];
    if (t1 && t1.pressed) mask |= JOY_TRIG1;
    if (t2 && t2.pressed) mask |= JOY_TRIG2;
    break;
  }
  if (Module) Module._emu_joy(mask);
}

// ---- flush pending disk writes before the page goes away ------------------
// A write inside the 1s debounce window would otherwise never reach
// IndexedDB on reload/close. pagehide (and visibilitychange->hidden) rather
// than beforeunload, which mobile Safari does not reliably fire. Each flush
// does its snapshot + IndexedDB put immediately - no further scheduling -
// since there is no guarantee the page gets another tick afterwards.
function flushAllDiskPersists() {
  flushDiskPersist(0);
  flushDiskPersist(1);
}
window.addEventListener("pagehide", flushAllDiskPersists);

// ---- pause when hidden ----------------------------------------------------
document.addEventListener("visibilitychange", () => {
  // A tab that goes away mid-keystroke never sees the keyup either, and on
  // mobile there is no window blur to lean on.
  if (document.hidden) releaseAllKeys();
  if (document.hidden) flushAllDiskPersists();
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
    fileEls[drive].click(); // works powered off too: the disk mounts at POWER ON
  });
  fileEls[drive].addEventListener("change", async () => {
    const file = fileEls[drive].files[0];
    fileEls[drive].value = "";
    if (!file) return;
    await insertFile(drive, file.name, new Uint8Array(await file.arrayBuffer()));
  });
  volEls[drive].addEventListener("change", () => {
    const d = drives[drive];
    if (!d) return;
    const newCurrent = parseInt(volEls[drive].value, 10) || 0;
    // Land any pending write on the volume it belongs to BEFORE switching -
    // pushVolume() reloads the core's disk and clears its dirty flag, so a
    // write still sitting in the debounce window would otherwise be lost,
    // and the stale timer would later snapshot the NEW volume in its place.
    flushDiskPersist(drive).then(() => {
      d.current = newCurrent;
      pushVolume(drive); // hot swap to the chosen volume
      loadDriveFromStore(drive).then((saved) => {
        if (saved && saved.name === d.name)
          saveDriveToStore(drive, saved.name, new Uint8Array(saved.buffer), d.current);
      });
    });
  });
  document.getElementById("eject" + drive).addEventListener("click", () => {
    const hadDisk = !!drives[drive];
    // Cancel any pending persist too - otherwise it fires ~1s later and
    // writes the just-ejected disk right back into IndexedDB.
    if (diskSaveTimers[drive]) {
      clearTimeout(diskSaveTimers[drive]);
      diskSaveTimers[drive] = null;
    }
    // Detach the drive and clear the core's dirty flag so there is nothing
    // left for the per-frame poll (or a pagehide flush) to act on. Without
    // this, the poll sees dirty_ still set on the very next frame, arms a
    // fresh timer, and persistDisk() - finding drives[drive] still
    // populated - writes the just-ejected disk straight back into
    // IndexedDB a second later.
    drives[drive] = null;
    if (Module) Module._emu_disk_clear_dirty(drive);
    clearDriveStore(drive);
    // Bring the UI (name, volume selector, WP indicator) to the same
    // "(empty)" state used for a drive that was never loaded. The core's
    // FDC still has no "eject" entry point to tell it the media is gone
    // (only insert/snapshot/wp/dirty exports exist), so this is JS-side
    // bookkeeping only - see the report for details.
    refreshDriveUI(drive);
    document.getElementById(`wp${drive}`).setAttribute("aria-pressed", "false");
    if (hadDisk) dnameEls[drive].textContent = "(empty) (保存消去)";
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
    if (!file) return;
    await insertFile(drive, file.name, new Uint8Array(await file.arrayBuffer()));
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
  await bootFromFile(file.name, new Uint8Array(await file.arrayBuffer()));
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

// ---- mouse (MZ-1X10) -------------------------------------------------------
// The browser reports movement at a resolution and rate the 1985 mouse never
// had, so the host side scales it. The machine's own ratio (BASIC
// `mouse 3,...`) is the software's setting and is left alone.
const mouseSensEl = document.getElementById("mouse-sens");
const mouseSensValEl = document.getElementById("mouse-sens-val");
const mouseBtn = document.getElementById("mouse-btn");
let mouseSens = parseFloat(localStorage.getItem("mzw_mouse_sens") || "1") || 1;
let mouseEnabled = localStorage.getItem("mzw_mouse_on") !== "0";
// fractional remainder, so slow movement is not rounded away
let mouseFracX = 0;
let mouseFracY = 0;

function refreshMouseUI() {
  mouseSensEl.value = String(Math.round(mouseSens * 10));
  mouseSensValEl.textContent = mouseSens.toFixed(1);
  mouseBtn.setAttribute("aria-pressed", mouseEnabled ? "true" : "false");
}
refreshMouseUI();

mouseSensEl.addEventListener("input", () => {
  mouseSens = parseInt(mouseSensEl.value, 10) / 10;
  localStorage.setItem("mzw_mouse_sens", String(mouseSens));
  refreshMouseUI();
});

mouseBtn.addEventListener("click", () => {
  mouseEnabled = !mouseEnabled;
  localStorage.setItem("mzw_mouse_on", mouseEnabled ? "1" : "0");
  if (!mouseEnabled && document.pointerLockElement) document.exitPointerLock();
  refreshMouseUI();
});

// reuse the existing screen canvas rather than a second lookup
canvas.addEventListener("click", () => {
  if (mouseEnabled && !document.pointerLockElement) canvas.requestPointerLock();
});

document.addEventListener("mousemove", (e) => {
  if (!Module || document.pointerLockElement !== canvas) return;
  mouseFracX += e.movementX * mouseSens;
  mouseFracY += e.movementY * mouseSens;
  const dx = Math.trunc(mouseFracX);
  const dy = Math.trunc(mouseFracY);
  mouseFracX -= dx;
  mouseFracY -= dy;
  if (dx || dy) Module._emu_mouse_motion(dx, dy);
});

for (const type of ["mousedown", "mouseup"]) {
  document.addEventListener(type, (e) => {
    if (!Module || document.pointerLockElement !== canvas) return;
    // Host button 0 (left) -> MZ button 1, host button 2 (right) -> MZ
    // button 2. Host button 1 is the MIDDLE button, not a second MZ button -
    // e.button values map left/middle/right to 0/1/2, so passing e.button
    // straight through (as this used to) sent the middle button's clicks to
    // MZ button 2 and left the right button, the one users actually reach
    // for, doing nothing. Anything else (middle, back/forward) is not wired
    // to anything.
    let mzButton;
    if (e.button === 0) mzButton = 0;
    else if (e.button === 2) mzButton = 1;
    else return;
    e.preventDefault();
    Module._emu_mouse_button(mzButton, type === "mousedown" ? 1 : 0);
  });
}

// The right mouse button now drives MZ button 2 while the pointer is
// locked to the screen, so its native context menu must not pop up over the
// emulator - a right-click there should reach the machine, not the browser.
document.addEventListener("contextmenu", (e) => {
  if (document.pointerLockElement === canvas) e.preventDefault();
});

document.addEventListener("pointerlockchange", () => {
  mouseBtn.setAttribute("aria-pressed",
    document.pointerLockElement === canvas ? "true" : String(mouseEnabled));
});

// ---- on-screen keyboard ----------------------------------------------------
// A real MZ-2500 keyboard has eleven keys no PC keyboard has a place for -
// GRAPH, KANA, LOCK, 変換, 無変換, HELP, COPY, HOME/CLR, BREAK, ALGO and the
// tenkey's own * and / - and binding each of them to some leftover host key
// only moves the problem. So the whole keyboard is drawn instead, laid out
// as the machine has it (MZ2500_UserManual.pdf 2-2, the keyboard photograph
// on p18), and every cap carries the matrix position it closes.
//
// Positions come from the key matrix table in MZ2500_IO_Map.pdf (port
// E8h/EAh, strobe rows 0-13, D7..D0), cross-checked cell by cell against
// BASIC-M25's own echo. Two cells the IO map leaves blank are filled in from
// that check: (1,7) is the tenkey minus (it echoes "-") and (13,0) is ALGO
// (it pops up the icon menu). (9,7) stays out: it is blank in the IO map and
// echoes nothing, so no key sits there. That leaves 96 cells carrying a key
// cap, and the layout below reaches every one of them; the other 16 of the
// matrix's 112 are cells the machine has no key for.
//
// mod: latching. SHIFT/CTRL/GRAPH/KANA/LOCK stay down until clicked again so
// that a one-pointer user can do SHIFT+BREAK or GRAPH+key at all. Everything
// else is momentary. w: key width in units of one standard cap.
const VKBD_BLOCKS = [
  { cls: "vk-main", rows: [
    [{ l: "アルゴ", p: [13, 0], w: 1.25 },
     { l: "F1", p: [0, 0] }, { l: "F2", p: [0, 1] }, { l: "F3", p: [0, 2] },
     { l: "F4", p: [0, 3] }, { l: "F5", p: [0, 4] },
     { sp: 0.5 },
     { l: "F6", p: [0, 5] }, { l: "F7", p: [0, 6] }, { l: "F8", p: [0, 7] },
     { l: "F9", p: [1, 0] }, { l: "F10", p: [1, 1] }, { l: "HELP", p: [13, 1] },
     { sp: 0.5 },
     { l: "COPY", p: [10, 1] }, { l: "BREAK", p: [3, 7], w: 1.5 },
     { sp: 0.25 }],
    [{ l: "ESC", p: [10, 5], w: 1.5 },
     { l: "1", p: [8, 1] }, { l: "2", p: [8, 2] }, { l: "3", p: [8, 3] },
     { l: "4", p: [8, 4] }, { l: "5", p: [8, 5] }, { l: "6", p: [8, 6] },
     { l: "7", p: [8, 7] }, { l: "8", p: [9, 0] }, { l: "9", p: [9, 1] },
     { l: "0", p: [8, 0] }, { l: "−", p: [9, 4] }, { l: "^", p: [7, 3] },
     { l: "¥", p: [7, 4] },
     { l: "⇦", p: [10, 4], w: 1.5, t: "BS" }],
    [{ l: "TAB", p: [3, 0], w: 1.5 },
     { l: "Q", p: [6, 1] }, { l: "W", p: [6, 7] }, { l: "E", p: [4, 5] },
     { l: "R", p: [6, 2] }, { l: "T", p: [6, 4] }, { l: "Y", p: [7, 1] },
     { l: "U", p: [6, 5] }, { l: "I", p: [5, 1] }, { l: "O", p: [5, 7] },
     { l: "P", p: [6, 0] }, { l: "@", p: [9, 5] }, { l: "[", p: [9, 6] },
     { l: "↵", p: [3, 2], w: 2.5, t: "CR" }],
    [{ l: "CTRL", p: [11, 4], w: 1.5, mod: true },
     { l: "LOCK", p: [11, 1], w: 1.5, mod: true },
     { l: "A", p: [4, 1] }, { l: "S", p: [6, 3] }, { l: "D", p: [4, 4] },
     { l: "F", p: [4, 6] }, { l: "G", p: [4, 7] }, { l: "H", p: [5, 0] },
     { l: "J", p: [5, 2] }, { l: "K", p: [5, 3] }, { l: "L", p: [5, 4] },
     { l: ";", p: [9, 3] }, { l: ":", p: [9, 2] }, { l: "]", p: [10, 0] },
     { sp: 1 }],
    [{ l: "SHIFT", p: [11, 2], w: 2, mod: true },
     { l: "Z", p: [7, 2] }, { l: "X", p: [7, 0] }, { l: "C", p: [4, 3] },
     { l: "V", p: [6, 6] }, { l: "B", p: [4, 2] }, { l: "N", p: [5, 6] },
     { l: "M", p: [5, 5] }, { l: ",", p: [7, 7] }, { l: ".", p: [7, 6] },
     { l: "/", p: [4, 0] }, { l: "_", p: [7, 5] },
     { l: "SHIFT", p: [11, 2], w: 3, mod: true }],
    [{ sp: 1 },
     { l: "GRAPH", p: [11, 0], w: 1.5, mod: true },
     { l: "無変換", p: [12, 0], w: 1.75 },
     { l: "SPACE", p: [3, 1], w: 7.5 },
     { l: "変換", p: [12, 1], w: 1.75 },
     { l: "カナ", p: [11, 3], w: 1.5, mod: true },
     { sp: 1 }],
  ] },
  { cls: "vk-tenkey", rows: [
    [{ l: "INST\nDEL", p: [10, 3] }, { l: "↑", p: [3, 3] },
     { l: "CLR\nHOME", p: [10, 2] }, { l: "*", p: [10, 6] }],
    [{ l: "←", p: [3, 5] }, { l: "↓", p: [3, 4] },
     { l: "→", p: [3, 6] }, { l: "/", p: [10, 7] }],
    [{ l: "7", p: [2, 7] }, { l: "8", p: [1, 2] }, { l: "9", p: [1, 3] },
     { l: "+", p: [1, 6] }],
    [{ l: "4", p: [2, 4] }, { l: "5", p: [2, 5] }, { l: "6", p: [2, 6] },
     { l: "−", p: [1, 7] }],
    [{ l: "1", p: [2, 1] }, { l: "2", p: [2, 2] }, { l: "3", p: [2, 3] },
     { l: "↵", p: [3, 2], t: "CR" }],
    [{ l: "0", p: [2, 0] }, { l: ",", p: [1, 4] }, { l: ".", p: [1, 5] },
     { sp: 1 }],
  ] },
];

const vkbdEl = document.getElementById("vkbd");
const vkbdBtn = document.getElementById("vkbd-btn");
const vkbdLatched = new Set();          // cell ids held by a latched modifier
const vkbdMomentary = new Set();        // cell ids held by a pointer right now
const vkbdButtons = new Map();          // cell id -> [button, ...] (SHIFT has two)

// Release everything a pointer is holding but leave the latches alone.
// Declared as a function so releaseAllKeys(), defined earlier, can call it.
function releaseVkbdMomentary() {
  for (const id of vkbdMomentary) {
    if (!vkbdLatched.has(id)) keySources.vkbd.delete(id);
    for (const btn of vkbdButtons.get(id) || []) btn.classList.remove("down");
  }
  vkbdMomentary.clear();
  syncKeys();
}

// Closing the panel must not leave a latched modifier held: an invisible
// SHIFT stuck down is exactly the bug this project has already been bitten by.
function releaseVkbdAll() {
  releaseVkbdMomentary();
  for (const id of vkbdLatched) {
    keySources.vkbd.delete(id);
    for (const btn of vkbdButtons.get(id) || []) {
      btn.classList.remove("down");
      btn.setAttribute("aria-pressed", "false");
    }
  }
  vkbdLatched.clear();
  syncKeys();
}

function vkbdSetHeld(key, held) {
  const id = cellId(key.p);
  if (held) vkbdMomentary.add(id);
  else vkbdMomentary.delete(id);
  const on = held || vkbdLatched.has(id);
  for (const btn of vkbdButtons.get(id) || []) btn.classList.toggle("down", on);
  setKey("vkbd", key.p, on);
}

function vkbdToggleLatch(key) {
  const id = cellId(key.p);
  const on = !vkbdLatched.has(id);
  if (on) vkbdLatched.add(id); else vkbdLatched.delete(id);
  for (const btn of vkbdButtons.get(id) || []) {
    btn.setAttribute("aria-pressed", on ? "true" : "false");
    btn.classList.toggle("down", on);
  }
  setKey("vkbd", key.p, on);
}

function buildVkbd() {
  for (const block of VKBD_BLOCKS) {
    const blockEl = document.createElement("div");
    blockEl.className = "vk-block " + block.cls;
    for (const row of block.rows) {
      const rowEl = document.createElement("div");
      rowEl.className = "vk-row";
      for (const key of row) {
        const w = key.w || key.sp || 1;
        // gap is 3px, so an n-wide cap also swallows the n-1 gaps it spans
        const width = `calc(var(--ku) * ${w} + 3px * ${w - 1})`;
        if (key.sp) {
          const spacer = document.createElement("span");
          spacer.className = "vk-gap";
          spacer.style.width = width;
          rowEl.appendChild(spacer);
          continue;
        }
        const btn = document.createElement("button");
        btn.type = "button";
        btn.className = "vk" + (key.mod ? " vk-mod" : "");
        btn.style.width = width;
        btn.textContent = key.l;
        btn.dataset.pos = cellId(key.p);
        btn.title = `${key.t || key.l.replace("\n", "/")}  (row ${key.p[0]}, bit ${key.p[1]})`;
        if (key.mod) btn.setAttribute("aria-pressed", "false");
        if (!vkbdButtons.has(btn.dataset.pos)) vkbdButtons.set(btn.dataset.pos, []);
        vkbdButtons.get(btn.dataset.pos).push(btn);

        if (key.mod) {
          btn.addEventListener("pointerdown", (e) => {
            e.preventDefault();   // never move focus off the page
            vkbdToggleLatch(key);
          });
        } else {
          // Momentary, and released by whichever of these comes first. The
          // pointer is deliberately NOT captured: leaving the cap has to
          // release the key, so that dragging off one cannot leave the
          // matrix holding it forever.
          btn.addEventListener("pointerdown", (e) => {
            e.preventDefault();
            vkbdSetHeld(key, true);
          });
          for (const type of ["pointerup", "pointerleave", "pointercancel"])
            btn.addEventListener(type, () => vkbdSetHeld(key, false));
        }
        // A pointer press is handled above and ALSO raises a click, which
        // would send the key a second time. detail is the click count for a
        // pointer and 0 only when the button was activated some other way -
        // a focused cap driven from the keyboard - which is the one case
        // that still needs a pulse of its own.
        btn.addEventListener("click", (e) => {
          if (e.detail !== 0) return;
          if (key.mod) { vkbdToggleLatch(key); return; }
          vkbdSetHeld(key, true);
          setTimeout(() => vkbdSetHeld(key, false), 80);
        });
        rowEl.appendChild(btn);
      }
      blockEl.appendChild(rowEl);
    }
    vkbdEl.appendChild(blockEl);
  }
  // a long press or right-click on a cap should not raise the host menu
  vkbdEl.addEventListener("contextmenu", (e) => e.preventDefault());
}
buildVkbd();

let vkbdOpen = localStorage.getItem("mzw_vkbd") === "1";
function refreshVkbd() {
  vkbdEl.hidden = !vkbdOpen;
  vkbdBtn.setAttribute("aria-pressed", vkbdOpen ? "true" : "false");
  vkbdBtn.classList.toggle("on", vkbdOpen);
}
vkbdBtn.addEventListener("click", () => {
  vkbdOpen = !vkbdOpen;
  localStorage.setItem("mzw_vkbd", vkbdOpen ? "1" : "0");
  if (!vkbdOpen) releaseVkbdAll();
  refreshVkbd();
});
refreshVkbd();
