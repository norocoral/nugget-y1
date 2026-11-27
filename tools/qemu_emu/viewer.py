#!/usr/bin/env python3
import collections
import http.server
import json
import os
import struct
import threading
import time

EMU_DIR = os.environ.get("Y1_EMU_DIR", "/tmp/y1_emu")
FB_PATH = os.path.join(EMU_DIR, "fb0")
FIFO_PATH = os.path.join(EMU_DIR, "input_fifo")
AUDIO_FIFO = os.path.join(EMU_DIR, "audio_fifo")
PORT = int(os.environ.get("Y1_EMU_PORT", "8765"))

audio_lock = threading.Lock()
audio_chunks = collections.deque(maxlen=64)

def audio_reader():
    while True:
        try:
            fd = os.open(AUDIO_FIFO, os.O_RDONLY)
            while True:
                data = os.read(fd, 8192)
                if not data:
                    break
                with audio_lock:
                    audio_chunks.append(data)
            os.close(fd)
        except OSError:
            time.sleep(0.1)

HTML = b"""<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<title>qemu emu</title>
<style>
  body { background:#000; color:#fff; font-family:system-ui,sans-serif; display:flex; flex-direction:column; align-items:center; margin:16px; }
  canvas { width:480px; height:360px; background:#000; display:block; }
</style>
</head>
<body>
  <div class="bezel" id="bezel">
    <canvas id="scr" width="480" height="360"></canvas>
  </div>
  <div class="wheel">
    <button onclick="tap(103)">Wheel -</button>
    <button onclick="tap(158)">MENU</button>
    <button onclick="tap(108)">Wheel +</button>
    <button onclick="tap(105)">Prev</button>
    <button onclick="tap(232)">Select</button>
    <button onclick="tap(106)">Skip</button>
    <div></div>
    <button onclick="tap(164)">Play/pause</button>
    <button id="audBtn" onclick="toggleAudio()">Audio: OFF</button>
  </div>
  <div class="hw">
    <label>Battery <input type="range" id="bat" min="0" max="100" value="85" onchange="setHw()"> <span id="batVal">85%</span></label>
    <label><input type="checkbox" id="chg" onchange="setHw()"> Charging</label>
    <label><input type="checkbox" id="usb" onchange="setHw()"> USB</label>
    <label><input type="checkbox" id="hp" checked onchange="setHw()"> Headphones</label>
    <span>BL: <b id="blVal">165</b></span>
  </div>
  <div class="hint">Keys: Up/Down or Mouse Wheel = Scroll, Enter = Select, Esc/Backspace = Menu, Space = Play, Left/Right = Prev/Next</div>
<script>
const cvs = document.getElementById('scr');
const ctx = cvs.getContext('2d');
const img = ctx.createImageData(480, 360);

async function refresh() {
  try {
    const r = await fetch('/fb.bgra', {cache:'no-store'});
    if (r.ok) {
      const buf = new Uint8Array(await r.arrayBuffer());
      if (buf.length === 480 * 360 * 4) {
        const d = img.data;
        for (let i = 0; i < buf.length; i += 4) {
          d[i] = buf[i + 2]; d[i + 1] = buf[i + 1]; d[i + 2] = buf[i]; d[i + 3] = 255;
        }
        ctx.putImageData(img, 0, 0);
      }
    }
  } catch (e) {}
  setTimeout(refresh, 33);
}
refresh();

async function pollHw() {
  try {
    const r = await fetch('/hw_state', {cache:'no-store'});
    if (r.ok) {
      const s = await r.json();
      document.getElementById('blVal').textContent = s.bl;
      cvs.style.opacity = Math.max(0.15, s.bl / 255);
    }
  } catch (e) {}
  setTimeout(pollHw, 400);
}
pollHw();

function setHw() {
  const bat = document.getElementById('bat').value;
  document.getElementById('batVal').textContent = bat + '%';
  const chg = document.getElementById('chg').checked ? 1 : 0;
  const usb = document.getElementById('usb').checked ? 1 : 0;
  const hp  = document.getElementById('hp').checked ? 1 : 0;
  fetch(`/hw?bat=${bat}&chg=${chg}&usb=${usb}&hp=${hp}`).catch(()=>{});
}

let audCtx = null, audOn = false, nextTime = 0;
function toggleAudio() {
  audOn = !audOn;
  document.getElementById('audBtn').textContent = audOn ? 'Audio: ON' : 'Audio: OFF';
  if (audOn && !audCtx) {
    audCtx = new (window.AudioContext || window.webkitAudioContext)({sampleRate: 44100});
    nextTime = audCtx.currentTime;
    pumpAudio();
  }
}
async function pumpAudio() {
  if (!audOn) return;
  try {
    const r = await fetch('/audio.pcm', {cache:'no-store'});
    if (r.ok) {
      const ab = await r.arrayBuffer();
      if (ab.byteLength >= 4) {
        const s16 = new Int16Array(ab);
        const frames = Math.floor(s16.length / 2);
        const buf = audCtx.createBuffer(2, frames, 44100);
        const ch0 = buf.getChannelData(0), ch1 = buf.getChannelData(1);
        for (let i = 0; i < frames; i++) {
          ch0[i] = s16[i * 2] / 32768.0;
          ch1[i] = s16[i * 2 + 1] / 32768.0;
        }
        const src = audCtx.createBufferSource();
        src.buffer = buf;
        src.connect(audCtx.destination);
        const now = audCtx.currentTime;
        if (nextTime < now) nextTime = now + 0.02;
        src.start(nextTime);
        nextTime += buf.duration;
      }
    }
  } catch (e) {}
  setTimeout(pumpAudio, 40);
}

function tap(code) { fetch('/key?code=' + code).catch(()=>{}); }
document.addEventListener('keydown', (e) => {
  const map = { ArrowUp:103, ArrowDown:108, Enter:232, Escape:158, Backspace:158, ' ':164, ArrowLeft:105, ArrowRight:106 };
  if (map[e.key] !== undefined) { e.preventDefault(); tap(map[e.key]); }
});
cvs.addEventListener('wheel', (e) => { e.preventDefault(); tap(e.deltaY > 0 ? 108 : 103); }, {passive:false});
</script>
</body>
</html>
"""

def send_key(code):
    now = time.time()
    sec = int(now)
    usec = int((now - sec) * 1_000_000)
    ev_down = struct.pack("<IIHHi", sec, usec, 1, code, 1)
    ev_syn  = struct.pack("<IIHHi", sec, usec, 0, 0, 0)
    ev_up   = struct.pack("<IIHHi", sec, usec, 1, code, 0)
    try:
        fd = os.open(FIFO_PATH, os.O_WRONLY | os.O_NONBLOCK)
        os.write(fd, ev_down + ev_syn + ev_up + ev_syn)
        os.close(fd)
    except OSError:
        pass

def write_emu_file(name, val):
    try:
        with open(os.path.join(EMU_DIR, name), "w") as f:
            f.write(str(val) + "\n")
    except OSError:
        pass

def read_emu_int(name, default):
    try:
        with open(os.path.join(EMU_DIR, name), "r") as f:
            return int(f.read().strip())
    except Exception:
        return default

class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass
    def do_GET(self):
        if self.path == "/":
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.end_headers()
            self.wfile.write(HTML)
        elif self.path.startswith("/fb.bgra"):
            try:
                with open(FB_PATH, "rb") as f:
                    data = f.read(480 * 360 * 4)
            except OSError:
                data = b""
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.end_headers()
            self.wfile.write(data)
        elif self.path.startswith("/audio.pcm"):
            with audio_lock:
                data = b"".join(audio_chunks)
                audio_chunks.clear()
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.end_headers()
            self.wfile.write(data)
        elif self.path.startswith("/hw_state"):
            bl = read_emu_int("bl_brightness", 165)
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(json.dumps({"bl": bl}).encode())
        elif self.path.startswith("/hw?"):
            qs = dict(p.split("=", 1) for p in self.path.split("?", 1)[1].split("&") if "=" in p)
            if "bat" in qs: write_emu_file("bat_cap", qs["bat"])
            if "chg" in qs: write_emu_file("bat_status", "Charging" if qs["chg"] == "1" else "Discharging")
            if "usb" in qs: write_emu_file("usb_online", qs["usb"])
            if "hp"  in qs: write_emu_file("h2w_state", qs["hp"])
            self.send_response(204)
            self.end_headers()
        elif self.path.startswith("/key?code="):
            send_key(int(self.path.split("=")[1]))
            self.send_response(204)
            self.end_headers()
        else:
            self.send_response(404)
            self.end_headers()

if __name__ == "__main__":
    threading.Thread(target=audio_reader, daemon=True).start()
    print(f"Y1 QEMU Viewer running at http://127.0.0.1:{PORT}")
    http.server.ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()
