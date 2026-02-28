# VL53L0X 雙 ToF 距離感測 — 優化版

ESP32 + 2x VL53L0X ToF 感測器，用於平面同動車 X/Y 軸手把追蹤。
Serial 輸出 OSC 風格格式，可直接接入 MaxMSP。

## 硬體接線

```
ESP32           VL53L0X #1 (X軸)    VL53L0X #2 (Y軸)
─────           ────────────────    ────────────────
GPIO 21 (SDA) ──── SDA ──────────── SDA
GPIO 22 (SCL) ──── SCL ──────────── SCL
3.3V ──────────── VIN ──────────── VIN
GND ───────────── GND ──────────── GND
GPIO 25 ───────── XSHUT
GPIO 26 ──────────────────────────── XSHUT
```

> 兩顆感測器共用 I2C 匯流排，透過 XSHUT 腳位依序啟動並重新分配地址（0x30, 0x31）。

## 安裝依賴

在 Arduino IDE 中安裝：

1. **Library Manager** → 搜尋 `Adafruit VL53L0X` → 安裝
2. 同時會自動安裝 `Adafruit BusIO` 等依賴

Board 選擇 ESP32 Dev Module。

## 校正流程

1. 上電後自動進入校正：
   - **前 500ms**：暖機（丟棄讀數）
   - **接下來 5 秒**：收集樣本，請將手把放在**原點位置**不動
2. 校正以收集樣本的**中位數**作為 offset（比最小值更抗雜訊）
3. 校正完成後自動進入正常量測模式

### 重新校正

透過 Serial 發送字元 `c` 即可重新觸發校正流程。

## Serial 輸出格式（OSC 風格）

Baud rate: **115200**

| 訊息 | 說明 |
|------|------|
| `/tof <filtered_x> <filtered_y> <raw_x> <raw_y>` | 正常量測（50Hz） |
| `/status calibrating <seconds_remaining>` | 校正倒數中 |
| `/cal_done <offsetX> <offsetY> <samplesX> <samplesY>` | 校正完成 |
| `/status running` | 開始正常運行 |
| `/status filters_reset` | 濾波器已重置 |
| `/error <message>` | 錯誤訊息 |

## Serial 指令

| 指令 | 功能 |
|------|------|
| `c` | 重新校正（暖機 + 5秒收集） |
| `r` | 重啟 EMA 濾波器（清除歷史，保留 offset） |

## MaxMSP Patch 接線建議

```
[serial /dev/cu.usbserial-XXXX 115200]
        |
    [sel 10]          ← 以換行符號分割訊息
        |
   [fromsymbol]       ← 字串轉為 symbol list
        |
  [route /tof /status /cal_done /error]
     |      |         |          |
[unpack 0 0 0 0]    ...        ...     ← /tof: filtered_x, filtered_y, raw_x, raw_y
     |   |
   [位移控制]
```

### 進階：分離 filtered 與 raw

```
[route /tof]
     |
[unpack 0 0 0 0]
 |  |    |  |
 |  |  [pack 0 0] → raw_x raw_y（除錯用）
[pack 0 0] → filtered_x filtered_y（控制用）
```

## 參數調整

在 `.ino` 檔案頂部可調整以下參數：

| 參數 | 預設值 | 說明 |
|------|--------|------|
| `TIMING_BUDGET_US` | 33000 (33ms) | 感測器量測時間。越大越精確但越慢 |
| `EMA_ALPHA` | 0.3 | EMA 平滑係數。越大越跟手、越小越平滑 |
| `MEDIAN_WINDOW` | 5 | 中位數濾波器視窗。越大越抗突波但延遲越高 |
| `OUTPUT_INTERVAL_MS` | 20 | 輸出間隔（20ms = 50Hz） |
| `WARMUP_MS` | 500 | 暖機時間 |
| `CALIB_MS` | 5000 | 校正收集時間 |
| `RANGE_MAX` | 2000 | 有效量測範圍上限 (mm) |

### 調整建議

- **手把反應太慢**：提高 `EMA_ALPHA`（如 0.5~0.8）、縮小 `MEDIAN_WINDOW`（如 3）
- **輸出太抖**：降低 `EMA_ALPHA`（如 0.1~0.2）、加大 `MEDIAN_WINDOW`（如 7）
- **感測器讀值不穩定**：加大 `TIMING_BUDGET_US`（如 50000）
- **需要更高輸出速率**：縮小 `OUTPUT_INTERVAL_MS`（但不要低於 timing budget 的兩倍）
