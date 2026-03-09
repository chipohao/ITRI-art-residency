# ToF Trajectory Visualization

VL53L0X 雙 ToF 感測器的即時軌跡視覺化工具。
支援 **軌跡拖尾 (Trail)** 和 **熱力圖 (Heatmap)** 兩種模式，並自動鏡像顯示（左右手對稱）。

---

## 架構

```
ESP32-C3 → Serial → Max/MSP [fromsymbol]
                         ↓
                    [send tof_viz] ──→ [receive tof_viz]
                                           ↓
                                   [node.script server.js]
                                           ↓  SSE (Server-Sent Events)
                                   瀏覽器 index.html (Canvas)
```

- **server.js**：Node for Max 腳本，接收 Max 訊息後透過 SSE 推送到瀏覽器
- **index.html**：單檔前端，Canvas 全螢幕即時繪製
- **零外部依賴**：不需要 `npm install`，SSE 不需要 WebSocket library

---

## Max 接法

### 方法 A：同一個 patch 裡

```
[fromsymbol] → [node.script server.js]
```

直接接就好，`/tof fX fY rX rY` 會自動對應到 server.js 的 `/tof` handler。

### 方法 B：跨 patch（visualization 資料夾有獨立 .maxpat）

主 patch：
```
[fromsymbol] → [send tof_viz]     ← 放在 [route /tof] 之前！
```

visualization patch：
```
[receive tof_viz] → [node.script server.js]
```

> **注意**：`[send tof_viz]` 必須在 `[route /tof]` **之前**。
> `[route]` 會把 `/tof` 前綴去掉，剩下的純數字 node.script 無法匹配。

---

## 使用方式

1. Max 啟動 serial + node.script
2. 瀏覽器開 **http://localhost:8080**
3. 開始移動感測器

---

## 鍵盤快捷鍵

| 按鍵 | 功能 |
|------|------|
| `1`  | 切換到 Trail（軌跡拖尾）模式 |
| `2`  | 切換到 Heatmap（熱力圖）模式 |
| `C`  | 清除所有軌跡 / 熱力資料 |
| `F`  | 切換全螢幕 |

---

## 可調參數一覽

所有參數都在 `index.html` 的 `<script>` 區塊頂部，搜尋變數名即可找到。

### 感測器範圍

| 參數 | 目前值 | 說明 |
|------|--------|------|
| `RANGE_X` | `320` | X 軸最大位移（mm）。Arduino 校正後輸出的 fX 最大值 |
| `RANGE_Y` | `220` | Y 軸最大位移（mm）。Arduino 校正後輸出的 fY 最大值 |
| `PAD` | `0.05` | 邊緣留白比例（0~0.5）。0.05 = 上下左右各留 5% 空白 |

> **座標方向**：
> - fX = 0 → 螢幕右邊緣（靠近 X 感測器），fX = RANGE_X → 中線
> - fY = 0 → 螢幕底部（靠近 Y 感測器），fY = RANGE_Y → 頂部
> - 左半邊為右半邊的鏡像

### 軌跡模式 (Trail)

| 參數 | 目前值 | 說明 |
|------|--------|------|
| `TRAIL_MAX` | `300` | 軌跡保留的最大點數。越大尾巴越長，記憶體用量也越高 |
| fade alpha | `0.08` | 每幀覆蓋的半透明黑色透明度（在 `drawTrail()` 裡）。越大淡出越快 |
| `velToColor` maxVel | `30` | 速度→顏色映射的最大速度值（mm/sample）。超過此值一律紅色 |

**顏色邏輯**：慢速 = 藍色 (hue 200)，快速 = 紅色 (hue 0)

### 熱力圖模式 (Heatmap)

| 參數 | 目前值 | 說明 | 調整建議 |
|------|--------|------|----------|
| `GRID` | `80` | 熱力圖解析度（80×80 格）。越大越精細，但 CPU 負擔越重 | 40~128 |
| `HEAT_DECAY` | `0.998` | 每幀衰減係數。越接近 1 熱力淡出越慢 | 0.990（快淡出）~ 0.9995（幾乎不淡） |
| `HEAT_ADD` | `0.15` | 每筆資料的熱力加權。越大，快速移動時軌跡也能看到 | 0.05（淡）~ 0.3（濃） |
| `HEAT_RADIUS` | `3` | 高斯擴散半徑（格數）。越大軌跡越粗 | 1（細線）~ 5（粗擴散） |

**熱力圖顏色漸層**：黑 → 藍 → 青 → 綠 → 黃 → 紅

**HEAT_DECAY 效果說明**：

| HEAT_DECAY | 半衰期 (幀數) | 60fps 時約等於 |
|------------|--------------|----------------|
| `0.990`    | ~69 幀       | ~1.2 秒         |
| `0.995`    | ~138 幀      | ~2.3 秒         |
| `0.998`    | ~346 幀      | ~5.8 秒         |
| `0.999`    | ~693 幀      | ~11.5 秒        |
| `0.9995`   | ~1386 幀     | ~23 秒          |

> 半衰期公式：`ln(0.5) / ln(HEAT_DECAY)`

### SSE / 網路

| 參數 | 目前值 | 位置 | 說明 |
|------|--------|------|------|
| `HTTP_PORT` | `8080` | server.js | HTTP + SSE 的 port。如有衝突可改其他 port |

---

## 檔案結構

```
visualization/
├── server.js       ← Node for Max 中繼（Max → SSE → 瀏覽器）
├── index.html      ← 前端視覺化（單檔，含 CSS + JS）
├── package.json    ← 專案描述（不需要 npm install）
└── README.md       ← 本文件
```

---

## 資料格式

### Max → server.js

```
/tof fX fY rX rY
```

- `fX`, `fY`：校正後的位移值（mm），非負整數
- `rX`, `rY`：原始距離讀數（mm），用於參考
- 頻率：約 50Hz（Arduino 端 20ms 間隔）

### server.js → 瀏覽器 (SSE)

```json
{"type":"tof","fX":123,"fY":45,"rX":456,"rY":789}
```

瀏覽器只使用 `fX` 和 `fY`。

---

## 常見調整情境

### 想要軌跡更長 / 更短
→ 調整 `TRAIL_MAX`（目前 300）

### 熱力圖淡出太快 / 太慢
→ 調整 `HEAT_DECAY`（目前 0.998，約 5.8 秒半衰期）

### 快速移動時熱力圖看不太到
→ 調大 `HEAT_ADD`（目前 0.15）或 `HEAT_RADIUS`（目前 3）

### 感測器實際範圍改變了
→ 調整 `RANGE_X` / `RANGE_Y` 為新的最大位移 mm 值

### 畫面太擠 / 邊緣被裁掉
→ 調整 `PAD`（目前 0.05 = 5% 留白）
