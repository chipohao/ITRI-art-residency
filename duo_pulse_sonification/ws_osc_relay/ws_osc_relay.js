/**
 * ws_osc_relay.js
 * WebSocket → OSC UDP 中繼
 *
 * 瀏覽器（heartbeats-harmony.html）把 pen dot x/y 座標
 * 透過 WebSocket 送到這裡，轉成 OSC binary UDP 傳給 Max/MSP。
 *
 * 使用方式：
 *   cd ws_osc_relay && npm install && node ws_osc_relay.js
 *
 * Max/MSP 接收：
 *   [udpreceive 9000]
 *       |
 *   [OSCroute /pen/x /pen/y]
 *       |           |
 *      x (-1~+1)  y (-1~+1)
 *
 * 設定說明：
 *   WS_PORT  — 瀏覽器連線的 WebSocket port（預設 8080）
 *   OSC_HOST — Max/MSP 所在 IP（同台電腦用 127.0.0.1）
 *   OSC_PORT — Max [udpreceive] 監聽的 port（預設 9000）
 */

const WebSocket = require('ws');
const dgram     = require('dgram');

const WS_PORT  = 8080;
const OSC_HOST = '127.0.0.1';
const OSC_PORT = 9000;

// ── OSC binary formatter (無外部套件) ────────────────────────────────────
// OSC string: null-terminated, padded to 4-byte boundary
function oscStrBuf(s) {
  const raw = s + '\0';
  const len = raw.length % 4 === 0 ? raw.length : raw.length + (4 - raw.length % 4);
  const buf = Buffer.alloc(len, 0);
  buf.write(raw, 'ascii');
  return buf;
}

// Build OSC message: address (string) + type tag ",f" + float32 value
function oscFloat(addr, val) {
  const addrBuf = oscStrBuf(addr);
  const typeBuf = oscStrBuf(',f');
  const valBuf  = Buffer.allocUnsafe(4);
  valBuf.writeFloatBE(val, 0);
  return Buffer.concat([addrBuf, typeBuf, valBuf]);
}

// ── UDP socket ────────────────────────────────────────────────────────────
const udp = dgram.createSocket('udp4');

function sendOsc(addr, val) {
  const pkt = oscFloat(addr, val);
  udp.send(pkt, OSC_PORT, OSC_HOST, err => {
    if (err) console.error('[udp] send error:', err.message);
  });
}

// ── WebSocket server ──────────────────────────────────────────────────────
const wss = new WebSocket.Server({ port: WS_PORT });

wss.on('connection', ws => {
  console.log('[ws] browser connected');

  ws.on('message', data => {
    // 格式: "/pen/x 0.7503\n/pen/y -0.3201\n"
    String(data).split('\n').forEach(line => {
      line = line.trim();
      if (!line) return;
      const m = line.match(/^(\/\S+)\s+([-\d.]+)/);
      if (!m) return;
      sendOsc(m[1], parseFloat(m[2]));
    });
  });

  ws.on('close', () => console.log('[ws] browser disconnected'));
  ws.on('error', () => {});
});

wss.on('listening', () => {
  console.log(`WebSocket server : ws://localhost:${WS_PORT}`);
  console.log(`OSC UDP → Max    : ${OSC_HOST}:${OSC_PORT}`);
  console.log('Waiting for browser connection...');
});
