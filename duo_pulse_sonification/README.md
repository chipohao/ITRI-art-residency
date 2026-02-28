# Duo Pulse - Dual Heartbeat Interaction System
# 雙人脈搏互動系統 (2025工研院藝術進駐計畫)

## 1. Project Overview (專案簡介)

**Duo Pulse** is an interactive bio-feedback system that captures the heartbeats of two participants simultaneously. By translating physiological signals into digital data, the system enables real-time generative art, soundscapes, and rhythmic interplay.

**Duo Pulse** 是一個雙人生物回饋互動系統，同時捕捉兩位參與者的心跳數據，驅動即時生成式藝術、音景與節奏互動。

### Files (檔案說明)

| File | Description |
| :--- | :--- |
| `duo_pulse_v1.1_stable/` | v1.1 — Rolling Min/Max adaptive threshold (穩定版) |
| `duo_pulse_v2.0_playground/` | **v2.3** — Three-state calibration, auto threshold, finger detection |
| `heartbeats-harmony.html` | Visual demo: Lissajous sand drawing driven by two BPMs (Web Serial) |

---

## 2. Technical Specifications (技術規格)

- **Microcontroller**: ESP32 (Dual-core, 240MHz)
- **Input**: 2× Analog Pulse Sensors (ADC1, GPIO 34 & 35)
- **ADC Attenuation**: `ADC_11db` — full-scale 0–3.9V, required for PPG DC bias (~1.6–2.0V)
- **Sample Rate**: 500Hz (via `micros()`)
- **Algorithm**: Three-state auto-calibration (v2.3)
- **Communication**: Serial 115200 baud (default) / WiFi OSC (optional, `#define ENABLE_WIFI`)

---

## 3. Hardware Setup (硬體接線)

| Component | ESP32 Pin | Note |
| :--- | :--- | :--- |
| Sensor A signal | GPIO 34 (D34) | ADC1, analog input |
| Sensor B signal | GPIO 35 (D35) | ADC1, analog input |
| VCC | 3.3V | Do NOT use 5V |
| GND | GND | — |

---

## 4. Signal Processing Algorithm — v2.3 (訊號處理演算法)

### Three-State Machine (三段式狀態機)

```
WAITING → (finger placed) → CALIBRATING (3 sec) → RUNNING
                ↑                                      |
                └──────── (finger removed) ────────────┘
```

**WAITING**: Signal < `FINGER_THRESH` (300). No output.

**CALIBRATING** (3 seconds after finger placed):
- Collects baseline (mean) and peak amplitude over 3 seconds.
- Outputs `/finger 1` immediately on finger detection.
- `threshold = baseline + amplitude × 0.4`

**RUNNING**:
- Slow baseline tracking: `baseline += (sig − baseline) × 0.0004` (τ ≈ 5 s)
  Only updates when signal is below threshold (avoids peak pulling baseline up).
- Beat detection with refractory period (350ms, max ~171 BPM) and 200ms gate.

---

## 5. Data Transmission (數據傳輸)

### Serial Output Format (輸出格式)

```
/pulse/userA/raw 1812
/pulse/userA/finger 1
/pulse/userA/beat 1
/pulse/userA/bpm 72
/pulse/userA/thresh 1940
```

| Address | Value | Description |
| :--- | :--- | :--- |
| `/pulse/userA/raw` | 0–4095 | Smoothed ADC value (100Hz throttle) |
| `/pulse/userA/finger` | 0 / 1 | Finger detection (1=on, 0=removed) |
| `/pulse/userA/beat` | 0 / 1 | Beat pulse (1=onset, 0=end after 200ms) |
| `/pulse/userA/bpm` | int | BPM, output on each beat |
| `/pulse/userA/thresh` | int | Current threshold (debug) |

Same addresses for `userB`.

### Max/MSP Reception (Max 接收方式)

**注意**：`[route]` 比對的是整個 selector，`/pulse/userA/bpm 72` 的 selector 是 `/pulse/userA/bpm`，不是 `/pulse/userA`，所以無法做兩層分拆。

**方法 A：直接 route 完整路徑（建議）**

```
[serial c 115200]
   |
[sel 10]                          ← 偵測換行
   |
[zl group 500]                    ← 收集成整數 list
   |
[itoa]                            ← 轉 ASCII symbol
   |
[fromsymbol]                      ← 轉成 Max message
   |
[route /pulse/userA/raw /pulse/userA/beat /pulse/userA/bpm /pulse/userA/finger
       /pulse/userB/raw /pulse/userB/beat /pulse/userB/bpm /pulse/userB/finger]
   |        |       |          |            |        |       |          |
raw_A  beat_A  bpm_A  finger_A          raw_B  beat_B  bpm_B  finger_B
```

**方法 B：OSCroute（需安裝 CNMAT 套件，支援前綴比對）**

```
[fromsymbol]
   |
[OSCroute /pulse/userA /pulse/userB]
       |                      |
[OSCroute /raw /beat /bpm /finger]
```

### Web Serial (Chrome / Edge 直接讀取)

Open `heartbeats-harmony.html` in Chrome, click **Scan Port**, select the ESP32 port.
The page auto-parses `/bpm` and `/finger` and updates the Lissajous animation in real time.

---

## 6. Heartbeats in Harmony — Visual Demo

`heartbeats-harmony.html` — p5.js sand-drawing Lissajous figure driven by two heartbeats.

**Controls:**
- **Heart A / B**: BPM sliders (40–160), also updated live via Web Serial
- **Phase**: phase offset between the two oscillators
- **Trail**: fade-out half-life (∞ = infinite accumulation)
- **Scan Port**: connect ESP32 via Web Serial (Chrome/Edge only)
- **Clear**: reset the drawing

**Finger indicator**: a small dot below each heart label — dim = no finger, green = finger detected.

**Run locally:**
```bash
# Any simple HTTP server works; file:// also works in recent Chrome
python3 -m http.server 8080
# then open http://localhost:8080/duo_pulse_sonification/heartbeats-harmony.html
```

---

## 7. Creative Strategies (創作策略建議)

- **Phasing**: Use `/beat` triggers for Steve Reich-style phase shifts.
- **Lissajous Harmony**: Two BPMs as integer ratios (3:2, 4:3) produce closed Lissajous figures.
- **Euclidean Rhythms**: BPM controls pattern density.
- **Bio-Feedback Visuals**: Map `/raw` to size, `/bpm` to color, `/finger` to on/off gating.
