# Duo Pulse - Dual Heartbeat Interaction System
# 雙人脈搏互動系統 (2025工研院藝術進駐計畫)

## 1. Project Overview (專案簡介)
**Duo Pulse** is an interactive bio-feedback system designed to capture the heartbeats of two participants simultaneously. By translating physiological signals into digital data, the system enables real-time generative art, soundscapes, or rhythmic interplay. 
**Duo Pulse** 是一個雙人生物回饋互動系統，旨在同時捕捉兩位參與者的心跳數據。透過將生理訊號轉化為數位資訊，本系統能驅動即時的生成式藝術、音景或節奏互動。

### Purpose & Goals (目的與目標)
*   **Physiological Connection**: visualization and sonification of the invisible bond.
*   **Smart Detection**: Uses **Rolling Min/Max Adaptive Algorithm** to adapt to different signal strengths.
*   **Stability**: Robust Serial communication for performance environments.

---

## 2. Technical Specifications (技術規格)

*   **Microcontroller**: ESP32 (Dual-core, 240MHz)
*   **Input**: 2x Analog Pulse Sensors (ADC1)
*   **Algorithm**: **Rolling Min/Max Adaptive Threshold** (動態範圍追蹤)
*   **Communication**: 
    *   **Serial Only** (Default): 115200 baud.
    *   **WiFi (OSC)**: Disabled by default (Uncomment `#define ENABLE_WIFI` in code to enable).

---

## 3. Hardware Setup (硬體接線)

**Use ADC1 pins to allow potential WiFi usage later.**
**請使用 ADC1 腳位，保留未來擴充 WiFi 的可能性。**

| Component | Pin | ESP32 GPIO | Note (說明) |
| :--- | :--- | :--- | :--- |
| **User A Pulse Signal** | **VP** or **D34** | GPIO 36 / 34 | Analog Input (ADC1) |
| **User B Pulse Signal** | **VN** or **D35** | GPIO 39 / 35 | Analog Input (ADC1) |
| **VCC** | **3.3V** | - | Power |
| **GND** | **GND** | - | Ground |

---

## 4. Signal Processing Algorithm (訊號處理演算法)

### Version 3.0: Rolling Min/Max Adaptive Threshold (滾動式極值追蹤)

Previous fixed-threshold methods failed when DC offset drifted (e.g., signal floating between 1700-2000). The new smart algorithm works as follows:
舊版的固定閥值法在訊號偏移時容易失效。新的演算法運作原理如下：

1.  **Finger Detection (手指偵測)**: 
    *   If raw signal < `500`: System outputs `0` (Silence). Indicates no finger attached.
    *   若數值小於 500，判斷為無手指，輸出 0。

2.  **Adaptive Range Tracking (動態範圍追蹤)**:
    *   The system continuously tracks the **Maximum** and **Minimum** values of the waveform.
    *   **Decay Logic**: The Max slowly decreases, and the Min slowly increases over time. This ensures the range adapts to the *current* waveform and doesn't get stuck on old peaks.
    *   系統持續追蹤波形的「最高點」與「最低點」。這兩個極值會隨著時間慢慢衰減 (Max下降/Min上升)，確保範圍能貼合當下的波形，不會被卡在舊的雜訊上。

3.  **Dynamic Threshold Calculation (動態閥值計算)**:
    *   `Current Threshold` = `(Max + Min) / 2`
    *   The detection line is always placed exactly in the **middle** of the wave. Even if the wave is weak (e.g., Amplitude 100) or offset (e.g., 2000-2100), the threshold will find the center (2050).
    *   偵測閥值永遠設在波形的正中間。即使波形很弱或位置偏移，系統也能抓到中心點。

4.  **Beat Trigger (心跳觸發)**:
    *   **Beat ON**: Signal rises above `Threshold + Hysteresis`.
    *   **Beat OFF**: Signal falls below `Threshold - Hysteresis`.

---

## 5. Data Transmission (數據傳輸)

### Output Format (輸出格式)
Serial Output example:
\`\`\`
/pulse/userA/raw 1024
/pulse/userA/beat 1
/pulse/userA/bpm 72
\`\`\`

| OSC Address | Value | Description |
| :--- | :--- | :--- |
| **/pulse/userA/raw** | 0-4095 | Raw Waveform (0 if no finger) |
| **/pulse/userA/beat** | 0 / 1 | Beat Trigger (1=Peak, 0=End) |
| **/pulse/userA/bpm** | float | Heart Rate (Updated on beat) |

### Max/MSP Reception (接收方式)
Use **Serial** object (`[serial c 115200]`).

**Routing Chain (接線圖):**
\`\`\`
[serial c 115200] 
   |
[sel 13 10] 
   |
[zl group 1000] 
   |
[itoa] 
   |
[fromsymbol] 
   |
[route /pulse/userA /pulse/userB]
   | (User A)                          | (User B)
   |                                   |
[route raw bpm beat]                [route raw bpm beat]
   |    |    |                         |    |    |
raw_val bpm  0/1                      raw  bpm  0/1
\`\`\`

---

## 6. Creative Strategies (創作策略建議)

*   **Phasing**: Use `/beat` triggers to create Steve Reich-style phase shifts.
*   **Euclidean Rhythms**: Use BPM to control density of a pattern.
*   **Clock Division**: Use BPM to drive different clock speeds (e.g. 4:3 polyrhythm).
*   **Visual Bio-Feedback**: Map `raw` to size, `bpm` to color.
