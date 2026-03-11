/*
 * Pressure Sensor 2 Hands — ESP32-C3 SuperMini + FSR x2 + WS2812 x2
 *
 * 功能：
 *   1. 雙 FSR 壓力感測，10 秒校正（抓 min/max），DEFAULT_MAX 保底
 *   2. 正常模式動態擴展上限（壓超過校正 max 自動更新）
 *   3. 校正期 LED 呼吸白燈（sine 漸變，不歸零）
 *   4. 閒置偵測 → 淡藍呼吸燈待機，壓力一來立刻恢復
 *   5. 按鈕（GPIO4）按下 → 重新校正（不中斷 Serial）
 *   6. 壓力值 0.0~1.0 浮點數輸出，LED 顆數隨 norm 線性對應
 *
 * ── 硬體接線（ESP32-C3 SuperMini）─────────────────────
 *   FSR1 訊號  → GPIO0     壓力感測器 1（分壓中點）
 *   FSR2 訊號  → GPIO1     壓力感測器 2（分壓中點）
 *   LED Ring 1 → GPIO5     WS2812 燈圈 1 DIN
 *   LED Ring 2 → GPIO6     WS2812 燈圈 2 DIN
 *   BUTTON     → GPIO4     外接按鈕（按下 = LOW，重新校正）
 *   FSR 電源   → 3.3V      分壓電路上端
 *   LED 電源   → 5V 外部   WS2812 VCC
 *   共地       → GND       所有模組 GND
 *
 * ── Serial 輸出格式 ──────────────────────────────────
 *   /pressure norm1 norm2 raw1 raw2     正常壓力資料（norm 0.0~1.0，50ms 間隔）
 *   /status calibrating N               校正倒數（每秒）
 *   /status calibrated                  校正完成
 *   /status recalibrating               按鈕觸發重新校正
 *   /status idle                        進入閒置待機
 *   /status active                      從閒置恢復
 */

#include <Adafruit_NeoPixel.h>

// =========================
// Pin 設定
// =========================
#define FSR1_PIN     0
#define LED1_PIN     5
#define FSR2_PIN     1
#define LED2_PIN     6
#define BUTTON_PIN   4
#define NUM_LEDS     12

// =========================
// LED 物件
// =========================
Adafruit_NeoPixel ring1(NUM_LEDS, LED1_PIN, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel ring2(NUM_LEDS, LED2_PIN, NEO_GRB + NEO_KHZ800);

// =========================
// 校正設定
// =========================
const unsigned long CALIBRATION_TIME = 10000;        // 10 秒校正
const int SAMPLE_DELAY_MS = 20;
const int SAFE_MARGIN = 80;                          // 零區安全範圍
const int DEFAULT_MAX = 2500;                        // 預設上限保底
const unsigned long PRESSURE_OUTPUT_INTERVAL = 50;   // 正常輸出間隔 (ms)
const unsigned long CAL_PRINT_INTERVAL = 1000;       // 校正倒數每秒輸出

// =========================
// 呼吸燈設定
// =========================
const int CAL_BREATHE_PERIOD  = 2000;   // 校正呼吸週期 (ms)
const int CAL_BREATHE_MIN     = 9;      // 校正最低亮度 (~15% of 60)
const int CAL_BREATHE_MAX     = 60;     // 校正最高亮度
const int IDLE_BREATHE_PERIOD = 3000;   // 待機呼吸週期 (ms)
const int IDLE_BREATHE_MIN    = 4;      // 待機最低亮度
const int IDLE_BREATHE_MAX    = 40;     // 待機最高亮度

// =========================
// 閒置偵測
// =========================
const unsigned long IDLE_TIMEOUT = 10000;  // 10 秒無壓力 → 待機
unsigned long lastActiveTime = 0;
bool isIdle = false;

// =========================
// FSR 校正資料結構
// =========================
struct FSRChannel {
  int fsrPin;
  Adafruit_NeoPixel* ring;
  int calibratedMin;
  int calibratedMax;
  int zeroThreshold;
};

FSRChannel ch1 = {FSR1_PIN, &ring1, 4095, 0, 0};
FSRChannel ch2 = {FSR2_PIN, &ring2, 4095, 0, 0};

// =========================
// 狀態控制
// =========================
bool isCalibrating = true;
unsigned long calibrationStartTime = 0;
unsigned long lastPressureOutputTime = 0;
unsigned long lastCalibrationPrintTime = 0;
int lastPrintedRemainSec = -1;

// =========================
// 按鈕去彈跳
// =========================
bool lastButtonReading = HIGH;
bool buttonState = HIGH;
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 30;

// =========================
// 讀取平均 ADC
// =========================
int readFSRRawAveraged(int pin, int samples = 8) {
  long sum = 0;
  for (int i = 0; i < samples; i++) {
    sum += analogRead(pin);
    delay(2);
  }
  return (int)(sum / samples);
}

// =========================
// 呼吸燈亮度計算（sine 漸變）
// =========================
int breatheBrightness(int period, int minB, int maxB) {
  float phase = (millis() % period) / (float)period * TWO_PI;
  float norm = 0.5 + 0.5 * sin(phase - HALF_PI);  // 0.0 ~ 1.0，從最暗開始
  return minB + (int)(norm * (maxB - minB));
}

// =========================
// raw 轉成 0.0~1.0 浮點數
// =========================
float rawToNorm(int raw, int zeroThreshold, int calibratedMax) {
  if (raw <= zeroThreshold) return 0.0;
  if (raw >= calibratedMax) return 1.0;
  int span = calibratedMax - zeroThreshold;
  if (span <= 0) return 0.0;
  float norm = (float)(raw - zeroThreshold) / (float)span;
  return constrain(norm, 0.0, 1.0);
}

// =========================
// norm 轉成 LED 顆數（0.0 → 1 顆, 1.0 → 12 顆）
// =========================
int normToLedCount(float norm) {
  int count = 1 + (int)(norm * 11);
  if (count < 1) count = 1;
  if (count > 12) count = 12;
  return count;
}

// =========================
// 顯示壓力（藍色，norm 對應 LED 顆數）
// =========================
void showPressure(Adafruit_NeoPixel &ring, float norm) {
  ring.setBrightness(60);
  int ledCount = normToLedCount(norm);
  uint32_t color = ring.Color(0, 0, 255);

  for (int i = 0; i < NUM_LEDS; i++) {
    ring.setPixelColor(i, (i < ledCount) ? color : 0);
  }
  ring.show();
}

// =========================
// 校正呼吸燈（白色，2 秒週期）
// =========================
void showCalibrationBreathe(Adafruit_NeoPixel &ring) {
  int b = breatheBrightness(CAL_BREATHE_PERIOD, CAL_BREATHE_MIN, CAL_BREATHE_MAX);
  ring.setBrightness(b);
  for (int i = 0; i < NUM_LEDS; i++) {
    ring.setPixelColor(i, ring.Color(255, 255, 255));
  }
  ring.show();
}

// =========================
// 待機呼吸燈（淡藍色，3 秒週期）
// =========================
void showIdleBreathe(Adafruit_NeoPixel &ring) {
  int b = breatheBrightness(IDLE_BREATHE_PERIOD, IDLE_BREATHE_MIN, IDLE_BREATHE_MAX);
  ring.setBrightness(b);
  for (int i = 0; i < NUM_LEDS; i++) {
    ring.setPixelColor(i, ring.Color(30, 30, 255));
  }
  ring.show();
}

// =========================
// 重設校正資料
// =========================
void resetCalibration(FSRChannel &ch) {
  ch.calibratedMin = 4095;
  ch.calibratedMax = 0;
  ch.zeroThreshold = 0;
}

// =========================
// 開始新一輪校正
// =========================
void startCalibration() {
  resetCalibration(ch1);
  resetCalibration(ch2);
  isCalibrating = true;
  isIdle = false;
  calibrationStartTime = millis();
  lastCalibrationPrintTime = 0;
  lastPrintedRemainSec = -1;
  Serial.println("/status recalibrating");
}

// =========================
// 更新校正資料
// =========================
void updateCalibration(FSRChannel &ch, int raw) {
  if (raw < ch.calibratedMin) ch.calibratedMin = raw;
  if (raw > ch.calibratedMax) ch.calibratedMax = raw;
}

// =========================
// 完成校正，計算 zeroThreshold + DEFAULT_MAX 保底
// =========================
void finalizeCalibration(FSRChannel &ch) {
  ch.zeroThreshold = ch.calibratedMin + SAFE_MARGIN;

  // 保底：如果校正期沒人壓，max 太低就套用預設值
  if (ch.calibratedMax < DEFAULT_MAX) {
    ch.calibratedMax = DEFAULT_MAX;
  }

  if (ch.zeroThreshold >= ch.calibratedMax) {
    ch.zeroThreshold = ch.calibratedMin;
  }
}

// =========================
// 按鈕檢查（debounce）
// =========================
void checkButton() {
  bool reading = digitalRead(BUTTON_PIN);
  if (reading != lastButtonReading) {
    lastDebounceTime = millis();
  }
  if ((millis() - lastDebounceTime) > debounceDelay) {
    if (reading != buttonState) {
      buttonState = reading;
      if (buttonState == LOW) {
        startCalibration();
      }
    }
  }
  lastButtonReading = reading;
}

// =========================
// 校正倒數輸出
// =========================
void printCalibrationCountdown(unsigned long elapsed) {
  long remainMs = (long)CALIBRATION_TIME - (long)elapsed;
  int remainSec = (remainMs > 0) ? (remainMs + 999) / 1000 : 0;
  if (remainSec != lastPrintedRemainSec) {
    lastPrintedRemainSec = remainSec;
    Serial.print("/status calibrating ");
    Serial.println(remainSec);
  }
}

// =========================
// setup
// =========================
void setup() {
  Serial.begin(115200);
  analogReadResolution(12);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  ring1.begin();
  ring1.setBrightness(60);
  ring1.show();

  ring2.begin();
  ring2.setBrightness(60);
  ring2.show();

  startCalibration();
}

// =========================
// loop
// =========================
void loop() {
  checkButton();

  int raw1 = readFSRRawAveraged(ch1.fsrPin);
  int raw2 = readFSRRawAveraged(ch2.fsrPin);

  // ─── 校正模式 ───
  if (isCalibrating) {
    unsigned long now = millis();
    unsigned long elapsed = now - calibrationStartTime;

    updateCalibration(ch1, raw1);
    updateCalibration(ch2, raw2);

    // 呼吸白燈
    showCalibrationBreathe(ring1);
    showCalibrationBreathe(ring2);

    // 每秒倒數
    if (now - lastCalibrationPrintTime >= CAL_PRINT_INTERVAL || lastPrintedRemainSec == -1) {
      lastCalibrationPrintTime = now;
      printCalibrationCountdown(elapsed);
    }

    // 校正完成
    if (elapsed >= CALIBRATION_TIME) {
      finalizeCalibration(ch1);
      finalizeCalibration(ch2);
      isCalibrating = false;
      lastActiveTime = millis();
      lastPressureOutputTime = millis();
      Serial.println("/status calibrated");
    }

    delay(SAMPLE_DELAY_MS);
    return;
  }

  // ─── 正常運作模式 ───

  // 動態擴展上限
  if (raw1 > ch1.calibratedMax) ch1.calibratedMax = raw1;
  if (raw2 > ch2.calibratedMax) ch2.calibratedMax = raw2;

  float norm1 = rawToNorm(raw1, ch1.zeroThreshold, ch1.calibratedMax);
  float norm2 = rawToNorm(raw2, ch2.zeroThreshold, ch2.calibratedMax);

  unsigned long now = millis();

  // 閒置偵測
  if (norm1 > 0.0 || norm2 > 0.0) {
    lastActiveTime = now;
    if (isIdle) {
      isIdle = false;
      Serial.println("/status active");
    }
  }

  bool shouldBeIdle = (now - lastActiveTime >= IDLE_TIMEOUT);

  // ─── 閒置待機：淡藍呼吸燈 ───
  if (shouldBeIdle) {
    if (!isIdle) {
      isIdle = true;
      Serial.println("/status idle");
    }
    showIdleBreathe(ring1);
    showIdleBreathe(ring2);

    // 閒置期間仍然輸出壓力資料（norm 0.0）
    if (now - lastPressureOutputTime >= PRESSURE_OUTPUT_INTERVAL) {
      lastPressureOutputTime = now;
      Serial.print("/pressure ");
      Serial.print(norm1, 3);
      Serial.print(" ");
      Serial.print(norm2, 3);
      Serial.print(" ");
      Serial.print(raw1);
      Serial.print(" ");
      Serial.println(raw2);
    }

    delay(1);
    return;
  }

  // ─── 正常顯示壓力 ───
  showPressure(ring1, norm1);
  showPressure(ring2, norm2);

  if (now - lastPressureOutputTime >= PRESSURE_OUTPUT_INTERVAL) {
    lastPressureOutputTime = now;
    Serial.print("/pressure ");
    Serial.print(norm1, 3);
    Serial.print(" ");
    Serial.print(norm2, 3);
    Serial.print(" ");
    Serial.print(raw1);
    Serial.print(" ");
    Serial.println(raw2);
  }

  delay(1);
}
