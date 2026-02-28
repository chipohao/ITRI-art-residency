# Duo Pulse — Sonification 設計筆記

## 系統架構（Max patch）

```
ESP32 (serial 115200)
    |
[serial] → parse → [route /pulse/userA/bpm /pulse/userA/beat ...]
                        |
              [prepend script setMaxBpmA]
                        |
                    [jweb heartbeats-harmony.html]   ← 視覺化 + 計算 pen 座標
                        |
              [route /pen/x /pen/y]
                    |        |
                  x 座標   y 座標  (-1.0 ~ +1.0)
```

---

## Pen Dot 座標說明

`heartbeats-harmony.html` 中，pen dot 以 Lissajous 方程式運動：

```
x = sin(freqA × t)          ← Heart A 驅動
y = sin(freqB × t + phase)  ← Heart B 驅動
```

- 輸出範圍：-1.0 ~ +1.0（正規化，**0 = 圖形正中央**）
- 輸出位址：`/pen/x`、`/pen/y`（約 20Hz，從 [jweb] outlet 送出）
- `freqA` = BPM_A / 60，`freqB` = BPM_B / 60

---

## 核心概念：過零點 = 心跳節奏

x 過零（負→正）的頻率 = Heart A 的心跳頻率本身
y 過零（負→正）的頻率 = Heart B 的心跳頻率本身

每次 upward zero crossing = 一個心跳周期完成。

### 兩軸過零點的關係 = Polyrhythm

| BPM 比例 | 效果 |
|----------|------|
| A : B = 1:1 | 兩軸同步，x 與 y 的 bang 對齊 |
| A : B = 2:3 | 每 2 次 x-bang 對 3 次 y-bang，穩定 polyrhythm |
| A : B = 非整數比 | 相位持續漂移，節奏不斷變化 |

---

## Max 接線：過零點偵測

### x 軸（Heart A 節奏）

```
[jweb]
    |
[route /pen/x]
    |
[> 0]          ← 正=1，負=0
    |
[change]       ← 只在值改變時輸出
    |
[sel 1]        ← 只抓 upward crossing（負→正）
    |
[bang]         ← 每個 Heart A 週期觸發一次
```

### y 軸（Heart B 節奏）同上，換成 `/pen/y`

---

## 聲音化映射建議

| 參數 | 計算方式 | 映射目標 |
|------|----------|----------|
| x | 直接用 | 立體聲 pan（左右空間） |
| y | 直接用 | filter cutoff 或 pitch |
| radius | `sqrt(x²+y²)` | 音量、reverb mix |
| angle | `atan2(y, x)` | 連續音高或 LFO 速率 |
| x upward zero crossing | 見上方接線 | 觸發音符（Heart A 節奏） |
| y upward zero crossing | 同上換 y | 觸發音符（Heart B 節奏） |
| 速度 | 連續兩個 x 值相減 `[- ]` | granular 密度 |

### 最簡單的起點

```
x  → [scale -1 1 -1 1] → stereo pan
radius = [sqrt] after [* x x] + [* y y] → amplitude
```

### 最有趣的起點

兩軸 upward zero crossing 各觸發不同音色，
節奏結構完全由兩顆心跳的 BPM 比例決定——
比例越接近整數比，越規律；越偏離，越複雜。

---

## jweb 使用方式

1. Max patch 新增 `[jweb]` 物件
2. 送 `[loadbang]` → `[read <絕對路徑>/heartbeats-harmony.html]` → `[jweb]`
3. 視窗縮放：送 `[zoom 0.8]` → `[jweb]`（等比縮小至 80%）
4. 頁面偵測到 Max 環境後自動進入 **max mode**（隱藏 serial 控制項）

### Max → jweb（送 BPM / beat / finger）

```
[prepend script setMaxBpmA]   → [jweb]   ← 更新 Heart A 的 BPM
[prepend script setMaxBpmB]   → [jweb]   ← 更新 Heart B 的 BPM
[prepend script setMaxBeatA]  → [jweb]   ← 觸發 Heart A 的 beat flash
[prepend script setMaxBeatB]  → [jweb]   ← 觸發 Heart B 的 beat flash
[prepend script setMaxFingerA 1] → [jweb] ← 設定手指狀態（0 或 1）
[prepend script setMaxFingerB 1] → [jweb]
```

### jweb → Max（pen 座標）

```
[jweb] outlet
    |
[route /pen/x /pen/y]
```
