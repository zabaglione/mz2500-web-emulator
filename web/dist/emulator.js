// Frontend: loads the wasm core and paces it off the AUDIO CLOCK - frames
// are emulated only while the audio queue sits below a small target depth,
// so audible latency stays pinned near TARGET_MS instead of accumulating.
// (The worklet additionally hard-caps its queue and drops the oldest
// samples, so delay is bounded even if this estimate ever drifts.)
//
// Two floppy drives are exposed (FD1/FD2, matching the real cabinet):
// per-drive hot insert never resets the machine, IPL reboots from FD1,
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
const diskFilenameEls = [document.getElementById("dfilename0"), document.getElementById("dfilename1")];
const diskTitleEls = [document.getElementById("dtitle0"), document.getElementById("dtitle1")];
const volEls = [document.getElementById("vol0"), document.getElementById("vol1")];
const fileEls = [document.getElementById("file0"), document.getElementById("file1")];
const masterVolumeEl = document.getElementById("master-volume");
const driveBayEl = document.getElementById("drivebay");
const panelDetailBtn = document.getElementById("panel-detail-btn");
const nekoDemoBtn = document.getElementById("neko-demo-btn");

let panelDetailed = localStorage.getItem("mzw_panel_detail") === "1";

function refreshPanelDetail() {
  driveBayEl.classList.toggle("compact", !panelDetailed);
  panelDetailBtn.setAttribute("aria-pressed", panelDetailed ? "true" : "false");
}

panelDetailBtn.addEventListener("click", () => {
  panelDetailed = !panelDetailed;
  localStorage.setItem("mzw_panel_detail", panelDetailed ? "1" : "0");
  refreshPanelDetail();
});
refreshPanelDetail();

let Module = null;
let audioCtx = null;
let workletNode = null;
let masterGainNode = null;
let running = false;

const savedMasterVolumeText = localStorage.getItem("mzw_master_volume");
const savedMasterVolume = savedMasterVolumeText === null ? 1 : Number(savedMasterVolumeText);
masterVolumeEl.value = Number.isFinite(savedMasterVolume)
  ? String(Math.min(1, Math.max(0, savedMasterVolume))) : "1";

function applyMasterVolume() {
  const value = Number(masterVolumeEl.value);
  if (masterGainNode && audioCtx)
    masterGainNode.gain.setTargetAtTime(value, audioCtx.currentTime, 0.01);
}

masterVolumeEl.addEventListener("input", () => {
  localStorage.setItem("mzw_master_volume", masterVolumeEl.value);
  applyMasterVolume();
});

// audio-clock bookkeeping: samples handed to the worklet vs. samples the
// hardware has consumed (audioCtx.currentTime is the consumption clock)
let produced = 0;
let audioT0 = 0;
let underruns = 0;
let dropped = 0;
let lastReportedQueued = 0;

let framesShown = 0;
let fpsWindowStart = 0;

// ---- RS-232C / Z80 SIO ---------------------------------------------------
// The core owns register timing, the three-byte receive FIFO, interrupts and
// the CDh clock divider. This layer only supplies a transport: a local
// terminal, a timed loopback, or a user-selected Web Serial adapter.
const sioChannelEl = document.getElementById("sio-channel");
const sioTransportEl = document.getElementById("sio-transport");
const sioConnectEl = document.getElementById("sio-connect");
const sioConnectionEl = document.getElementById("sio-connection");
const sioLineEl = document.getElementById("sio-line");
const sioTerminalEl = document.getElementById("sio-terminal");
const sioInputEl = document.getElementById("sio-input");
const sioSendEl = document.getElementById("sio-send");
const sioAppendCrEl = document.getElementById("sio-append-cr");
const sioClearEl = document.getElementById("sio-clear");
const sioSaveEl = document.getElementById("sio-save");
const ioBayEl = document.getElementById("io-bay");
const sioIoPanelEl = document.getElementById("sio-io-panel");
const printerIoPanelEl = document.getElementById("printer-io-panel");
const sioIoToggleEl = document.getElementById("sio-io-toggle");
const printerIoToggleEl = document.getElementById("printer-io-toggle");
const sioIoCloseEl = document.getElementById("sio-io-close");
const printerIoCloseEl = document.getElementById("printer-io-close");

function setIoPanelVisible(kind, visible) {
  const serial = kind === "sio";
  const panel = serial ? sioIoPanelEl : printerIoPanelEl;
  const toggle = serial ? sioIoToggleEl : printerIoToggleEl;
  panel.hidden = !visible;
  toggle.setAttribute("aria-expanded", visible ? "true" : "false");
  toggle.textContent = visible
    ? (serial ? "HIDE I/O" : "HIDE OUTPUT")
    : (serial ? "OPEN I/O" : "OPEN OUTPUT");
  ioBayEl.hidden = sioIoPanelEl.hidden && printerIoPanelEl.hidden;
}

sioIoToggleEl.addEventListener("click", () => {
  setIoPanelVisible("sio", sioIoPanelEl.hidden);
});
printerIoToggleEl.addEventListener("click", () => {
  setIoPanelVisible("printer", printerIoPanelEl.hidden);
});
sioIoCloseEl.addEventListener("click", () => setIoPanelVisible("sio", false));
printerIoCloseEl.addEventListener("click", () => setIoPanelVisible("printer", false));

const SIO_CAPTURE_LIMIT = 1024 * 1024;
const SIO_RENDER_LIMIT = 16 * 1024;
const sioCapture = [[], []];
const sioCaptureDropped = [0, 0];
let sioTerminalDirty = true;
let sioAppliedModem = [null, null];

let webSerialPort = null;
let webSerialChannel = -1;
let webSerialReader = null;
let webSerialWriter = null;
let webSerialReadTask = null;
let webSerialOpenSignature = "";
let webSerialWriteChain = Promise.resolve();
let webSerialSignalChain = Promise.resolve();
let webSerialSignalPollBusy = false;
let webSerialLastSignalPoll = 0;
let webSerialLastOutputs = "";
let webSerialInputs = { cts: true, dcd: true };
let webSerialDisconnecting = false;

function selectedSioChannel() {
  return parseInt(sioChannelEl.value, 10) & 1;
}

function sioLineConfig(channel) {
  if (!Module) return null;
  return {
    baud: Module._emu_sio_baud(channel),
    rxBits: Module._emu_sio_rx_bits(channel),
    txBits: Module._emu_sio_tx_bits(channel),
    stopHalfBits: Module._emu_sio_stop_half_bits(channel),
    parity: Module._emu_sio_parity(channel),
    rxEnabled: Module._emu_sio_rx_enabled(channel) !== 0,
    txEnabled: Module._emu_sio_tx_enabled(channel) !== 0,
    routed: Module._emu_sio_rs232_connected(channel) !== 0,
    dtr: Module._emu_sio_dtr(channel) !== 0,
    rts: Module._emu_sio_rts(channel) !== 0,
    breakOn: Module._emu_sio_break(channel) !== 0,
  };
}

function parityLabel(parity) {
  return parity === 1 ? "O" : parity === 2 ? "E" : "N";
}

function stopBitsLabel(halfBits) {
  if (halfBits === 2) return "1";
  if (halfBits === 3) return "1.5";
  if (halfBits === 4) return "2";
  return "sync";
}

function sioSignature(config) {
  return config ? [config.baud, config.rxBits, config.txBits,
                    config.parity, config.stopHalfBits].join(":") : "";
}

function webSerialOptions(config) {
  if (!config || !config.routed) return { error: "RS-232C path is not selected" };
  if (!config.baud) return { error: "SIO clock is stopped" };
  if (config.rxBits !== config.txBits)
    return { error: "Web Serial needs equal RX and TX data bits" };
  if (config.rxBits !== 7 && config.rxBits !== 8)
    return { error: "Web Serial supports only 7 or 8 data bits" };
  if (config.stopHalfBits !== 2 && config.stopHalfBits !== 4)
    return { error: "Web Serial supports only 1 or 2 stop bits" };
  return {
    options: {
      baudRate: config.baud,
      dataBits: config.rxBits,
      stopBits: config.stopHalfBits / 2,
      parity: config.parity === 1 ? "odd" : config.parity === 2 ? "even" : "none",
      flowControl: "none",
    },
  };
}

function setSioConnection(text, error = false) {
  sioConnectionEl.textContent = text;
  sioConnectionEl.classList.toggle("error", error);
}

function desiredSioModem(channel) {
  const selected = selectedSioChannel();
  const config = sioLineConfig(channel);
  if (!config || channel !== selected || !config.routed)
    return { cts: false, dcd: false };

  if (sioTransportEl.value === "terminal" || sioTransportEl.value === "loopback")
    return { cts: true, dcd: true };
  if (webSerialPort && webSerialChannel === channel) return webSerialInputs;
  return { cts: false, dcd: false };
}

function syncSioModemInputs() {
  if (!Module) return;
  for (let channel = 0; channel < 2; channel++) {
    const desired = desiredSioModem(channel);
    const signature = `${desired.cts ? 1 : 0}:${desired.dcd ? 1 : 0}`;
    if (sioAppliedModem[channel] === signature) continue;
    Module._emu_sio_set_modem(channel, desired.cts ? 1 : 0, desired.dcd ? 1 : 0);
    sioAppliedModem[channel] = signature;
  }
}

function appendSioCapture(channel, bytes) {
  const capture = sioCapture[channel];
  for (const byte of bytes) capture.push(byte);
  if (capture.length > SIO_CAPTURE_LIMIT) {
    const excess = capture.length - SIO_CAPTURE_LIMIT;
    capture.splice(0, excess);
    sioCaptureDropped[channel] += excess;
  }
  if (channel === selectedSioChannel()) {
    sioTerminalDirty = true;
    if (bytes.length) setIoPanelVisible("sio", true);
  }
}

function renderSioTerminal() {
  if (!sioTerminalDirty) return;
  const channel = selectedSioChannel();
  const capture = sioCapture[channel];
  const start = Math.max(0, capture.length - SIO_RENDER_LIMIT);
  let text = "";
  if (sioCaptureDropped[channel] || start)
    text = `[${sioCaptureDropped[channel] + start} earlier byte(s) hidden]\n`;
  let previousCr = false;
  for (let i = start; i < capture.length; i++) {
    const byte = capture[i];
    if (byte === 0x0D) {
      text += "\n";
      previousCr = true;
    } else if (byte === 0x0A) {
      if (!previousCr) text += "\n";
      previousCr = false;
    } else if (byte === 0x09) {
      text += "\t";
      previousCr = false;
    } else if (byte >= 0x20 && byte <= 0x7E) {
      text += String.fromCharCode(byte);
      previousCr = false;
    } else {
      text += `\\x${byte.toString(16).padStart(2, "0").toUpperCase()}`;
      previousCr = false;
    }
  }
  sioTerminalEl.value = text;
  sioTerminalEl.scrollTop = sioTerminalEl.scrollHeight;
  sioTerminalDirty = false;
}

function parseSioInput(text) {
  const bytes = [];
  for (let i = 0; i < text.length; i++) {
    let code = text.charCodeAt(i);
    if (code !== 0x5C) {
      if (code > 0xFF) throw new Error("Input is limited to single-byte characters");
      bytes.push(code);
      continue;
    }
    if (++i >= text.length) { bytes.push(0x5C); break; }
    const escape = text[i];
    if (escape === "r") bytes.push(0x0D);
    else if (escape === "n") bytes.push(0x0A);
    else if (escape === "t") bytes.push(0x09);
    else if (escape === "\\") bytes.push(0x5C);
    else if (escape === "x" && /^[0-9a-fA-F]{2}$/.test(text.slice(i + 1, i + 3))) {
      bytes.push(parseInt(text.slice(i + 1, i + 3), 16));
      i += 2;
    } else {
      code = escape.charCodeAt(0);
      if (code > 0xFF) throw new Error("Input is limited to single-byte characters");
      bytes.push(code);
    }
  }
  return bytes;
}

function injectSioBytes(channel, bytes) {
  if (!Module) return 0;
  let accepted = 0;
  for (const byte of bytes) accepted += Module._emu_sio_rx_queue(channel, byte) ? 1 : 0;
  return accepted;
}

function loopbackSioBytes(channel, bytes) {
  if (!Module) return 0;
  let accepted = 0;
  for (const byte of bytes) accepted += Module._emu_sio_rx_now(channel, byte) ? 1 : 0;
  return accepted;
}

function queueWebSerialWrite(bytes) {
  const writer = webSerialWriter;
  const port = webSerialPort;
  if (!writer || !port || bytes.length === 0) return;
  const copy = Uint8Array.from(bytes);
  webSerialWriteChain = webSerialWriteChain.then(async () => {
    if (webSerialPort === port && webSerialWriter === writer) await writer.write(copy);
  }).catch((error) => {
    if (webSerialPort === port) setSioConnection(`Write failed: ${error.message}`, true);
  });
}

function pumpSio() {
  if (!Module) return;
  const selected = selectedSioChannel();
  const mode = sioTransportEl.value;
  for (let channel = 0; channel < 2; channel++) {
    const bytes = [];
    for (;;) {
      const value = Module._emu_sio_tx_pop(channel);
      if (value < 0) break;
      bytes.push(value);
    }
    if (!bytes.length || !Module._emu_sio_rs232_connected(channel)) continue;
    appendSioCapture(channel, bytes);
    if (channel !== selected) continue;
    if (mode === "loopback") loopbackSioBytes(channel, bytes);
    else if (mode === "webserial" && webSerialPort && webSerialChannel === channel)
      queueWebSerialWrite(bytes);
  }
  renderSioTerminal();
}

async function readWebSerial(port, channel) {
  try {
    while (webSerialPort === port && port.readable) {
      const reader = port.readable.getReader();
      webSerialReader = reader;
      try {
        while (webSerialPort === port) {
          const { value, done } = await reader.read();
          if (done) break;
          if (value && Module && Module._emu_sio_rs232_connected(channel))
            injectSioBytes(channel, value);
        }
      } finally {
        if (webSerialReader === reader) webSerialReader = null;
        reader.releaseLock();
      }
    }
  } catch (error) {
    if (webSerialPort === port) setSioConnection(`Read failed: ${error.message}`, true);
  }
}

async function pollWebSerialSignals(now) {
  const port = webSerialPort;
  if (!port || webSerialSignalPollBusy || now - webSerialLastSignalPoll < 100) return;
  webSerialLastSignalPoll = now;
  webSerialSignalPollBusy = true;
  try {
    const signals = await port.getSignals();
    if (webSerialPort === port) {
      webSerialInputs = {
        cts: signals.clearToSend !== false,
        dcd: signals.dataCarrierDetect !== false,
      };
      syncSioModemInputs();
    }
  } catch (error) {
    if (webSerialPort === port) {
      webSerialInputs = { cts: true, dcd: true };
      syncSioModemInputs();
    }
  } finally {
    webSerialSignalPollBusy = false;
  }
}

function updateWebSerialOutputs(config) {
  const port = webSerialPort;
  if (!port || !config) return;
  const signature = `${config.dtr ? 1 : 0}:${config.rts ? 1 : 0}:${config.breakOn ? 1 : 0}`;
  if (signature === webSerialLastOutputs) return;
  webSerialLastOutputs = signature;
  webSerialSignalChain = webSerialSignalChain.then(async () => {
    if (webSerialPort !== port) return;
    await port.setSignals({
      dataTerminalReady: config.dtr,
      requestToSend: config.rts,
      break: config.breakOn,
    });
  }).catch((error) => {
    if (webSerialPort === port) setSioConnection(`Signal update failed: ${error.message}`, true);
  });
}

async function disconnectWebSerial(message = "Disconnected") {
  if (webSerialDisconnecting) return;
  const port = webSerialPort;
  if (!port) { setSioConnection(message); return; }
  webSerialDisconnecting = true;
  webSerialPort = null;
  webSerialChannel = -1;
  webSerialOpenSignature = "";
  webSerialLastOutputs = "";
  const reader = webSerialReader;
  try {
    if (reader) await reader.cancel();
    if (webSerialReadTask) await webSerialReadTask;
    await webSerialWriteChain;
    if (webSerialWriter) webSerialWriter.releaseLock();
    webSerialWriter = null;
    await port.close();
  } catch (error) {
    setSioConnection(`Disconnect failed: ${error.message}`, true);
  } finally {
    webSerialReadTask = null;
    webSerialReader = null;
    webSerialInputs = { cts: true, dcd: true };
    webSerialDisconnecting = false;
    sioAppliedModem = [null, null];
    syncSioModemInputs();
    updateSioUi();
    if (!sioConnectionEl.classList.contains("error")) setSioConnection(message);
  }
}

async function connectWebSerial() {
  if (webSerialPort) { await disconnectWebSerial(); return; }
  if (!Module) { setSioConnection("Power on first", true); return; }
  if (!("serial" in navigator)) {
    setSioConnection("Web Serial is unavailable in this browser", true);
    return;
  }
  const channel = selectedSioChannel();
  const config = sioLineConfig(channel);
  const result = webSerialOptions(config);
  if (result.error) { setSioConnection(result.error, true); return; }

  let port = null;
  try {
    port = await navigator.serial.requestPort();
    await port.open(result.options);
    webSerialPort = port;
    webSerialChannel = channel;
    webSerialOpenSignature = sioSignature(config);
    webSerialInputs = { cts: true, dcd: true };
    webSerialWriter = port.writable ? port.writable.getWriter() : null;
    webSerialReadTask = readWebSerial(port, channel);
    sioAppliedModem = [null, null];
    syncSioModemInputs();
    updateWebSerialOutputs(config);
    setSioConnection(`Connected to channel ${channel ? "B" : "A"}`);
    setIoPanelVisible("sio", true);
    updateSioUi();
  } catch (error) {
    if (port) {
      try { await port.close(); } catch (closeError) { /* best effort */ }
    }
    webSerialPort = null;
    webSerialChannel = -1;
    webSerialWriter = null;
    webSerialReadTask = null;
    setSioConnection(`Connection failed: ${error.message}`, true);
    updateSioUi();
  }
}

function updateSioUi(now = performance.now()) {
  const channel = selectedSioChannel();
  const config = sioLineConfig(channel);
  const mode = sioTransportEl.value;
  sioConnectEl.hidden = mode !== "webserial";
  sioConnectEl.textContent = webSerialPort ? "Disconnect" : "Connect";
  sioChannelEl.disabled = !!webSerialPort;
  sioTransportEl.disabled = !!webSerialPort;
  sioSendEl.disabled = !config || !config.routed || mode === "webserial";
  sioInputEl.disabled = sioSendEl.disabled;

  if (!config) {
    sioLineEl.textContent = "POWER OFF";
  } else {
    const name = channel ? "B" : "A";
    const route = config.routed ? "RS-232C" : "MOUSE";
    sioLineEl.textContent =
      `${name} ${config.baud} baud RX${config.rxBits}/TX${config.txBits} ` +
      `${parityLabel(config.parity)}${stopBitsLabel(config.stopHalfBits)} | ` +
      `RX ${config.rxEnabled ? "on" : "off"} TX ${config.txEnabled ? "on" : "off"} | ` +
      `DTR ${config.dtr ? 1 : 0} RTS ${config.rts ? 1 : 0} ` +
      `BRK ${config.breakOn ? 1 : 0} | ${route}`;
  }

  if (webSerialPort && config) {
    if (sioSignature(config) !== webSerialOpenSignature)
      void disconnectWebSerial("Line settings changed; reconnect");
    else {
      updateWebSerialOutputs(config);
      void pollWebSerialSignals(now);
    }
  }
  syncSioModemInputs();
  renderSioTerminal();
}

function sendSioInput() {
  const channel = selectedSioChannel();
  try {
    const bytes = parseSioInput(sioInputEl.value);
    if (sioAppendCrEl.checked) bytes.push(0x0D);
    const accepted = injectSioBytes(channel, bytes);
    if (accepted !== bytes.length)
      setSioConnection(`Accepted ${accepted} of ${bytes.length} byte(s)`, true);
    else setSioConnection(`Queued ${accepted} byte(s)`);
    sioInputEl.value = "";
  } catch (error) {
    setSioConnection(error.message, true);
  }
}

sioChannelEl.addEventListener("change", () => {
  sioTerminalDirty = true;
  sioAppliedModem = [null, null];
  setSioConnection(sioTransportEl.value === "loopback" ?
                   "Timed loopback ready" : "Virtual line ready");
  updateSioUi();
});
sioTransportEl.addEventListener("change", () => {
  sioAppliedModem = [null, null];
  if (sioTransportEl.value === "terminal") setSioConnection("Virtual line ready");
  else if (sioTransportEl.value === "loopback") setSioConnection("Timed loopback ready");
  else if ("serial" in navigator) setSioConnection("Select a physical adapter");
  else setSioConnection("Web Serial is unavailable in this browser", true);
  updateSioUi();
});
sioConnectEl.addEventListener("click", () => void connectWebSerial());
sioSendEl.addEventListener("click", sendSioInput);
sioInputEl.addEventListener("keydown", (event) => {
  if (event.key === "Enter") { event.preventDefault(); sendSioInput(); }
});
sioClearEl.addEventListener("click", () => {
  const channel = selectedSioChannel();
  sioCapture[channel].length = 0;
  sioCaptureDropped[channel] = 0;
  sioTerminalDirty = true;
  renderSioTerminal();
});
sioSaveEl.addEventListener("click", () => {
  const channel = selectedSioChannel();
  const blob = new Blob([Uint8Array.from(sioCapture[channel])],
                        { type: "application/octet-stream" });
  const link = document.createElement("a");
  link.href = URL.createObjectURL(blob);
  link.download = `mz2500-sio-${channel ? "b" : "a"}.bin`;
  link.click();
  setTimeout(() => URL.revokeObjectURL(link.href), 0);
});
if ("serial" in navigator) {
  navigator.serial.addEventListener("disconnect", (event) => {
    if (event.target === webSerialPort) void disconnectWebSerial("Adapter disconnected");
  });
}
updateSioUi();

// ---- built-in CMT data recorder -----------------------------------------
// WAV keeps arbitrary/custom pulse timing. Standard MZ logical images are
// expanded to their machine-specific comparator waveform by the core.
const cmtStateEl = document.getElementById("cmt-state");
const cmtProgressEl = document.getElementById("cmt-progress");
const cmtLoadEl = document.getElementById("cmt-load");
const cmtInsertEl = document.getElementById("cmt-insert");
const cmtNewEl = document.getElementById("cmt-new");
const cmtEjectEl = document.getElementById("cmt-eject");
const cmtWpEl = document.getElementById("cmt-wp");
const cmtSaveEl = document.getElementById("cmt-save");
const cmtFileEl = document.getElementById("cmt-file");
const cmtCommandEls = Array.from(document.querySelectorAll(".cmt-command"));
const cmtDeckEl = document.getElementById("cmt-deck");
const cmtMediaNameEl = document.getElementById("cmt-media-name");
const cmtCounterEl = document.getElementById("cmt-counter");
const cmtCounterResetEl = document.getElementById("cmt-counter-reset");
const cmtMicEl = document.getElementById("cmt-mic-toggle");
const cmtRecEl = document.getElementById("cmt-rec");
const cmtLedEls = {
  play: document.getElementById("cmt-led-play"),
  ff: document.getElementById("cmt-led-ff"),
  rew: document.getElementById("cmt-led-rew"),
  rec: document.getElementById("cmt-led-rec"),
};

let cmtMedia = null; // { name, bytes, kind: "wav"|"mzf", wp, inserted }
let cmtSaveTimer = null;
let cmtUiSignature = "";
let cmtCounterZeroMs = 0;

function formatTapeTime(ms) {
  const total = Math.max(0, Math.floor(ms / 1000));
  const minutes = Math.floor(total / 60);
  return `${String(minutes).padStart(2, "0")}:${String(total % 60).padStart(2, "0")}`;
}

function formatTapeCounter(position, duration) {
  if (duration <= 0) return "000";
  const relative = Math.round((position - cmtCounterZeroMs) * 999 / duration);
  const wrapped = ((relative % 1000) + 1000) % 1000;
  return String(wrapped).padStart(3, "0");
}

function cmtSnapshot() {
  if (!Module) return null;
  const size = Module._emu_cmt_snapshot();
  if (size <= 0) return null;
  const ptr = Module._emu_cmt_data();
  return Module.HEAPU8.slice(ptr, ptr + size);
}

function isWavTape(bytes) {
  return bytes && bytes.length >= 12 &&
    bytes[0] === 0x52 && bytes[1] === 0x49 && bytes[2] === 0x46 && bytes[3] === 0x46 &&
    bytes[8] === 0x57 && bytes[9] === 0x41 && bytes[10] === 0x56 && bytes[11] === 0x45;
}

function detectTapeKind(bytes) {
  return isWavTape(bytes) ? "wav" : "mzf";
}

function wavTapeName(name) {
  const source = name || "tape";
  const base = source.replace(/\.(?:wav|mzt|mzf|m12)$/i, "");
  return `${base || "tape"}.wav`;
}

function mountCmtMedia() {
  if (!Module || !cmtMedia || !cmtMedia.bytes.length) return false;
  const kind = detectTapeKind(cmtMedia.bytes);
  const ptr = Module._malloc(cmtMedia.bytes.length);
  Module.HEAPU8.set(cmtMedia.bytes, ptr);
  const ok = kind === "wav"
    ? Module._emu_cmt_insert_wav(ptr, cmtMedia.bytes.length) !== 0
    : Module._emu_cmt_insert_mzf(ptr, cmtMedia.bytes.length) !== 0;
  Module._free(ptr);
  if (!ok) return false;
  cmtMedia.kind = kind;
  cmtMedia.inserted = true;
  Module._emu_cmt_set_wp(cmtMedia.wp ? 1 : 0);
  saveCmtToStore(cmtMedia);
  refreshCmtUi();
  return true;
}

function persistCmt() {
  if (!Module || !cmtMedia) return;
  const dirty = Module._emu_cmt_dirty() !== 0;
  if (dirty || cmtMedia.kind === "wav") {
    const bytes = cmtSnapshot();
    if (bytes) {
      cmtMedia.bytes = bytes;
      cmtMedia.kind = "wav";
      cmtMedia.name = wavTapeName(cmtMedia.name);
    }
  }
  cmtMedia.wp = Module._emu_cmt_wp() !== 0;
  cmtMedia.inserted = Module._emu_cmt_loaded() !== 0;
  Module._emu_cmt_clear_dirty();
  return saveCmtToStore(cmtMedia);
}

function scheduleCmtPersist() {
  if (cmtSaveTimer) return;
  cmtSaveTimer = setTimeout(() => {
    cmtSaveTimer = null;
    persistCmt();
  }, 1000);
}

function flushCmtPersist() {
  if (cmtSaveTimer) {
    clearTimeout(cmtSaveTimer);
    cmtSaveTimer = null;
  }
  return Promise.resolve(persistCmt());
}

function refreshCmtUi() {
  const loaded = !!Module && Module._emu_cmt_loaded() !== 0;
  const transport = loaded ? Module._emu_cmt_transport() : 0;
  const position = Module ? Module._emu_cmt_position_ms() : 0;
  const duration = Module ? Module._emu_cmt_duration_ms() : 0;
  const wp = Module ? Module._emu_cmt_wp() !== 0 : !!(cmtMedia && cmtMedia.wp);
  const recording = !!Module && Module._emu_cmt_recording() !== 0;

  if (Module && cmtMedia && cmtMedia.inserted !== loaded) {
    cmtMedia.inserted = loaded;
    saveCmtToStore(cmtMedia);
  }
  const states = ["STOP", "PLAY", "FF", "REW"];
  const name = cmtMedia ? cmtMedia.name : "NO IMAGE";
  const state = !Module ? "POWER OFF" : loaded ? states[transport] : "EJECTED";
  const signature = [state, name, position, duration, wp, recording, cmtCounterZeroMs].join("|");
  if (signature !== cmtUiSignature) {
    cmtStateEl.textContent = `${state} | ${formatTapeTime(position)} / ${formatTapeTime(duration)}`;
    cmtMediaNameEl.textContent = cmtMedia ? name : "NO TAPE";
    cmtCounterEl.textContent = formatTapeCounter(position, duration);
    cmtProgressEl.max = Math.max(1, duration);
    cmtProgressEl.value = Math.min(position, Math.max(1, duration));
    cmtWpEl.setAttribute("aria-pressed", wp ? "true" : "false");
    cmtDeckEl.classList.toggle("powered", !!Module);
    cmtDeckEl.classList.toggle("loaded", loaded);
    cmtDeckEl.classList.toggle("transport-play", loaded && transport === 1);
    cmtDeckEl.classList.toggle("transport-ff", loaded && transport === 2);
    cmtDeckEl.classList.toggle("transport-rew", loaded && transport === 3);
    cmtLedEls.play.classList.toggle("on", loaded && transport === 1);
    cmtLedEls.ff.classList.toggle("on", loaded && transport === 2);
    cmtLedEls.rew.classList.toggle("on", loaded && transport === 3);
    cmtLedEls.rec.classList.toggle("on", recording);
    cmtRecEl.setAttribute("aria-pressed", recording ? "true" : "false");
    cmtUiSignature = signature;
  }
  cmtInsertEl.disabled = !Module || !cmtMedia || loaded;
  cmtEjectEl.disabled = !Module || !loaded;
  cmtSaveEl.disabled = !cmtMedia;
  for (const button of cmtCommandEls) {
    button.disabled = !loaded;
    button.classList.toggle("active", loaded && Number(button.dataset.command) === transport);
  }
}

cmtLoadEl.addEventListener("click", () => cmtFileEl.click());
cmtFileEl.addEventListener("change", async () => {
  const file = cmtFileEl.files[0];
  cmtFileEl.value = "";
  if (!file) return;
  const bytes = new Uint8Array(await file.arrayBuffer());
  const candidate = { name: file.name, bytes, kind: detectTapeKind(bytes),
                      wp: true, inserted: true };
  const previous = cmtMedia;
  cmtMedia = candidate;
  if (Module && !mountCmtMedia()) {
    cmtMedia = previous;
    statusEl.textContent = "CMT: UNSUPPORTED TAPE IMAGE";
    return;
  }
  cmtCounterZeroMs = 0;
  await saveCmtToStore(cmtMedia);
  refreshCmtUi();
});

cmtInsertEl.addEventListener("click", () => {
  if (!mountCmtMedia()) statusEl.textContent = "CMT: NO VALID IMAGE";
});

cmtNewEl.addEventListener("click", async () => {
  if (!Module) {
    statusEl.textContent = "POWER ON FIRST";
    return;
  }
  await flushCmtPersist();
  if (!Module._emu_cmt_create_blank(5 * 60)) {
    statusEl.textContent = "CMT: CANNOT CREATE TAPE";
    return;
  }
  cmtMedia = { name: "blank-tape.wav", bytes: cmtSnapshot(), kind: "wav",
               wp: false, inserted: true };
  cmtCounterZeroMs = 0;
  await saveCmtToStore(cmtMedia);
  refreshCmtUi();
});

cmtEjectEl.addEventListener("click", async () => {
  await flushCmtPersist();
  if (!Module) return;
  Module._emu_cmt_eject();
  if (cmtMedia) {
    cmtMedia.inserted = false;
    await saveCmtToStore(cmtMedia);
  }
  refreshCmtUi();
});

cmtWpEl.addEventListener("click", () => {
  if (!cmtMedia) return;
  cmtMedia.wp = !cmtMedia.wp;
  if (Module) Module._emu_cmt_set_wp(cmtMedia.wp ? 1 : 0);
  saveCmtToStore(cmtMedia);
  refreshCmtUi();
});

for (const button of cmtCommandEls) {
  button.addEventListener("click", () => {
    if (Module) Module._emu_cmt_command(Number(button.dataset.command));
    refreshCmtUi();
  });
}

cmtCounterResetEl.addEventListener("click", () => {
  cmtCounterZeroMs = Module ? Module._emu_cmt_position_ms() : 0;
  cmtUiSignature = "";
  refreshCmtUi();
});

cmtMicEl.addEventListener("click", () => {
  statusEl.textContent = "CMT: ANALOG MICROPHONE TRACK NOT AVAILABLE";
});

cmtRecEl.addEventListener("click", () => {
  statusEl.textContent = "CMT: ANALOG RECORD KEY NOT AVAILABLE";
});

cmtSaveEl.addEventListener("click", async () => {
  await flushCmtPersist();
  if (!cmtMedia) return;
  const isWav = cmtMedia.kind === "wav";
  const url = URL.createObjectURL(new Blob([cmtMedia.bytes], {
    type: isWav ? "audio/wav" : "application/octet-stream",
  }));
  const a = document.createElement("a");
  a.href = url;
  a.download = isWav ? wavTapeName(cmtMedia.name) : cmtMedia.name;
  a.click();
  URL.revokeObjectURL(url);
});

refreshCmtUi();

// ---- MZ-1E30 SASI hard disk --------------------------------------------
const sasiStateEl = document.getElementById("sasi-state");
const sasiBlockEl = document.getElementById("sasi-block");
const sasiTargetEl = document.getElementById("sasi-target");
const sasiNewSizeEl = document.getElementById("sasi-new-size");
const sasiLoadEl = document.getElementById("sasi-load");
const sasiInsertEl = document.getElementById("sasi-insert");
const sasiNewEl = document.getElementById("sasi-new");
const sasiWpEl = document.getElementById("sasi-wp");
const sasiSaveEl = document.getElementById("sasi-save");
const sasiEjectEl = document.getElementById("sasi-eject");
const sasiFileEl = document.getElementById("sasi-file");

let sasiMedia = null; // { name, bytes, blockSize, wp, inserted, target }
let sasiSaveTimer = null;
let sasiUiSignature = "";
sasiBlockEl.value = localStorage.getItem("mzw_sasi_block") || "0";
sasiTargetEl.value = localStorage.getItem("mzw_sasi_target") || "0";

function sasiSnapshot() {
  if (!Module) return new Uint8Array();
  const size = Module._emu_sasi_snapshot();
  if (size <= 0) return new Uint8Array();
  const ptr = Module._emu_sasi_data();
  return Module.HEAPU8.slice(ptr, ptr + size);
}

function mountSasiMedia() {
  if (!Module || !sasiMedia || !sasiMedia.bytes.length) return false;
  const ptr = Module._malloc(sasiMedia.bytes.length);
  Module.HEAPU8.set(sasiMedia.bytes, ptr);
  const requestedBlock = Number(sasiMedia.blockSize || 0);
  const ok = Module._emu_sasi_insert(ptr, sasiMedia.bytes.length, requestedBlock) !== 0;
  Module._free(ptr);
  if (!ok) return false;
  sasiMedia.blockSize = Module._emu_sasi_block_size();
  sasiMedia.inserted = true;
  Module._emu_sasi_set_wp(sasiMedia.wp ? 1 : 0);
  Module._emu_sasi_set_target(Number(sasiMedia.target || 0));
  Module._emu_sasi_clear_dirty();
  saveSasiToStore(sasiMedia);
  refreshSasiUi();
  return true;
}

function persistSasi() {
  if (!Module || !sasiMedia) return;
  if (Module._emu_sasi_loaded()) {
    const bytes = sasiSnapshot();
    if (bytes.length) sasiMedia.bytes = bytes;
    sasiMedia.blockSize = Module._emu_sasi_block_size();
    sasiMedia.wp = Module._emu_sasi_wp() !== 0;
    sasiMedia.target = Module._emu_sasi_target();
    sasiMedia.inserted = true;
  }
  Module._emu_sasi_clear_dirty();
  return saveSasiToStore(sasiMedia);
}

function scheduleSasiPersist() {
  if (sasiSaveTimer) return;
  sasiSaveTimer = setTimeout(() => {
    sasiSaveTimer = null;
    persistSasi();
  }, 1000);
}

function flushSasiPersist() {
  if (sasiSaveTimer) {
    clearTimeout(sasiSaveTimer);
    sasiSaveTimer = null;
  }
  return Promise.resolve(persistSasi());
}

function refreshSasiUi() {
  const loaded = !!Module && Module._emu_sasi_loaded() !== 0;
  const block = loaded ? Module._emu_sasi_block_size() : (sasiMedia && sasiMedia.blockSize) || 0;
  const wp = loaded ? Module._emu_sasi_wp() !== 0 : !!(sasiMedia && sasiMedia.wp);
  const target = loaded ? Module._emu_sasi_target() : Number(sasiTargetEl.value);
  const phaseNames = ["BUS FREE", "COMMAND", "DATA OUT", "DATA IN", "STATUS", "MESSAGE IN"];
  const phase = loaded ? phaseNames[Module._emu_sasi_phase()] || "UNKNOWN" : "BUS FREE";
  const name = sasiMedia ? sasiMedia.name : "NO IMAGE";
  const bytes = loaded ? Module._emu_sasi_size() : (sasiMedia ? sasiMedia.bytes.length : 0);
  const signature = `${loaded}:${name}:${bytes}:${block}:${wp}:${target}:${phase}`;
  if (signature !== sasiUiSignature) {
    sasiStateEl.textContent = Module
      ? `${loaded ? "INSERTED" : "EJECTED"} | ${name} | ${(bytes / 1048576).toFixed(2)} MiB | ` +
        `${block || "?"} B/block | ID ${target} | ${phase}`
      : "POWER OFF";
    sasiWpEl.setAttribute("aria-pressed", wp ? "true" : "false");
    sasiUiSignature = signature;
  }
  sasiInsertEl.disabled = !Module || !sasiMedia || loaded;
  sasiEjectEl.disabled = !loaded;
  sasiWpEl.disabled = !sasiMedia;
  sasiSaveEl.disabled = !sasiMedia || !sasiMedia.bytes.length;
}

sasiLoadEl.addEventListener("click", () => sasiFileEl.click());
sasiFileEl.addEventListener("change", async () => {
  const file = sasiFileEl.files[0];
  sasiFileEl.value = "";
  if (!file) return;
  await flushSasiPersist();
  const candidate = {
    name: file.name,
    bytes: new Uint8Array(await file.arrayBuffer()),
    blockSize: Number(sasiBlockEl.value),
    wp: false,
    inserted: true,
    target: Number(sasiTargetEl.value),
  };
  const previous = sasiMedia;
  sasiMedia = candidate;
  if (Module && !mountSasiMedia()) {
    sasiMedia = previous;
    statusEl.textContent = "SASI: IMAGE SIZE/BLOCK SIZE MISMATCH";
    return;
  }
  await saveSasiToStore(sasiMedia);
  refreshSasiUi();
});

sasiInsertEl.addEventListener("click", () => {
  if (!mountSasiMedia()) statusEl.textContent = "SASI: NO VALID IMAGE";
});

sasiNewEl.addEventListener("click", async () => {
  if (!Module) {
    statusEl.textContent = "POWER ON FIRST";
    return;
  }
  await flushSasiPersist();
  const [sizeText, blockText] = sasiNewSizeEl.value.split(":");
  const size = Number(sizeText), blockSize = Number(blockText);
  if (!Module._emu_sasi_create_blank(size, blockSize)) {
    statusEl.textContent = "SASI: CANNOT CREATE IMAGE";
    return;
  }
  const isMZ = size === 22437888 && blockSize === 1024;
  sasiMedia = {
    name: isMZ ? "mz1f23.hdf" : `blank-${Math.round(size / 1000000)}mb.hdf`,
    bytes: sasiSnapshot(), blockSize, wp: false, inserted: true,
    target: Number(sasiTargetEl.value),
  };
  Module._emu_sasi_set_target(sasiMedia.target);
  Module._emu_sasi_clear_dirty();
  await saveSasiToStore(sasiMedia);
  refreshSasiUi();
});

sasiEjectEl.addEventListener("click", async () => {
  await flushSasiPersist();
  if (!Module) return;
  Module._emu_sasi_eject();
  if (sasiMedia) {
    sasiMedia.inserted = false;
    await saveSasiToStore(sasiMedia);
  }
  refreshSasiUi();
});

sasiWpEl.addEventListener("click", () => {
  if (!sasiMedia) return;
  sasiMedia.wp = !sasiMedia.wp;
  if (Module && Module._emu_sasi_loaded())
    Module._emu_sasi_set_wp(sasiMedia.wp ? 1 : 0);
  saveSasiToStore(sasiMedia);
  refreshSasiUi();
});

sasiSaveEl.addEventListener("click", async () => {
  await flushSasiPersist();
  if (!sasiMedia || !sasiMedia.bytes.length) return;
  const url = URL.createObjectURL(new Blob([sasiMedia.bytes],
    { type: "application/octet-stream" }));
  const link = document.createElement("a");
  link.href = url;
  link.download = /\.(hdf|hds|hdi|nhd|img|bin)$/i.test(sasiMedia.name)
    ? sasiMedia.name : `${sasiMedia.name}.hdf`;
  link.click();
  URL.revokeObjectURL(url);
});

sasiBlockEl.addEventListener("change", () => {
  localStorage.setItem("mzw_sasi_block", sasiBlockEl.value);
});
sasiTargetEl.addEventListener("change", () => {
  localStorage.setItem("mzw_sasi_target", sasiTargetEl.value);
  if (sasiMedia) sasiMedia.target = Number(sasiTargetEl.value);
  if (Module) Module._emu_sasi_set_target(Number(sasiTargetEl.value));
  if (sasiMedia) saveSasiToStore(sasiMedia);
  refreshSasiUi();
});

refreshSasiUi();

// ---- parallel printer capture ------------------------------------------
const printerOnlineEl = document.getElementById("printer-online");
const printerStateEl = document.getElementById("printer-state");
const printerPreviewEl = document.getElementById("printer-preview");
const printerSaveEl = document.getElementById("printer-save");
const printerClearEl = document.getElementById("printer-clear");

let printerBytes = new Uint8Array();
let printerUiSignature = "";
printerOnlineEl.checked = localStorage.getItem("mzw_printer_online") !== "0";

function printerSnapshot() {
  if (!Module) return new Uint8Array();
  const size = Module._emu_printer_snapshot();
  if (size <= 0) return new Uint8Array();
  const ptr = Module._emu_printer_data();
  return Module.HEAPU8.slice(ptr, ptr + size);
}

function formatPrinterPreview(bytes) {
  const start = Math.max(0, bytes.length - 4096);
  let out = start ? `[${start} earlier byte(s) hidden]\n` : "";
  for (let i = start; i < bytes.length; i++) {
    const value = bytes[i];
    if (value === 0x0A) out += "\n";
    else if (value === 0x0D) out += "\\r";
    else if (value === 0x09) out += "\t";
    else if (value >= 0x20 && value <= 0x7E) out += String.fromCharCode(value);
    else out += `\\x${value.toString(16).toUpperCase().padStart(2, "0")}`;
  }
  return out;
}

function applyPrinterHostSettings() {
  if (Module) Module._emu_printer_set_online(printerOnlineEl.checked ? 1 : 0);
}

function refreshPrinterUi(force = false) {
  if (!Module) {
    printerStateEl.textContent = "POWER OFF";
    printerSaveEl.disabled = true;
    printerClearEl.disabled = true;
    return;
  }
  if (force || Module._emu_printer_dirty()) {
    const previousLength = printerBytes.length;
    printerBytes = printerSnapshot();
    Module._emu_printer_clear_dirty();
    printerPreviewEl.value = formatPrinterPreview(printerBytes);
    printerPreviewEl.scrollTop = printerPreviewEl.scrollHeight;
    if (printerBytes.length > previousLength) setIoPanelVisible("printer", true);
  }
  const online = Module._emu_printer_online() !== 0;
  const dropped = Module._emu_printer_dropped();
  const signature = `${online}:${printerBytes.length}:${dropped}`;
  if (signature !== printerUiSignature) {
    printerStateEl.textContent =
      `${online ? "ONLINE" : "OFFLINE"} | ${printerBytes.length} byte(s)` +
      (dropped ? ` | ${dropped} dropped` : "");
    printerUiSignature = signature;
  }
  printerSaveEl.disabled = printerBytes.length === 0;
  printerClearEl.disabled = printerBytes.length === 0;
}

printerOnlineEl.addEventListener("change", () => {
  localStorage.setItem("mzw_printer_online", printerOnlineEl.checked ? "1" : "0");
  applyPrinterHostSettings();
  refreshPrinterUi();
});

printerSaveEl.addEventListener("click", () => {
  printerBytes = printerSnapshot();
  if (!printerBytes.length) return;
  const url = URL.createObjectURL(new Blob([printerBytes],
    { type: "application/octet-stream" }));
  const link = document.createElement("a");
  link.href = url;
  link.download = "mz2500-printer.prn";
  link.click();
  URL.revokeObjectURL(url);
});

printerClearEl.addEventListener("click", () => {
  if (Module) Module._emu_printer_clear_output();
  printerBytes = new Uint8Array();
  printerPreviewEl.value = "";
  printerUiSignature = "";
  refreshPrinterUi();
});

refreshPrinterUi();

// ---- MZ-1E35 raw GPIO and analogue input --------------------------------
const adpcmRamEl = document.getElementById("adpcm-ram");
const adpcmGainEl = document.getElementById("adpcm-gain");
const adpcmGainValueEl = document.getElementById("adpcm-gain-value");
const adpcmInputEl = document.getElementById("adpcm-input");
const adpcmInputStateEl = document.getElementById("adpcm-input-state");
const adpcmStateEl = document.getElementById("adpcm-state");
const adpcmGpioEls = Array.from(document.querySelectorAll("#adpcm-gpio [data-gpio]"));

let adpcmInputStream = null;
let adpcmInputSource = null;
let adpcmUiSignature = "";

adpcmRamEl.value = localStorage.getItem("mzw_adpcm_ram") || "32768";
adpcmGainEl.value = localStorage.getItem("mzw_adpcm_gain") || "100";
let savedGpio = parseInt(localStorage.getItem("mzw_adpcm_gpio") || "15", 10) & 0x0F;
for (const input of adpcmGpioEls)
  input.checked = (savedGpio & (1 << Number(input.dataset.gpio))) !== 0;

function adpcmGpioInputMask() {
  let mask = 0;
  for (const input of adpcmGpioEls)
    if (input.checked) mask |= 1 << Number(input.dataset.gpio);
  return mask;
}

function applyAdpcmHostSettings() {
  const gain = Number(adpcmGainEl.value) / 100;
  adpcmGainValueEl.textContent = gain.toFixed(2);
  if (!Module) return;
  Module._emu_adpcm_set_ram_size(Number(adpcmRamEl.value));
  Module._emu_adpcm_set_gain(gain);
  Module._emu_adpcm_set_gpio_inputs(adpcmGpioInputMask());
}

function refreshAdpcmUi() {
  if (!Module) {
    adpcmStateEl.textContent = "POWER OFF";
    return;
  }
  const direction = Module._emu_adpcm_gpio_direction() & 0x0F;
  const outputs = Module._emu_adpcm_gpio_outputs() & 0x0F;
  const pins = Module._emu_adpcm_gpio_pins() & 0x0F;
  const adc = Module._emu_adpcm_adc_enabled() !== 0;
  const ram = Module._emu_adpcm_ram_size() / 1024;
  const signature = [direction, outputs, pins, adc, ram].join("|");
  if (signature !== adpcmUiSignature) {
    const hex = (value) => value.toString(16).toUpperCase();
    adpcmStateEl.textContent =
      `GPIO DIR=${hex(direction)} OUT=${hex(outputs)} PIN=${hex(pins)} | ` +
      `ADC ${adc ? "running" : "stopped"} | RAM ${ram} KB`;
    for (const input of adpcmGpioEls)
      input.disabled = (direction & (1 << Number(input.dataset.gpio))) !== 0;
    adpcmUiSignature = signature;
  }
}

function queueAdpcmInput(samples, rate) {
  if (!Module || !samples || !samples.length) return;
  const ptr = Module._malloc(samples.length * 4);
  Module.HEAPF32.set(samples, ptr >> 2);
  Module._emu_adpcm_input_samples(ptr, samples.length, rate);
  Module._free(ptr);
}

function stopAdpcmInput() {
  if (workletNode) workletNode.port.postMessage({ capture: false });
  if (adpcmInputSource) adpcmInputSource.disconnect();
  if (adpcmInputStream)
    for (const track of adpcmInputStream.getTracks()) track.stop();
  adpcmInputSource = null;
  adpcmInputStream = null;
  if (Module) Module._emu_adpcm_clear_input();
  adpcmInputEl.setAttribute("aria-pressed", "false");
  adpcmInputEl.textContent = "Start audio input";
  adpcmInputStateEl.textContent = "Input stopped";
}

async function toggleAdpcmInput() {
  if (adpcmInputStream) {
    stopAdpcmInput();
    return;
  }
  if (!Module || !audioCtx || !workletNode) {
    adpcmInputStateEl.textContent = "Power on first";
    return;
  }
  if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia) {
    adpcmInputStateEl.textContent = "Audio capture is unavailable";
    return;
  }
  try {
    const stream = await navigator.mediaDevices.getUserMedia({ audio: {
      channelCount: 1,
      echoCancellation: false,
      noiseSuppression: false,
      autoGainControl: false,
    }});
    Module._emu_adpcm_clear_input();
    adpcmInputStream = stream;
    adpcmInputSource = audioCtx.createMediaStreamSource(stream);
    adpcmInputSource.connect(workletNode);
    workletNode.port.postMessage({ capture: true });
    adpcmInputEl.setAttribute("aria-pressed", "true");
    adpcmInputEl.textContent = "Stop audio input";
    adpcmInputStateEl.textContent = "Browser audio device connected";
  } catch (error) {
    stopAdpcmInput();
    adpcmInputStateEl.textContent = `Input error: ${error && error.name ? error.name : "unknown"}`;
  }
}

adpcmRamEl.addEventListener("change", () => {
  localStorage.setItem("mzw_adpcm_ram", adpcmRamEl.value);
  applyAdpcmHostSettings();
});
adpcmGainEl.addEventListener("input", () => {
  localStorage.setItem("mzw_adpcm_gain", adpcmGainEl.value);
  applyAdpcmHostSettings();
});
for (const input of adpcmGpioEls) {
  input.addEventListener("change", () => {
    localStorage.setItem("mzw_adpcm_gpio", String(adpcmGpioInputMask()));
    applyAdpcmHostSettings();
  });
}
adpcmInputEl.addEventListener("click", toggleAdpcmInput);
applyAdpcmHostSettings();

// ---- disks ----------------------------------------------------------------
// A .d88 file may hold several concatenated volumes (multi-disk releases).
const d88TitleDecoder = new TextDecoder("shift_jis");

function d88Volumes(bytes) {
  const volumes = [];
  let off = 0;
  while (off + 0x2b0 <= bytes.length && volumes.length < 16) {
    const size = bytes[off + 0x1c] | (bytes[off + 0x1d] << 8) |
                 (bytes[off + 0x1e] << 16) | (bytes[off + 0x1f] << 24);
    if (size < 0x2b0 || off + size > bytes.length) break;
    const rawTitle = bytes.subarray(off, off + 17);
    const terminator = rawTitle.indexOf(0);
    const titleBytes = terminator < 0 ? rawTitle : rawTitle.subarray(0, terminator);
    const title = d88TitleDecoder.decode(titleBytes)
      .replace(/[\u0000-\u001f\u007f]/g, "").trim();
    volumes.push({ title, bytes: bytes.slice(off, off + size) });
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

const DRIVE_ORIGIN_USER = "user";
const DRIVE_ORIGIN_NEKO_DEMO = "neko-demo";

// drive state: null or { name, volumes, current, origin }
const drives = [null, null];

function isNekoDemoDisk(drive) {
  return drives[drive]?.origin === DRIVE_ORIGIN_NEKO_DEMO;
}

// ---- persistence (IndexedDB): inserted disks survive reloads ------------
// The connection is opened once and cached (both as a promise for callers
// that can await, and as the resolved IDBDatabase itself in dbConn) so a
// pagehide flush (Finding 5) never has to pay for a fresh indexedDB.open()
// round trip - by the time anything could be dirty, refreshRomSlots() at
// startup has already opened it.
let dbConn = null;
let dbPromise = null;
let dbUnavailable = false;
function idb() {
  if (dbUnavailable) return Promise.reject(new Error("IndexedDB unavailable"));
  if (!dbPromise) {
    dbPromise = new Promise((res, rej) => {
      const r = indexedDB.open("mz2500w", 4);
      let settled = false;
      const timer = setTimeout(() => {
        if (settled) return;
        settled = true;
        dbUnavailable = true;
        dbPromise = null;
        rej(new Error("IndexedDB open timed out"));
      }, 2000);
      r.onupgradeneeded = () => {
        const db = r.result;
        if (!db.objectStoreNames.contains("drives")) db.createObjectStore("drives");
        if (!db.objectStoreNames.contains("roms")) db.createObjectStore("roms");
        if (!db.objectStoreNames.contains("cmt")) db.createObjectStore("cmt");
        if (!db.objectStoreNames.contains("sasi")) db.createObjectStore("sasi");
      };
      r.onsuccess = () => {
        if (settled) {
          r.result.close();
          return;
        }
        settled = true;
        clearTimeout(timer);
        dbConn = r.result;
        res(r.result);
      };
      r.onerror = () => {
        if (settled) return;
        settled = true;
        clearTimeout(timer);
        dbUnavailable = true;
        dbPromise = null;
        rej(r.error);
      };
      // A tab left open on an older schema can block the upgrade request.
      // Persistence is optional, so never let that stall POWER ON or leave
      // the ROM panel empty; a later retry can connect after the old tab is
      // closed or reloaded.
      r.onblocked = () => {
        if (settled) return;
        settled = true;
        clearTimeout(timer);
        dbUnavailable = true;
        dbPromise = null;
        rej(new Error("IndexedDB upgrade is blocked by another tab"));
      };
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

function putCmtRecordSync(media) {
  if (!dbConn || !media) return false;
  try {
    dbConn.transaction("cmt", "readwrite").objectStore("cmt").put({
      name: media.name,
      buffer: media.bytes.slice().buffer,
      kind: media.kind || detectTapeKind(media.bytes),
      wp: !!media.wp,
      inserted: !!media.inserted,
    }, "tape");
    return true;
  } catch (e) {
    return false;
  }
}

async function saveCmtToStore(media) {
  if (!media || !media.bytes) return;
  if (putCmtRecordSync(media)) return;
  try {
    await idb();
    putCmtRecordSync(media);
  } catch (e) { /* private mode etc.: persistence is best-effort */ }
}

async function loadCmtFromStore() {
  try {
    const db = await idb();
    return await new Promise((res) => {
      const rq = db.transaction("cmt").objectStore("cmt").get("tape");
      rq.onsuccess = () => res(rq.result || null);
      rq.onerror = () => res(null);
    });
  } catch (e) { return null; }
}

function putSasiRecordSync(media) {
  if (!dbConn || !media) return false;
  try {
    dbConn.transaction("sasi", "readwrite").objectStore("sasi").put({
      name: media.name,
      buffer: media.bytes.slice().buffer,
      blockSize: Number(media.blockSize || 0),
      wp: !!media.wp,
      inserted: !!media.inserted,
      target: Number(media.target || 0),
    }, "disk");
    return true;
  } catch (e) {
    return false;
  }
}

async function saveSasiToStore(media) {
  if (!media || !media.bytes) return;
  if (putSasiRecordSync(media)) return;
  try {
    await idb();
    putSasiRecordSync(media);
  } catch (e) { /* best-effort */ }
}

async function loadSasiFromStore() {
  try {
    const db = await idb();
    return await new Promise((res) => {
      const rq = db.transaction("sasi").objectStore("sasi").get("disk");
      rq.onsuccess = () => res(rq.result || null);
      rq.onerror = () => res(null);
    });
  } catch (e) { return null; }
}

// ---- user ROM slots (browser-local only; nothing is ever uploaded) -------
const ROM_KINDS = [
  { key: "ipl", kind: 0, label: "ipl.rom", note: "32KB / MZ-2500 IPL" },
  { key: "kanji", kind: 2, label: "kanji.rom", note: "256KB / バンク39h窓" },
  { key: "dict", kind: 3, label: "dict.rom", note: "256KB / バンク3Ah窓" },
  { key: "sasi", kind: 4, label: "sasi.rom", note: "MZ-1E30 BIOS / A8h-A9h" },
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
  { id: "hw-adpcm", kind: 3, key: "mzw_hw_adpcm" },
  { id: "hw-emm", kind: 4, key: "mzw_hw_emm" },
  { id: "hw-sasi", kind: 5, key: "mzw_hw_sasi" },
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
    applyHwOptionsToMachine(); // RAM/GRAM changes settle at the next IPL
  });
}

const realIplEl = document.getElementById("real-ipl-mode");
const bootModeEl = document.getElementById("boot-mode");
bootModeEl.value = localStorage.getItem("mzw_boot_mode") || "0";
bootModeEl.addEventListener("change", () => {
  localStorage.setItem("mzw_boot_mode", bootModeEl.value);
  if (Number(bootModeEl.value) !== 0 && isNekoDemoDisk(0)) {
    ejectDrive(0, { clearStored: false });
    statusEl.textContent = "NEKO DEMO EJECTED - PRESS IPL";
    return;
  }
  if (Module) {
    statusEl.textContent = "BOOT MODE CHANGED - PRESS IPL";
  }
});
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
    diskFilenameEls[drive].textContent = "NO DISK";
    diskTitleEls[drive].textContent = "—";
    volEls[drive].hidden = true;
    return;
  }
  const multi = d.volumes.length > 1;
  const volume = d.volumes[clampVolumeIndex(d.current, d.volumes.length)];
  diskFilenameEls[drive].textContent = d.name || "UNTITLED.D88";
  diskTitleEls[drive].textContent = volume.title || "UNTITLED";
  volEls[drive].hidden = !multi;
  if (multi) {
    volEls[drive].innerHTML = "";
    d.volumes.forEach((v, i) => {
      const o = document.createElement("option");
      o.value = i;
      o.textContent = `${i + 1}: ${v.title || "UNTITLED"}`;
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
  // core. Same ordering bug as bootFromFile()/iplBoot() (see finding 1):
  // reassigning first would let the flush snapshot the CORE's still-old
  // bytes into the NEW disk's record and name.
  await flushDiskPersist(drive);
  drives[drive] = {
    name,
    volumes,
    current,
    origin: (opts && opts.origin) || DRIVE_ORIGIN_USER,
  };
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
  if (d.origin === DRIVE_ORIGIN_NEKO_DEMO) return;
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
  drives[drive] = {
    name: "BLANK",
    volumes: [{ bytes, title: "BLANK" }],
    current: 0,
    origin: DRIVE_ORIGIN_USER,
  };
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

function stopAtIplPrompt(message) {
  stopIplWatchdog();
  running = false;
  restartAudio();
  ctx2d.fillStyle = "#000";
  ctx2d.fillRect(0, 0, canvas.width, canvas.height);
  lampEls[0].classList.remove("on");
  lampEls[1].classList.remove("on");
  fpsEl.textContent = "";
  audioStatEl.textContent = "";
  statusEl.textContent = message;
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
    if (Number(bootModeEl.value) !== 0) {
      stopIplWatchdog();
      statusEl.textContent =
        `LEGACY IPL STOPPED AT PC=${j.cpu.pc.toString(16).toUpperCase()}h`;
      return false;
    }
    stopIplWatchdog();
    realIplEl.checked = false;
    localStorage.setItem("mzw_real_ipl", "0");
    iplBoot();
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

async function iplBoot(options = {}) {
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
  const requestedBootMode = Number(bootModeEl.value);
  // The bundled disk belongs only to the launch initiated by the NEKO
  // convenience button. Any later front-panel IPL starts from normal media
  // state, including another IPL in MZ-2500 mode.
  if (isNekoDemoDisk(0) && !options.allowNekoDemo) {
    ejectDrive(0, { clearStored: false });
  }
  Module._emu_set_boot_mode(requestedBootMode);
  const hasIpl = Module._emu_has_ipl() !== 0;
  const hasKanji = Module._emu_has_kanji() !== 0;
  if (requestedBootMode !== 0 && (!hasIpl || !hasKanji)) {
    stopAtIplPrompt("LEGACY MODE REQUIRES MZ-2500 IPL.ROM AND KANJI.ROM");
    return false;
  }
  if (drives[0] && !pushVolume(0)) {
    stopAtIplPrompt("D88 IMAGE COULD NOT BE MOUNTED");
    return false;
  }
  if (drives[1] && !pushVolume(1)) {
    stopAtIplPrompt("D88 IMAGE COULD NOT BE MOUNTED");
    return false;
  }

  // The core inspects the mounted records rather than the filename. This
  // keeps IPLPRO as the only automatic dummy-IPL case and makes every other
  // structurally valid D88 require the owner's real IPL ROM.
  const diskProfile = drives[0] ? Module._emu_disk_boot_profile(0) : 0;
  if (diskProfile === 3) {
    stopAtIplPrompt("D88 HAS UNSUPPORTED OR TRUNCATED RECORDS");
    return false;
  }
  if (diskProfile === 2 && !hasIpl) {
    stopAtIplPrompt("SPECIAL D88 REQUIRES IPL.ROM");
    return false;
  }
  const wantRealIpl = hasIpl &&
    (diskProfile === 2 ||
     (!options.forceDummyIpl && (realIplEl.checked || requestedBootMode !== 0)));
  if (!drives[0] && !wantRealIpl) {
    stopAtIplPrompt("READY - NO DISK (INSERT D88, THEN PRESS IPL)");
    return false;
  }
  const ok = wantRealIpl ? Module._emu_boot_real_ipl()
                         : (drives[0] ? Module._emu_boot() : 0);
  if (!ok) {
    stopAtIplPrompt(wantRealIpl ? "REAL IPL DID NOT START" : "NOT A BOOTABLE DISK");
    return false;
  }
  applyAdpcmHostSettings();
  sioAppliedModem = [null, null];
  syncSioModemInputs();
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
  return true;
}

// canvas drop / initial load: mount into FD1 (volume 2 goes to FD2) and boot
//
// Finding 1: this used to assign drives[0]/drives[1] for the INCOMING disk
// and only then call iplBoot(), which flushes any pending persist. But
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
  const origin = (opts && opts.origin) || DRIVE_ORIGIN_USER;
  drives[0] = { name, volumes, current: 0, origin };
  if (volumes.length > 1) drives[1] = { name, volumes, current: 1, origin };
  if (!opts || !opts.noSave) {
    saveDriveToStore(0, name, bytes, 0);
    if (volumes.length > 1) saveDriveToStore(1, name, bytes, 1);
  }
  await iplBoot(opts);
}

async function powerOn() {
  overlay.classList.add("hidden");
  statusEl.textContent = "BOOTING...";

  const v = encodeURIComponent(window.BUILD_ID || "dev");
  audioCtx = new AudioContext();
  statusEl.textContent = "LOADING AUDIO...";
  await audioCtx.audioWorklet.addModule("audio-worklet.js?v=" + v);
  workletNode = new AudioWorkletNode(audioCtx, "mz-audio", { outputChannelCount: [2] });
  masterGainNode = audioCtx.createGain();
  masterGainNode.gain.value = Number(masterVolumeEl.value);
  workletNode.connect(masterGainNode);
  masterGainNode.connect(audioCtx.destination);
  workletNode.port.onmessage = (e) => {
    if (e.data.input) {
      queueAdpcmInput(e.data.input, e.data.rate || audioCtx.sampleRate);
      return;
    }
    underruns = e.data.underruns;
    dropped = e.data.dropped;
    lastReportedQueued = e.data.queued;
    // snap the depth estimate to the worklet's ground truth (heals both
    // underrun silence-fill and any counter drift)
    produced = (audioCtx.currentTime - audioT0) * audioCtx.sampleRate + e.data.queued;
  };
  // A background Chrome tab may keep the context suspended and leave the
  // resume promise pending even though this function was entered from a
  // click. Audio can join later; machine startup must not wait forever.
  audioCtx.resume().catch(() => {});

  // core stderr = diagnostics (unimplemented-port notes etc.), not errors;
  // keep them out of the browser's error channel
  statusEl.textContent = "LOADING CORE...";
  Module = await createMZ2500({ printErr: (t) => console.log("[core]", t) });
  Module._emu_init(audioCtx.sampleRate);
  Module._emu_set_boot_mode(Number(bootModeEl.value));
  sioAppliedModem = [null, null];
  syncSioModemInputs();
  updateSioUi();
  applyHwOptionsToMachine();
  applyAdpcmHostSettings();
  applyPrinterHostSettings();
  refreshPrinterUi(true);
  statusEl.textContent = "RESTORING MEDIA...";
  await applyRomsToMachine();
  if (!cmtMedia) {
    const savedCmt = await loadCmtFromStore();
    if (savedCmt) {
      cmtMedia = {
        name: savedCmt.name || "tape.wav",
        bytes: new Uint8Array(savedCmt.buffer),
        kind: savedCmt.kind || detectTapeKind(new Uint8Array(savedCmt.buffer)),
        wp: !!savedCmt.wp,
        inserted: savedCmt.inserted !== false,
      };
    }
  }
  if (cmtMedia && cmtMedia.inserted && !mountCmtMedia())
    statusEl.textContent = "CMT: STORED TAPE IMAGE IS INVALID";
  refreshCmtUi();
  if (!sasiMedia) {
    const savedSasi = await loadSasiFromStore();
    if (savedSasi) {
      sasiMedia = {
        name: savedSasi.name || "disk.hdf",
        bytes: new Uint8Array(savedSasi.buffer),
        blockSize: Number(savedSasi.blockSize || 0),
        wp: !!savedSasi.wp,
        inserted: savedSasi.inserted !== false,
        target: Number(savedSasi.target || 0),
      };
      sasiTargetEl.value = String(sasiMedia.target);
    }
  }
  Module._emu_sasi_set_target(Number(sasiTargetEl.value));
  if (sasiMedia && sasiMedia.inserted && !mountSasiMedia())
    statusEl.textContent = "SASI: STORED IMAGE IS INVALID";
  refreshSasiUi();

  // Restore user-selected disks saved in this browser (IndexedDB). Disks
  // chosen while the power was still off are already in drives[] and take
  // precedence. Bundled media is never inserted by this path.
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
      drives[0] = {
        name: saved0.name,
        volumes,
        current: clampVolumeIndex(saved0.current, volumes.length),
        origin: DRIVE_ORIGIN_USER,
      };
  }
  await iplBoot();
}

let powerOnPromise = null;
let nekoDemoPromise = null;

function ensurePowerOn() {
  if (powerOnPromise) return powerOnPromise;
  if (Module) return Promise.resolve();
  powerOnPromise = powerOn()
    .catch((error) => {
      console.error("Power-on failed", error);
      statusEl.textContent = "POWER ON FAILED";
      throw error;
    })
    .finally(() => { powerOnPromise = null; });
  return powerOnPromise;
}

function loadNekoDemo() {
  if (nekoDemoPromise) return nekoDemoPromise;
  nekoDemoBtn.disabled = true;
  nekoDemoBtn.setAttribute("aria-busy", "true");

  // This is an explicit MZ-2500 demo action, so make the mode switch visible
  // and persistent. The one-shot dummy-IPL option keeps the demo independent
  // of an optional real IPL setting without changing that advanced setting.
  bootModeEl.value = "0";
  localStorage.setItem("mzw_boot_mode", "0");

  nekoDemoPromise = (async () => {
    await ensurePowerOn();
    if (!Module) return;
    Module._emu_set_boot_mode(0);
    statusEl.textContent = "LOADING NEKO CAN RUN DEMO...";
    const v = encodeURIComponent(window.BUILD_ID || "dev");
    const resp = await fetch("neko_can_run_demo.d88?v=" + v);
    if (!resp.ok) throw new Error(`Demo disk fetch failed: ${resp.status}`);
    await bootFromFile(
      "neko_can_run_demo.d88",
      new Uint8Array(await resp.arrayBuffer()),
      {
        noSave: true,
        forceDummyIpl: true,
        allowNekoDemo: true,
        origin: DRIVE_ORIGIN_NEKO_DEMO,
      });
  })()
    .catch((error) => {
      console.error("NEKO demo load failed", error);
      statusEl.textContent = "NEKO DEMO LOAD FAILED";
    })
    .finally(() => {
      nekoDemoBtn.disabled = false;
      nekoDemoBtn.removeAttribute("aria-busy");
      nekoDemoPromise = null;
    });
  return nekoDemoPromise;
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
    pumpSio();
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
  if (Module._emu_cmt_dirty()) scheduleCmtPersist();
  if (Module._emu_sasi_dirty()) scheduleSasiPersist();
  const lamps = Module._emu_fdd_lamps();
  lampEls[0].classList.toggle("on", (lamps & 1) !== 0);
  lampEls[1].classList.toggle("on", (lamps & 2) !== 0);
  updateSioUi(now);
  refreshCmtUi();
  refreshAdpcmUi();
  refreshPrinterUi();
  refreshSasiUi();

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
  flushCmtPersist();
  flushSasiPersist();
}
window.addEventListener("pagehide", flushAllDiskPersists);
window.addEventListener("pagehide", stopAdpcmInput);

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
function ejectDrive(drive, options = {}) {
  const disk = drives[drive];
  const hadDisk = !!disk;
  if (diskSaveTimers[drive]) {
    clearTimeout(diskSaveTimers[drive]);
    diskSaveTimers[drive] = null;
  }
  drives[drive] = null;
  if (Module) Module._emu_disk_eject(drive);
  const clearStored = options.clearStored !== false &&
    (!disk || disk.origin !== DRIVE_ORIGIN_NEKO_DEMO);
  if (clearStored) clearDriveStore(drive);
  refreshDriveUI(drive);
  document.getElementById(`wp${drive}`).setAttribute("aria-pressed", "false");
  return hadDisk;
}

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
    const hadDisk = ejectDrive(drive);
    if (hadDisk) statusEl.textContent = `FD${drive + 1}: EJECTED`;
  });
  const box = document.getElementById("ins" + drive).closest(".drive");
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

function systemReset() {
  if (!Module) return;
  stopIplWatchdog();
  Module._emu_system_reset();
  statusEl.textContent = "RUNNING (SYSTEM RESET)";
  restartAudio();
  if (!running) {
    running = true;
    fpsWindowStart = performance.now();
    requestAnimationFrame(tick);
  }
}

document.getElementById("reset-btn").addEventListener("click", systemReset);
document.getElementById("ipl-btn").addEventListener("click", () => iplBoot());

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

document.getElementById("power").addEventListener("click", () => {
  ensurePowerOn().catch(() => {});
});
nekoDemoBtn.addEventListener("click", loadNekoDemo);

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
      `COMPAT boot=${j.compat.boot} memory=${j.compat.memory} display=${j.compat.display} ` +
      `frame=${j.compat.frame_cycles}cyc\n` +
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
const CONTEXT_MENU_CLICK_MS = 120;
const CONTEXT_MENU_DUPLICATE_MS = 500;
const deliveredMouseButtons = [false, false];
let hostPrimaryDown = false;
let hostPhysicalSecondaryDown = false;
let hostContextSecondaryDown = false;
let contextPrimaryCandidate = null;
let contextPrimaryHoldsSecondary = false;
let contextSecondaryReleaseTimer = 0;
let lastPhysicalSecondaryEventAt = -Infinity;

function deliverMouseButton(index, down) {
  if (deliveredMouseButtons[index] === down) return;
  deliveredMouseButtons[index] = down;
  if (Module && running) Module._emu_mouse_button(index, down ? 1 : 0);
}

function syncHostMouseButtons() {
  deliverMouseButton(0, hostPrimaryDown);
  deliverMouseButton(1, hostPhysicalSecondaryDown || hostContextSecondaryDown);
}

function clearContextPrimaryCandidate() {
  if (!contextPrimaryCandidate) return;
  if (contextPrimaryCandidate.timer) clearTimeout(contextPrimaryCandidate.timer);
  contextPrimaryCandidate = null;
}

function releaseContextSecondaryLater() {
  if (contextSecondaryReleaseTimer) clearTimeout(contextSecondaryReleaseTimer);
  contextSecondaryReleaseTimer = setTimeout(() => {
    contextSecondaryReleaseTimer = 0;
    hostContextSecondaryDown = false;
    syncHostMouseButtons();
  }, CONTEXT_MENU_CLICK_MS);
}

function clickMouseButton(index) {
  if (index === 0) hostPrimaryDown = true;
  else hostContextSecondaryDown = true;
  syncHostMouseButtons();
  if (index === 0) {
    setTimeout(() => {
      hostPrimaryDown = false;
      syncHostMouseButtons();
    }, CONTEXT_MENU_CLICK_MS);
  } else {
    releaseContextSecondaryLater();
  }
}

function releaseHostMouseButtons() {
  clearContextPrimaryCandidate();
  if (contextSecondaryReleaseTimer) clearTimeout(contextSecondaryReleaseTimer);
  contextSecondaryReleaseTimer = 0;
  contextPrimaryHoldsSecondary = false;
  hostPrimaryDown = false;
  hostPhysicalSecondaryDown = false;
  hostContextSecondaryDown = false;
  syncHostMouseButtons();
}

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
    if (e.button === 0 && type === "mousedown" && e.ctrlKey) {
      // Some platforms turn Ctrl+primary into a context-menu gesture. Hold
      // this primary event briefly and let the contextmenu event make the
      // decision; if no contextmenu follows, it remains a normal click.
      clearContextPrimaryCandidate();
      contextPrimaryCandidate = { released: false, timer: 0 };
      e.preventDefault();
      return;
    }

    if (e.button === 0 && type === "mouseup" && contextPrimaryHoldsSecondary) {
      contextPrimaryHoldsSecondary = false;
      hostContextSecondaryDown = false;
      syncHostMouseButtons();
      e.preventDefault();
      return;
    }

    if (e.button === 0 && type === "mouseup" && contextPrimaryCandidate) {
      const candidate = contextPrimaryCandidate;
      candidate.released = true;
      candidate.timer = setTimeout(() => {
        if (contextPrimaryCandidate !== candidate) return;
        contextPrimaryCandidate = null;
        clickMouseButton(0);
      }, CONTEXT_MENU_CLICK_MS);
      e.preventDefault();
      return;
    }

    if (e.button === 0) {
      hostPrimaryDown = type === "mousedown";
    } else if (e.button === 2) {
      hostPhysicalSecondaryDown = type === "mousedown";
      lastPhysicalSecondaryEventAt = performance.now();
    } else {
      return;
    }
    e.preventDefault();
    syncHostMouseButtons();
  });
}

// Treat the platform's semantic context-menu gesture as MZ button 2. This
// covers trackpad gestures and accessibility mappings that do not expose a
// physical button 2. A physical right-click also emits contextmenu on many
// browsers, so the recent-event guard prevents a duplicate click.
document.addEventListener("contextmenu", (e) => {
  if (document.pointerLockElement !== canvas) return;
  e.preventDefault();
  if (!Module || !running) return;

  if (contextPrimaryCandidate) {
    const released = contextPrimaryCandidate.released;
    clearContextPrimaryCandidate();
    hostContextSecondaryDown = true;
    syncHostMouseButtons();
    if (released) releaseContextSecondaryLater();
    else contextPrimaryHoldsSecondary = true;
    return;
  }

  if (hostPhysicalSecondaryDown ||
      performance.now() - lastPhysicalSecondaryEventAt < CONTEXT_MENU_DUPLICATE_MS) return;

  // If a browser reports an unusual primary-button context gesture without
  // Ctrl, undo the primary state before delivering the semantic secondary.
  if (hostPrimaryDown && (e.button === 0 || e.ctrlKey)) hostPrimaryDown = false;
  hostContextSecondaryDown = true;
  syncHostMouseButtons();
  releaseContextSecondaryLater();
});

document.addEventListener("pointerlockchange", () => {
  releaseHostMouseButtons();
  mouseBtn.setAttribute("aria-pressed",
    document.pointerLockElement === canvas ? "true" : String(mouseEnabled));
});
window.addEventListener("blur", releaseHostMouseButtons);

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
