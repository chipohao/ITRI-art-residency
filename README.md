# ITRI Art Residency - Interactive Sensing Modules

## 專案簡介 | Project Overview

工研院藝術進駐專案，結合多種互動感測技術（壓電、脈搏、吹氣、ToF 距離、手部追蹤等），透過 ESP32 微控制器擷取感測數據，並以 Max/MSP、Python、WebSocket 等技術串接，實現即時互動藝術裝置。

An art residency project at ITRI (Industrial Technology Research Institute, Taiwan) combining multiple interactive sensing technologies — piezoelectric, pulse, breath, ToF distance, and hand tracking — using ESP32 microcontrollers, Max/MSP, Python, and WebSocket for real-time interactive art installations.

---

## 專案架構 | Project Structure

| Directory | 中文說明 | Description |
|---|---|---|
| `piezo_detect/` | 壓電感測基礎模組 | Basic piezoelectric sensor module |
| `piezo_detect_fft/` | 壓電感測 FFT 分析 | Piezoelectric sensor with FFT analysis |
| `duo_pulse/` | 雙人脈搏感測 | Dual pulse sensor (two-person heartbeat detection) |
| `duo_pulse_sonification/` | 脈搏聲音化模組 | Pulse sensor sonification with multiple firmware versions |
| `pressure_sensor/` | 壓力感測模組 | Pressure sensor module |
| `breath_sensor/` | 吹氣感測模組 | Breath/blow detection sensor module |
| `tof_distance_sensor/` | ToF 距離感測模組 | Time-of-Flight distance sensor module |
| `local_mediapipe/` | 本地端 MediaPipe 手部追蹤 | Local MediaPipe hand tracking |
| `remote_hand_tracking/` | 遠端手部追蹤（WebSocket） | Remote hand tracking via WebSocket |
| `legacy_local_cv_engine/` | 初版本地影像動態偵測引擎 | Legacy local computer vision engine |

---

## 技術棧 | Tech Stack

- **Microcontroller**: ESP32
- **Sensing**: Piezoelectric, Pulse Sensor, Breath Sensor, ToF (VL53L0X/VL53L1X), Pressure Sensor
- **Software**: Max/MSP, Python, JavaScript (Node.js)
- **Communication**: Serial, WebSocket, OSC (Open Sound Control)
- **Computer Vision**: MediaPipe (Hand Tracking)

## 硬體需求 | Hardware Requirements

- ESP32 開發板 / ESP32 Development Board
- 壓電感測器 / Piezoelectric Sensor
- 脈搏感測器 x2 / Pulse Sensor x2
- 吹氣感測器 / Breath Sensor
- ToF 距離感測器 (VL53L0X / VL53L1X) / ToF Distance Sensor
- 壓力感測器 / Pressure Sensor
- 網路攝影機（手部追蹤用）/ Webcam (for hand tracking)

## 各模組簡述 | Module Details

### piezo_detect
基礎壓電感測模組，ESP32 讀取壓電訊號並透過 Serial 傳送至 Max/MSP。
Basic piezo sensing: ESP32 reads piezoelectric signals and sends data to Max/MSP via Serial.

### piezo_detect_fft
在壓電感測的基礎上加入 FFT 頻譜分析功能。
Extends piezo detection with FFT (Fast Fourier Transform) spectral analysis in Max/MSP.

### duo_pulse
雙人脈搏感測模組，使用 ESP32 同時讀取兩個脈搏感測器，具備動態閥值與自動校正功能，支援 WiFi/OSC 傳輸。
Dual pulse sensor module using ESP32 with dynamic thresholding and auto-calibration. Supports WiFi/OSC output.

### duo_pulse_sonification
脈搏聲音化模組，將脈搏感測數據轉換為聲音輸出，包含多個韌體版本（v1.1 穩定版、v2.0 實驗版）與 Max/MSP patch。
Pulse sensor sonification module that converts heartbeat data into audio output. Includes multiple firmware versions (v1.1 stable, v2.0 playground) and Max/MSP patch.

### pressure_sensor
壓力感測模組，搭配 Max/MSP 進行雙手壓力偵測與視覺化。
Pressure sensor module with Max/MSP integration for two-hand pressure detection and visualization.

### breath_sensor
吹氣感測模組，偵測氣流強度。
Breath/blow detection sensor for measuring airflow intensity.

### tof_distance_sensor
ToF (Time-of-Flight) 距離感測模組，支援多種平滑模式與視覺化。
ToF distance sensor module with multiple smoothing modes and visualization options.

### local_mediapipe
使用 MediaPipe 在本地端進行手部追蹤，結合 Max/MSP 與網頁視覺化。
Local hand tracking using MediaPipe, integrated with Max/MSP and web-based visualization.

### remote_hand_tracking
透過 WebSocket 從遠端機器接收 MediaPipe 手部追蹤資料。
Receives MediaPipe hand tracking data from a remote machine via WebSocket.

### legacy_local_cv_engine
初版的本地影像動態偵測引擎，使用 Python OpenCV 進行動態偵測，由 Max/MSP 的 Node.js 腳本啟動。
Legacy local computer vision engine using Python OpenCV for motion detection, launched from Max/MSP via Node.js.

---

## 授權 | License

MIT License. See [LICENSE](LICENSE) for details.
