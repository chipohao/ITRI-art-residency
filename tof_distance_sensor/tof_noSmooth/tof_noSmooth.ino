#include <Wire.h>
#include "Adafruit_VL53L0X.h"

// I2C pins (ESP32 default)
#define I2C_SDA 21
#define I2C_SCL 22

// XSHUT pins
#define XSHUT_1 25
#define XSHUT_2 26

// New I2C addresses
#define ADDR_1 0x30
#define ADDR_2 0x31

Adafruit_VL53L0X lox1;
Adafruit_VL53L0X lox2;

const int OUT_OF_RANGE = -1;
const uint32_t TIMING_BUDGET_US = 20000;

// -----------------------
// Calibration settings
// -----------------------
const unsigned long CALIB_MS = 5000;   // 前 5 秒校正
unsigned long calibStartMs = 0;
bool calibDone = false;

int offsetX = 0, offsetY = 0;          // 校正後的基準（最小值）
int calibMinX = 999999;
int calibMinY = 999999;

// 統一輸出格式：x=...,Y=...
void printXY(int x, int y) {
  Serial.print("x=");
  Serial.print(x);
  Serial.print(",Y=");
  Serial.println(y);
}

void setup() {
  Serial.begin(115200);
  delay(500);

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);

  pinMode(XSHUT_1, OUTPUT);
  pinMode(XSHUT_2, OUTPUT);

  // 兩顆先關
  digitalWrite(XSHUT_1, LOW);
  digitalWrite(XSHUT_2, LOW);
  delay(10);

  // 開第一顆 -> 改地址
  digitalWrite(XSHUT_1, HIGH);
  delay(10);
  lox1.begin(0x29, false, &Wire);
  lox1.setAddress(ADDR_1);

  // 開第二顆 -> 改地址
  digitalWrite(XSHUT_2, HIGH);
  delay(10);
  lox2.begin(0x29, false, &Wire);
  lox2.setAddress(ADDR_2);

  // 快速量測
  lox1.setMeasurementTimingBudgetMicroSeconds(TIMING_BUDGET_US);
  lox2.setMeasurementTimingBudgetMicroSeconds(TIMING_BUDGET_US);

  // start calibration
  calibStartMs = millis();
  calibDone = false;
  calibMinX = 999999;
  calibMinY = 999999;

  Serial.println("CAL START (5s): move to closest positions now.");
}

void loop() {
  VL53L0X_RangingMeasurementData_t m1, m2;

  lox1.rangingTest(&m1, false);
  lox2.rangingTest(&m2, false);

  // ✅ 原始數據（不做平滑）
  int rawX = (m1.RangeStatus != 4) ? (int)m1.RangeMilliMeter : OUT_OF_RANGE;
  int rawY = (m2.RangeStatus != 4) ? (int)m2.RangeMilliMeter : OUT_OF_RANGE;

  // -----------------------
  // Calibration phase (5s)
  // -----------------------
  if (!calibDone) {
    // 用 raw 更新最小值（只吃有效值）
    if (rawX != OUT_OF_RANGE) calibMinX = min(calibMinX, rawX);
    if (rawY != OUT_OF_RANGE) calibMinY = min(calibMinY, rawY);

    unsigned long elapsed = millis() - calibStartMs;

    // ✅ 倒數 + 狀態列印（每 200ms）
    static unsigned long lastPrint = 0;
    if (millis() - lastPrint >= 200) {
      lastPrint = millis();

      long remainMs = (long)CALIB_MS - (long)elapsed;
      int remainSec = (remainMs > 0) ? (int)((remainMs + 999) / 1000) : 0;

      int x0 = OUT_OF_RANGE;
      int y0 = OUT_OF_RANGE;

      // 以「目前最小值」暫時歸零，讓你看校正是否有在抓到 min
      if (rawX != OUT_OF_RANGE && calibMinX != 999999) x0 = max(0, rawX - calibMinX);
      if (rawY != OUT_OF_RANGE && calibMinY != 999999) y0 = max(0, rawY - calibMinY);

      Serial.print("CAL ");
      Serial.print(remainSec);
      Serial.print("s | minX=");
      Serial.print((calibMinX == 999999) ? -1 : calibMinX);
      Serial.print(" minY=");
      Serial.print((calibMinY == 999999) ? -1 : calibMinY);
      Serial.print(" | ");
      Serial.print("x=");
      Serial.print(x0);
      Serial.print(",Y=");
      Serial.println(y0);
    }

    // 校正結束
    if (elapsed >= CALIB_MS) {
      offsetX = (calibMinX == 999999) ? 0 : calibMinX;
      offsetY = (calibMinY == 999999) ? 0 : calibMinY;
      calibDone = true;

      Serial.print("CAL DONE | offsetX=");
      Serial.print(offsetX);
      Serial.print(" offsetY=");
      Serial.println(offsetY);
    }
    return;
  }

  // -----------------------
  // Normal output (zeroed)
  // -----------------------
  int xOut = OUT_OF_RANGE;
  int yOut = OUT_OF_RANGE;

  if (rawX != OUT_OF_RANGE) xOut = max(0, rawX - offsetX);
  if (rawY != OUT_OF_RANGE) yOut = max(0, rawY - offsetY);

  printXY(xOut, yOut);
}