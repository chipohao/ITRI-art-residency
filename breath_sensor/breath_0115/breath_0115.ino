/*
 * Breath Sensor — ESP32 (breath_0115)
 *
 * 雙路吹氣/吸氣感測系統：將呼吸強度映射為 -10~+10 浮點輸出，
 * 並同步驅動兩組 PWM 霧化器（超音波霧化片），
 * 讓呼吸強度直接控制出霧量。
 *
 * ── 硬體接線 ──────────────────────────────────────────────
 *   HX710B_1 : SCK=GPIO18  DOUT=GPIO19
 *   HX710B_2 : SCK=GPIO16  DOUT=GPIO17
 *   MOSFET_1 : PWM 輸出 GPIO25（控制霧化器 1）
 *   MOSFET_2 : PWM 輸出 GPIO26（控制霧化器 2）
 *   供電      : 3.3V & GND
 *
 *   HX710B 為 24-bit ADC（常用於壓力/呼吸感測），
 *   此處採用 bit-bang SPI 讀取（GPIO 模擬時序），非硬體 SPI/I2C。
 *
 * ── 演算法 ────────────────────────────────────────────────
 *   1. 開機 baseline 校準：上電後各讀 40 次取平均，
 *      baseline 代表靜止（無呼吸）時的 raw 值。
 *   2. Deadzone：|raw - baseline| ≤ 30000 輸出 0（過濾靜止雜訊）。
 *   3. 自適應映射：動態追蹤歷史 maxBlow / minInhale，
 *      吹氣 → +1.0~+10.0，吸氣 → -1.0~-10.0。
 *      |out| ≥ 8 時為 100% 霧化。
 *   4. Kick 機制：霧化器從停止（duty=0）重新啟動時，
 *      先全功率維持 1100ms（KICK_MS），確保超音波霧化片順利起振。
 *
 * ── Serial 輸出格式（115200 baud）────────────────────────
 *   "out1 out2\n"   — 例：3.47 -1.20
 *   每 50ms 輸出一次（約 20Hz）
 *   out 範圍：-10.0（最強吸氣）~ 0.0（靜止）~ +10.0（最強吹氣）
 *
 * ── Max/MSP 接收方式 ─────────────────────────────────────
 *   [serial <port> 115200]
 *   → [fromsymbol] → [unpack f f] → 分別取得 out1, out2
 *
 * 注意：使用 ESP32 Arduino Core 3.x 新版 API（ledcAttach），
 *       舊版 Core 2.x 需改為 ledcSetup + ledcAttachPin。
 */

#include <limits.h>
#include <math.h>

// =======================================================
// (A) 2x HX710B Breath Sensors (raw -> out -10~+10)
// =======================================================
// HX710B_1: SCK=GPIO18, DOUT=GPIO19
// HX710B_2: SCK=GPIO16, DOUT=GPIO17

// --- Sensor 1 pins ---
const int PIN_SCK_1  = 18;
const int PIN_DOUT_1 = 19;

// --- Sensor 2 pins ---
const int PIN_SCK_2  = 16;
const int PIN_DOUT_2 = 17;

// --- 呼吸映射參數 ---
const long DEADZONE_RAW = 30000;   // raw 在 baseline ± 這範圍內都輸出 0

const float OUT_MIN_POS = 1.0;     // 吹氣最小輸出
const float OUT_MAX_POS = 10.0;    // 吹氣最大輸出
const float OUT_MIN_NEG = -1.0;    // 吸氣最小輸出（靠近 0）
const float OUT_MAX_NEG = -10.0;   // 吸氣最大輸出（最強吸氣）

// --- baseline 與極值 ---
long baseline1 = 0, baseline2 = 0;
bool baselineReady1 = false, baselineReady2 = false;

long maxBlow1 = LONG_MIN, maxBlow2 = LONG_MIN;
long minInhale1 = LONG_MAX, minInhale2 = LONG_MAX;

// --- HX710B raw reader (generic) ---
long readHX710B(int pinSCK, int pinDOUT) {
  unsigned long t0 = millis();
  while (digitalRead(pinDOUT) == HIGH) {
    if (millis() - t0 > 2000) return LONG_MIN; // timeout
  }

  long value = 0;
  for (int i = 0; i < 24; i++) {
    digitalWrite(pinSCK, HIGH);
    delayMicroseconds(2);
    value = (value << 1) | digitalRead(pinDOUT);
    digitalWrite(pinSCK, LOW);
    delayMicroseconds(2);
  }

  // 25th clock
  digitalWrite(pinSCK, HIGH);
  delayMicroseconds(2);
  digitalWrite(pinSCK, LOW);
  delayMicroseconds(2);

  // sign extend 24-bit
  if (value & 0x800000) value |= 0xFF000000;
  return value;
}

float mapFloat(float x, float inMin, float inMax, float outMin, float outMax) {
  if (inMax == inMin) return outMin;
  float t = (x - inMin) / (inMax - inMin);
  if (t < 0) t = 0;
  if (t > 1) t = 1;
  return outMin + t * (outMax - outMin);
}

long calibrateBaseline(int pinSCK, int pinDOUT, int samples, int delayMs) {
  long long sum = 0;
  int ok = 0;
  for (int i = 0; i < samples; i++) {
    long v = readHX710B(pinSCK, pinDOUT);
    if (v != LONG_MIN) {
      sum += v;
      ok++;
    }
    delay(delayMs);
  }
  if (ok == 0) return 0;
  return (long)(sum / ok);
}

// breathMap: per-sensor state passed by reference
float breathMap(long raw, long baseline, long &maxBlow, long &minInhale) {
  long delta = raw - baseline;

  // deadzone -> 0
  if (labs(delta) <= DEADZONE_RAW) return 0.0f;

  // blow (+)
  if (delta > 0) {
    if (raw > maxBlow) maxBlow = raw;

    long minPos = baseline + DEADZONE_RAW;
    long effectiveMax = maxBlow;
    if (effectiveMax <= minPos) return OUT_MIN_POS;

    return mapFloat((float)raw, (float)minPos, (float)effectiveMax, OUT_MIN_POS, OUT_MAX_POS);
  }
  // inhale (-)
  else {
    if (raw < minInhale) minInhale = raw;

    long maxNeg = baseline - DEADZONE_RAW;
    long effectiveMin = minInhale;
    if (effectiveMin >= maxNeg) return OUT_MIN_NEG;

    // raw from [baseline-DEADZONE .. minInhale] -> [-1 .. -10]
    return mapFloat((float)raw, (float)maxNeg, (float)effectiveMin, OUT_MIN_NEG, OUT_MAX_NEG);
  }
}

// =======================================================
// (B) 2x PWM Mist Control (each with KICK)
// =======================================================

// --- PWM pins ---
const int PWM_PIN_1 = 25;   // MOSFET1 gate/PWM
const int PWM_PIN_2 = 26;   // MOSFET2 gate/PWM

// ESP32 3.0: 不需要再定義 Channel (PWM_CH_1/2)

const int PWM_RES  = 10;   // 0~1023
const int PWM_FREQ = 100;

const int MAX_DUTYRAW = (1 << PWM_RES) - 1; // 1023
const int KICK_MS = 1100;

// 霧化強度規則
const float DUTY_BASELINE = 5.0f;    // out=0 時：最低霧化 5%（不切斷）
const float DUTY_MIN_POS  = 5.0f;    // |out|=1 時：5%
const float OUT_FULL_SCALE = 8.0f;   // |out|>=8 -> 100%

const int LOOP_DELAY_MS = 50;

// each channel remembers last dutyRaw for kick condition
int lastDutyRaw1 = 0;
int lastDutyRaw2 = 0;

int dutyRawFromPercent(float percent) {
  percent = constrain(percent, 0.0f, 100.0f);
  return (int)((percent / 100.0f) * MAX_DUTYRAW + 0.5f);
}

float dutyPercentFromOut(float out) {
  if (out == 0.0f) return DUTY_BASELINE;

  float mag = fabs(out); // 吹/吸都用強度（取絕對值）

  if (mag >= OUT_FULL_SCALE) return 100.0f;
  if (mag < 1.0f) return DUTY_BASELINE;

  float clampedMag = constrain(mag, 1.0f, OUT_FULL_SCALE);
  return mapFloat(clampedMag, 1.0f, OUT_FULL_SCALE, DUTY_MIN_POS, 100.0f);
}

// 修改: 傳入的是 PIN 而不是 Channel
void setMistDutyRawWithKick(int pwmPin, int targetDutyRaw, int &lastDutyRawRef) {
  targetDutyRaw = constrain(targetDutyRaw, 0, MAX_DUTYRAW);

  // kick when 0 -> >0
  if (lastDutyRawRef == 0 && targetDutyRaw > 0) {
    ledcWrite(pwmPin, MAX_DUTYRAW); // ESP32 3.0: 寫入 Pin
    delay(KICK_MS);
  }

  ledcWrite(pwmPin, targetDutyRaw); // ESP32 3.0: 寫入 Pin
  lastDutyRawRef = targetDutyRaw;
}

// =======================================================
// setup / loop
// =======================================================

void setup() {
  Serial.begin(115200);
  delay(300);

  // HX710B init
  pinMode(PIN_SCK_1, OUTPUT);
  pinMode(PIN_DOUT_1, INPUT);
  digitalWrite(PIN_SCK_1, LOW);

  pinMode(PIN_SCK_2, OUTPUT);
  pinMode(PIN_DOUT_2, INPUT);
  digitalWrite(PIN_SCK_2, LOW);

  delay(200);

  Serial.println("System Start");

  // calibrate baseline for each sensor
  baseline1 = calibrateBaseline(PIN_SCK_1, PIN_DOUT_1, 40, 50);
  baseline2 = calibrateBaseline(PIN_SCK_2, PIN_DOUT_2, 40, 50);

  baselineReady1 = true;
  baselineReady2 = true;

  maxBlow1   = baseline1 + DEADZONE_RAW;
  minInhale1 = baseline1 - DEADZONE_RAW;

  maxBlow2   = baseline2 + DEADZONE_RAW;
  minInhale2 = baseline2 - DEADZONE_RAW;

  // PWM init (ESP32 3.0 New API)
  // 直接 Attach Pin，不需要 ledcSetup
  ledcAttach(PWM_PIN_1, PWM_FREQ, PWM_RES);
  ledcAttach(PWM_PIN_2, PWM_FREQ, PWM_RES);

  // initial: 5% for both
  int initDutyRaw = dutyRawFromPercent(DUTY_BASELINE);
  setMistDutyRawWithKick(PWM_PIN_1, initDutyRaw, lastDutyRaw1);
  setMistDutyRawWithKick(PWM_PIN_2, initDutyRaw, lastDutyRaw2);
}

void loop() {
  // read both sensors
  long raw1 = readHX710B(PIN_SCK_1, PIN_DOUT_1);
  long raw2 = readHX710B(PIN_SCK_2, PIN_DOUT_2);

  // --- Sensor 1 handling ---
  float out1 = 0.0f;
  float dutyPercent1 = DUTY_BASELINE;
  int dutyRaw1 = dutyRawFromPercent(dutyPercent1);

  if (raw1 == LONG_MIN) {
    out1 = 0.0f;
    dutyPercent1 = DUTY_BASELINE;
    dutyRaw1 = dutyRawFromPercent(dutyPercent1);
  } else {
    out1 = breathMap(raw1, baseline1, maxBlow1, minInhale1);
    dutyPercent1 = dutyPercentFromOut(out1);
    dutyRaw1 = dutyRawFromPercent(dutyPercent1);
  }

  // --- Sensor 2 handling ---
  float out2 = 0.0f;
  float dutyPercent2 = DUTY_BASELINE;
  int dutyRaw2 = dutyRawFromPercent(dutyPercent2);

  if (raw2 == LONG_MIN) {
    out2 = 0.0f;
    dutyPercent2 = DUTY_BASELINE;
    dutyRaw2 = dutyRawFromPercent(dutyPercent2);
  } else {
    out2 = breathMap(raw2, baseline2, maxBlow2, minInhale2);
    dutyPercent2 = dutyPercentFromOut(out2);
    dutyRaw2 = dutyRawFromPercent(dutyPercent2);
  }

  // apply PWM (傳入 PIN)
  setMistDutyRawWithKick(PWM_PIN_1, dutyRaw1, lastDutyRaw1);
  setMistDutyRawWithKick(PWM_PIN_2, dutyRaw2, lastDutyRaw2);

  // ==========================================
  // Serial 輸出 for Max/MSP
  // 格式: "數值1 數值2" (中間空格，最後換行)
  // ==========================================
  
  Serial.print(out1, 2);
  Serial.print(" ");
  Serial.println(out2, 2);

  delay(LOOP_DELAY_MS);
}