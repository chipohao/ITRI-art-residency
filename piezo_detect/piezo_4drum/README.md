# Piezo 4-Drum

ESP32-C3 SuperMini 四通道壓電感測器，用於敲擊偵測。

## 硬體接線

| 功能 | GPIO | 說明 |
|------|------|------|
| Piezo 1 | GPIO0 | 壓電感測器 + 1M ohm to GND |
| Piezo 2 | GPIO1 | 壓電感測器 + 1M ohm to GND |
| Piezo 3 | GPIO3 | 壓電感測器 + 1M ohm to GND |
| Piezo 4 | GPIO4 | 壓電感測器 + 1M ohm to GND |
| LED 1 | GPIO5 | LED + 220 ohm |
| LED 2 | GPIO6 | LED + 220 ohm |
| LED 3 | GPIO7 | LED + 220 ohm |
| LED 4 | GPIO10 | LED + 220 ohm |
| Button | GPIO2 | 校正按鈕 (press = LOW) |

## Serial 輸出格式

```
/piezo <channel> <velocity> <rawPeak>
```

- `channel`: 1~4
- `velocity`: 1~127（從 peak 值映射）
- `rawPeak`: 原始 ADC 值 (0~4095)

範例：
```
/piezo 1 88 3084
/piezo 3 112 3560
```

## 裝置識別（WHO Protocol）

電腦端發送 `WHO\n`，ESP32 回覆 `ID:piezo`。

用於 USB Hub 接多個 ESP32 時自動辨識 Serial Port 對應的裝置。

## 演算法特性

- **Dynamic baseline tracking** — 自動追蹤環境基線，適應震動變化
- **IIR filter** — 低通濾波降噪
- **Consecutive hit debounce** — 連續超過門檻才觸發，避免誤判
- **Non-blocking peak capture** — 15ms 內掃描峰值，不阻塞 loop
- **Non-blocking LED flash** — LED 閃爍不影響偵測
- **Serial buffer overflow protection** — TX buffer 快滿時跳過輸出，防止長時間運行後延遲

## 可調參數

| 參數 | 預設值 | 說明 |
|------|--------|------|
| `THRESHOLD_DELTA` | 80 | 超過 baseline 多少觸發 |
| `MAX_READING` | 3000 | velocity 映射的最大 ADC 值 |
| `SCAN_TIME_MS` | 15 | 峰值掃描時間 (ms) |
| `COOLDOWN_MS` | 120 | 同通道觸發冷卻 (ms) |
| `HITS_REQUIRED` | 2 | 連續幾次超門檻才觸發 |
| `LED_ON_MS` | 30 | LED 亮燈時間 (ms) |
