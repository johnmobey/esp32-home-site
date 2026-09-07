/*
 * CHAOS BOARD relay server
 * - Relays control messages between browsers and the ESP32
 * - Converts webcam JPEG frames into 128x64 1-bit XBM (bilinear + Bayer dither)
 * - Tracks ESP32 presence and broadcasts it to all browsers
 * - Rate limits clients, sanitizes free-text payloads, pings dead sockets
 */
const express = require('express');
const { WebSocketServer } = require('ws');
const path = require('path');
const http = require('http');
const jpeg = require('jpeg-js');

const OPEN = 1; // ws readyState

const app = express();
const server = http.createServer(app);
const wss = new WebSocketServer({ server, maxPayload: 512 * 1024 });

app.use(express.static(path.join(__dirname, 'public')));

let esp32Socket = null;

// ----------------------------------------------------------------
// Webcam frame pipeline (pure JS: decode -> bilinear 128x64 -> Bayer 4x4)
// ----------------------------------------------------------------
const BAYER = [
  [ 0,  8,  2, 10],
  [12,  4, 14,  6],
  [ 3, 11,  1,  9],
  [15,  7, 13,  5],
];
const DW = 128, DH = 64;
let camBusy = false;

function decodeResizeDither(buffer) {
  const img = jpeg.decode(buffer, { useTArray: true, formatAsRGBA: true });
  const sw = img.width, sh = img.height, src = img.data;
  const gray = new Float32Array(DW * DH);

  for (let y = 0; y < DH; y++) {
    const gy = (y + 0.5) * sh / DH - 0.5;
    let y0 = Math.floor(gy);
    const fy = gy - y0;
    y0 = Math.max(0, Math.min(sh - 1, y0));
    const y1 = Math.min(sh - 1, y0 + 1);
    for (let x = 0; x < DW; x++) {
      const gx = (x + 0.5) * sw / DW - 0.5;
      let x0 = Math.floor(gx);
      const fx = gx - x0;
      x0 = Math.max(0, Math.min(sw - 1, x0));
      const x1 = Math.min(sw - 1, x0 + 1);
      const i00 = (y0 * sw + x0) * 4, i10 = (y0 * sw + x1) * 4;
      const i01 = (y1 * sw + x0) * 4, i11 = (y1 * sw + x1) * 4;
      const l00 = src[i00] * 0.299 + src[i00 + 1] * 0.587 + src[i00 + 2] * 0.114;
      const l10 = src[i10] * 0.299 + src[i10 + 1] * 0.587 + src[i10 + 2] * 0.114;
      const l01 = src[i01] * 0.299 + src[i01 + 1] * 0.587 + src[i01 + 2] * 0.114;
      const l11 = src[i11] * 0.299 + src[i11 + 1] * 0.587 + src[i11 + 2] * 0.114;
      gray[y * DW + x] =
        l00 * (1 - fx) * (1 - fy) + l10 * fx * (1 - fy) +
        l01 * (1 - fx) * fy       + l11 * fx * fy;
    }
  }

  const out = Buffer.alloc(1024);
  let idx = 0;
  for (let y = 0; y < DH; y++) {
    for (let x = 0; x < DW; x += 8) {
      let byte = 0;
      for (let bit = 0; bit < 8; bit++) {
        const t = (BAYER[y & 3][(x + bit) & 3] + 0.5) * 16; // 8..248
        if (gray[y * DW + x + bit] > t) byte |= (1 << bit); // LSB-first XBM
      }
      out[idx++] = byte;
    }
  }
  return out;
}

function handleCamFrame(base64) {
  if (camBusy) return; // drop while busy: queue never builds up, latency stays low
  camBusy = true;
  setImmediate(() => {
    try {
      const buffer = Buffer.from(base64, 'base64');
      if (buffer.length >= 100 && buffer.length <= 300000) {
        const xbm = decodeResizeDither(buffer);
        if (esp32Socket && esp32Socket.readyState === OPEN) esp32Socket.send(xbm);
      }
    } catch (e) {
      // malformed jpeg — ignore frame
    } finally {
      camBusy = false;
    }
  });
}

// ----------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------
function sanitizeText(s, max) {
  return String(s).replace(/[^\x20-\x7E]/g, '').trim().slice(0, max);
}

function broadcastToWeb(data) {
  const str = JSON.stringify(data);
  wss.clients.forEach((c) => {
    if (c !== esp32Socket && c.readyState === OPEN) c.send(str);
  });
}

function espConnected(ws) {
  esp32Socket = ws;
  broadcastToWeb({ t: 'esp', online: true });
  console.log('ESP32 connected to relay');
}

function espDisconnected() {
  esp32Socket = null;
  broadcastToWeb({ t: 'esp', online: false });
  console.log('ESP32 disconnected');
}

const RATE_LIMIT = 30; // messages per second per client
function rateLimited(ws) {
  const sec = Math.floor(Date.now() / 1000);
  if (ws._rlSec !== sec) { ws._rlSec = sec; ws._rlCount = 0; }
  return ++ws._rlCount > RATE_LIMIT;
}

// ----------------------------------------------------------------
// Relay
// ----------------------------------------------------------------
wss.on('connection', (ws) => {
  ws.isAlive = true;
  ws.on('pong', () => { ws.isAlive = true; });

  ws.on('message', (data, isBinary) => {
    if (isBinary) return;                 // browsers send text only
    if (data.length > 262144) return;     // 256 KB cap
    if (rateLimited(ws)) return;

    const msgStr = data.toString();

    if (msgStr === 'ESP_AUTH') { espConnected(ws); return; }

    if (msgStr.startsWith('CAM_FRAME:')) {
      handleCamFrame(msgStr.replace(/^CAM_FRAME:(data:image\/jpeg;base64,)?/, ''));
      return;
    }

    // Sanitize free-text payloads before they reach the device
    if (msgStr.startsWith('POST:')) {
      const clean = sanitizeText(msgStr.slice(5), 60);
      if (clean && esp32Socket && esp32Socket.readyState === OPEN) {
        esp32Socket.send('POST:' + clean);
      }
      return;
    }
    if (msgStr.startsWith('GAMEOVER:')) {
      const m = msgStr.match(/^GAMEOVER:(\d{1,5}),(.{0,4})$/);
      if (m && esp32Socket && esp32Socket.readyState === OPEN) {
        esp32Socket.send('GAMEOVER:' + m[1] + ',' + sanitizeText(m[2], 4).toUpperCase());
      }
      return;
    }

    // Generic relay: browser -> ESP, ESP -> all browsers
    if (ws !== esp32Socket) {
      if (esp32Socket && esp32Socket.readyState === OPEN) esp32Socket.send(msgStr);
    } else {
      wss.clients.forEach((client) => {
        if (client !== esp32Socket && client.readyState === OPEN) client.send(msgStr);
      });
    }
  });

  ws.on('close', () => {
    if (ws === esp32Socket) espDisconnected();
  });
});

// Kill dead sockets (mobile sleep, dropped SSL) every 30s
const pingInterval = setInterval(() => {
  wss.clients.forEach((ws) => {
    if (ws.isAlive === false) { ws.terminate(); return; }
    ws.isAlive = false;
    ws.ping();
  });
}, 30000);

const PORT = process.env.PORT || 3000;
server.listen(PORT, () => console.log(`Chaos relay listening on port ${PORT}`));

// Graceful shutdown (Railway sends SIGTERM on redeploy)
function shutdown() {
  clearInterval(pingInterval);
  wss.clients.forEach((c) => { try { c.close(); } catch (e) {} });
  server.close(() => process.exit(0));
  setTimeout(() => process.exit(0), 3000);
}
process.on('SIGTERM', shutdown);
process.on('SIGINT', shutdown);