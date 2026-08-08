// The spectator page, served whole at GET / — no build step, no external
// assets. It decodes the same framing as protocol.ts (keep in sync) and
// plays the stream at machine speed using the Web Audio clock as the only
// timeline: video frames are shown when their audio is heard.
export const VIEWER_HTML = `<!doctype html>
<html lang="ja">
<head>
<meta charset="utf-8">
<title>MZ-2500 観戦ビュー</title>
<style>
  body { background: #14161a; color: #d8dbe0; font-family: ui-monospace, Menlo, monospace;
         margin: 0; display: flex; flex-wrap: wrap; gap: 16px; padding: 16px; }
  #screen { image-rendering: pixelated; width: min(1280px, 90vw); background: #000;
            border: 1px solid #333; }
  #status { margin: 8px 0; min-height: 1.2em; }
  button { font: inherit; background: #2a2e35; color: inherit; border: 1px solid #555;
           padding: 4px 12px; cursor: pointer; }
  button:disabled { opacity: 0.4; cursor: default; }
  #panel { min-width: 280px; }
  #panel h3 { margin: 12px 0 4px; font-size: 14px; color: #8ab; }
  table { border-collapse: collapse; font-size: 13px; width: 100%; }
  td, th { border: 1px solid #3a3e45; padding: 2px 8px; text-align: left; }
  .on { color: #7f7; }
  .off { color: #666; }
</style>
</head>
<body>
<div>
  <canvas id="screen" width="640" height="400"></canvas>
  <div id="status">▶ を押すと観戦を開始します（音が出ます）</div>
  <button id="start">▶ 観戦開始</button>
  <button id="live" disabled>最新へジャンプ</button>
</div>
<div id="panel">
  <h3>サウンド (YM2203)</h3>
  <table id="sound"><tbody><tr><td>-</td></tr></tbody></table>
  <h3>情報</h3>
  <table><tbody>
    <tr><td>フレーム</td><td id="frameno">-</td></tr>
    <tr><td>遅延</td><td id="lag">-</td></tr>
  </tbody></table>
</div>
<script>
"use strict";
var canvas = document.getElementById("screen");
var ctx2d = canvas.getContext("2d");
var statusEl = document.getElementById("status");
var startBtn = document.getElementById("start");
var liveBtn = document.getElementById("live");

var audioCtx = null;
var info = null;            // stream header
var nextTime = 0;           // audio schedule head (audioCtx time)
var timeline = [];          // {frameNo, at} per scheduled audio chunk
var videoQueue = [];        // {frameNo, bitmap} in arrival order
var stateQueue = [];        // {frameNo, state} applied when playback reaches them
var decodeChain = Promise.resolve(null); // keeps PNG decode order
var sources = [];           // scheduled AudioBufferSourceNodes (for jump)
var playFrame = -1;         // frame whose audio is playing now
var lastHeartbeat = 0;
var streamOpen = false;

function setStatus(t) { statusEl.textContent = t; }

// ---- incremental protocol decoder (mirror of protocol.ts) ----
var parseBuf = new Uint8Array(0);
var haveHeader = false;
function concatBytes(a, b) {
  var out = new Uint8Array(a.length + b.length);
  out.set(a, 0); out.set(b, a.length);
  return out;
}
function parseChunk(chunk, onMessage) {
  parseBuf = concatBytes(parseBuf, chunk);
  for (;;) {
    if (!haveHeader) {
      var nl = parseBuf.indexOf(10);
      if (nl < 0) return;
      info = JSON.parse(new TextDecoder().decode(parseBuf.subarray(0, nl)));
      parseBuf = parseBuf.subarray(nl + 1);
      haveHeader = true;
    }
    if (parseBuf.length < 4) return;
    var view = new DataView(parseBuf.buffer, parseBuf.byteOffset);
    var len = view.getUint32(0, true);
    if (parseBuf.length < 4 + len) return;
    var type = parseBuf[4];
    var payload = parseBuf.subarray(5, 4 + len);
    parseBuf = parseBuf.subarray(4 + len);
    if (type === 0) { onMessage(type, -1, null); continue; }
    var pv = new DataView(payload.buffer, payload.byteOffset);
    onMessage(type, pv.getUint32(0, true), payload.subarray(4));
  }
}

// ---- message handling ----
function onMessage(type, frameNo, body) {
  if (type === 0) { lastHeartbeat = performance.now(); return; }
  if (type === 1) { // video: decode in order, then queue
    var blob = new Blob([body], { type: "image/png" });
    decodeChain = decodeChain.then(function (prev) {
      return createImageBitmap(blob).then(function (bmp) {
        videoQueue.push({ frameNo: frameNo, bitmap: bmp });
        return bmp;
      });
    });
  } else if (type === 2) { // repeat: alias the previous bitmap
    decodeChain = decodeChain.then(function (prev) {
      if (prev) videoQueue.push({ frameNo: frameNo, bitmap: prev });
      return prev;
    });
  } else if (type === 3) { // audio: schedule at the buffer head
    if (!audioCtx) return;
    var n = body.length >> 1;
    var buf = audioCtx.createBuffer(1, Math.max(n, 1), info.audioRate);
    var ch = buf.getChannelData(0);
    var dv = new DataView(body.buffer, body.byteOffset);
    for (var i = 0; i < n; i++) ch[i] = dv.getInt16(i * 2, true) / 32768;
    var src = audioCtx.createBufferSource();
    src.buffer = buf;
    src.connect(audioCtx.destination);
    var at = Math.max(nextTime, audioCtx.currentTime + 0.05);
    src.start(at);
    timeline.push({ frameNo: frameNo, at: at });
    nextTime = at + buf.duration;
    sources.push(src);
    src.onended = function () {
      var i = sources.indexOf(src);
      if (i >= 0) sources.splice(i, 1);
    };
  } else if (type === 4) { // state: queue until playback reaches its frame
    stateQueue.push({ frameNo: frameNo, state: JSON.parse(new TextDecoder().decode(body)) });
  }
}

function renderSound(s) {
  var rows = [];
  for (var i = 0; i < s.fm.length; i++) {
    var c = s.fm[i];
    rows.push("<tr><td>FM" + (c.ch + 1) + "</td><td class=" +
      (c.keyOn ? "on>" + c.note + " " + c.freqHz + "Hz" : "off>-") + "</td></tr>");
  }
  for (var j = 0; j < s.ssg.length; j++) {
    var g = s.ssg[j];
    var on = g.toneEnabled && g.volume > 0;
    rows.push("<tr><td>SSG" + "ABC"[g.ch] + "</td><td class=" +
      (on ? "on>" + g.note + " vol" + g.volume : "off>-") + "</td></tr>");
  }
  rows.push("<tr><td>BEEP</td><td class=" + (s.beep ? "on>on" : "off>-") + "</td></tr>");
  document.getElementById("sound").innerHTML = "<tbody>" + rows.join("") + "</tbody>";
}

// ---- playback loop: audio clock drives video ----
function tick() {
  requestAnimationFrame(tick);
  if (!audioCtx) return;
  var t = audioCtx.currentTime;
  while (timeline.length && timeline[0].at <= t) playFrame = timeline.shift().frameNo;
  var drew = null;
  while (videoQueue.length && videoQueue[0].frameNo <= playFrame) drew = videoQueue.shift();
  if (drew) ctx2d.drawImage(drew.bitmap, 0, 0);
  var stateDrew = null;
  while (stateQueue.length && stateQueue[0].frameNo <= playFrame) stateDrew = stateQueue.shift();
  if (stateDrew) renderSound(stateDrew.state);
  document.getElementById("frameno").textContent = playFrame < 0 ? "-" : String(playFrame);
  var lag = Math.max(0, nextTime - t);
  document.getElementById("lag").textContent = lag.toFixed(1) + "s";
  liveBtn.disabled = lag < 1;
  if (!streamOpen || performance.now() - lastHeartbeat > 5000) {
    setStatus("⏹ 切断されました — 再読み込みで再接続");
  } else if (nextTime <= t) {
    setStatus("⏸ AIの操作待ち（マシン停止中）");
  } else {
    setStatus("▶ 再生中");
  }
}

function jumpToLive() {
  for (var i = 0; i < sources.length; i++) { try { sources[i].stop(); } catch (e) {} }
  sources = [];
  timeline = [];
  if (videoQueue.length) {
    var last = videoQueue[videoQueue.length - 1];
    ctx2d.drawImage(last.bitmap, 0, 0);
    playFrame = last.frameNo;
  }
  videoQueue = [];
  if (stateQueue.length) renderSound(stateQueue[stateQueue.length - 1].state);
  stateQueue = [];
  nextTime = 0;
}
liveBtn.addEventListener("click", jumpToLive);

// ---- stream reader ----
function connect() {
  fetch("/stream").then(function (res) {
    var reader = res.body.getReader();
    streamOpen = true;
    lastHeartbeat = performance.now();
    function pump() {
      reader.read().then(function (r) {
        if (r.done) { streamOpen = false; return; }
        lastHeartbeat = performance.now();
        parseChunk(r.value, onMessage);
        pump();
      }, function () { streamOpen = false; });
    }
    pump();
  }, function () { streamOpen = false; });
}

startBtn.addEventListener("click", function () {
  if (audioCtx) return;
  audioCtx = new AudioContext();
  startBtn.disabled = true;
  connect();
  tick();
});
</script>
</body>
</html>
`;
