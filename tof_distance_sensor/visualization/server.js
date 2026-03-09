/**
 * ToF Visualization — Node for Max version
 *
 * Max 接法（超簡單）：
 *   [fromsymbol] → [node.script server.js]
 *   不需要 route 或 prepend，直接把 /tof /status /cal_done 丟進來就好
 *
 * 瀏覽器開 http://localhost:8080
 * 資料透過 SSE (Server-Sent Events) 推送，零外部依賴。
 */

const maxApi = require('max-api');
const http = require('http');
const fs = require('fs');
const path = require('path');

const HTTP_PORT = 8080;

// ── SSE clients ────────────────────────────────────────────────
let sseClients = [];

function broadcast(data) {
  const payload = `data: ${JSON.stringify(data)}\n\n`;
  sseClients = sseClients.filter((res) => {
    try {
      res.write(payload);
      return true;
    } catch (e) {
      return false; // client disconnected
    }
  });
}

// ── HTTP server ────────────────────────────────────────────────
const server = http.createServer((req, res) => {
  if (req.url === '/events') {
    // SSE endpoint
    res.writeHead(200, {
      'Content-Type': 'text/event-stream',
      'Cache-Control': 'no-cache',
      'Connection': 'keep-alive',
      'Access-Control-Allow-Origin': '*'
    });
    res.write('\n'); // flush headers
    sseClients.push(res);
    maxApi.post(`[SSE] client connected (${sseClients.length} total)`);

    req.on('close', () => {
      sseClients = sseClients.filter((c) => c !== res);
      maxApi.post(`[SSE] client disconnected (${sseClients.length} total)`);
    });
    return;
  }

  // Serve index.html
  const filePath = path.join(__dirname, 'index.html');
  fs.readFile(filePath, (err, data) => {
    if (err) {
      res.writeHead(500);
      res.end('Error loading index.html');
      return;
    }
    res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
    res.end(data);
  });
});

server.listen(HTTP_PORT, () => {
  maxApi.post(`[HTTP] http://localhost:${HTTP_PORT}`);
  maxApi.post('Ready! 開瀏覽器即可看到視覺化');
});

// ── Receive data from Max ──────────────────────────────────────
// [fromsymbol] 直接送進來：/tof fX fY rX rY [vX vY]
// handler 名稱用 "/tof"，跟 Serial 輸出的第一個字一致，不用 route 或 prepend
// vX, vY 為可選（速度 mm/s），相容 tof_c3_supermini（無速度）和 tof_c3_supermini_vel（有速度）
maxApi.addHandler('/tof', (...args) => {
  broadcast({
    type: 'tof',
    fX: args[0],
    fY: args[1],
    rX: args[2],
    rY: args[3],
    vX: args[4] !== undefined ? args[4] : 0,
    vY: args[5] !== undefined ? args[5] : 0
  });
});

// /status calibrating 4, /status running, etc.
maxApi.addHandler('/status', (...args) => {
  broadcast({ type: 'status', args });
});

// /cal_done offsetX offsetY samplesX samplesY
maxApi.addHandler('/cal_done', (...args) => {
  broadcast({ type: 'cal_done', args });
});
