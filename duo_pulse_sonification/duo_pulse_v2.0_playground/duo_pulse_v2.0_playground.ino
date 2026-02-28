/*
 * Dual Pulse Sensor - ESP32 (v2.3)
 *
 * Beat detection 改為三段式：
 *   1. 等待手指（WAITING）
 *   2. 校準期 3 秒（CALIBRATING）— 收集 baseline 和 amplitude
 *   3. 運作（RUNNING）— threshold = baseline + amplitude * THRESH_RATIO
 *      baseline 緩慢跟蹤 DC 漂移（tau ≈ 5 秒）
 *
 * 輸出：
 *   /pulse/userX/raw      — 平滑後的 ADC 值（100Hz throttle）
 *   /pulse/userX/finger   — 1 = 手指在 / 0 = 手指拿掉
 *   /pulse/userX/beat     — 1 = 心跳開始 / 0 = 心跳結束（200ms gate）
 *   /pulse/userX/bpm      — 每次心跳時輸出 BPM
 *   /pulse/userX/thresh   — 目前使用的 threshold（debug 用）
 */

// #define ENABLE_WIFI

#ifdef ENABLE_WIFI
  #include <WiFi.h>
  #include <WiFiUdp.h>
  #include <OSCMessage.h>
  const char* ssid     = "YOUR_WIFI_SSID";
  const char* password = "YOUR_WIFI_PASSWORD";
  IPAddress outIp(192, 168, 1, 100);
  const unsigned int outPort = 8000;
  WiFiUDP Udp;
#endif

const int PIN_SENSOR_A = 34;
const int PIN_SENSOR_B = 35;

// ──────────────────────────────────────────────
class PulseSensor {
public:
  int    pin;
  String prefix;

  // 狀態機
  enum State { WAITING, CALIBRATING, RUNNING } state = WAITING;

  // 平滑
  float smoothed    = 0;
  float SLIDE       = 8.0;   // 500Hz 下 ≈ 16ms，壓雜訊

  // 手指偵測
  bool  fingerOn    = false;
  const int FINGER_THRESH = 300;

  // 校準
  unsigned long calibStartMs = 0;
  const int     CALIB_MS     = 3000;  // 3 秒校準期
  long          calibSum     = 0;
  int           calibCount   = 0;
  int           calibPeak    = 0;

  // 自動 threshold
  float baseline   = 0;
  int   amplitude  = 0;
  int   threshold  = 2048;
  // threshold = baseline + amplitude * THRESH_RATIO
  // 0.4 表示峰值往上 40% 才觸發，可依訊號調整
  const float THRESH_RATIO = 0.40;
  // baseline 跟蹤速度：alpha = 1/2500 ≈ 5 秒 tau at 500Hz
  const float BASELINE_ALPHA = 0.0004;

  // Beat 偵測
  bool          armed       = true;
  bool          pulse       = false;
  unsigned long lastBeatMs  = 0;
  int           bpm         = 0;
  const int     REFRACTORY  = 350;    // ms，對應 max ~171 BPM
  const int     BEAT_GATE   = 200;    // ms，beat ON 持續時間

  // Raw 輸出節流（100Hz）
  unsigned long lastRawMs   = 0;

  PulseSensor(int p, String pfx) : pin(p), prefix(pfx) {}

  // ─────────────────────────────────────────────
  void update(int raw, unsigned long nowMs) {

    // ── 平滑 ────────────────────────────────────
    if (smoothed < 1 || raw < FINGER_THRESH) {
      smoothed = raw;
    } else {
      smoothed += (raw - smoothed) / SLIDE;
    }
    int sig = (int)smoothed;

    // ── 手指偵測 ─────────────────────────────────
    if (sig < FINGER_THRESH) {
      if (fingerOn) {
        fingerOn = false;
        send(prefix + "/finger", 0);
        // 手指拿掉 → 重置狀態
        state     = WAITING;
        armed     = true;
        if (pulse) {
          pulse = false;
          send(prefix + "/beat", 0);
        }
      }
      sendRaw(sig, nowMs);
      return;
    } else {
      if (!fingerOn) {
        fingerOn = true;
        send(prefix + "/finger", 1);
        // 重新進入校準
        state       = CALIBRATING;
        calibStartMs = nowMs;
        calibSum    = 0;
        calibCount  = 0;
        calibPeak   = sig;
        Serial.print(prefix); Serial.println(" calibrating...");
      }
    }

    // ── 校準期 ───────────────────────────────────
    if (state == CALIBRATING) {
      calibSum  += sig;
      calibCount++;
      if (sig > calibPeak) calibPeak = sig;

      if (nowMs - calibStartMs >= CALIB_MS) {
        baseline  = (float)calibSum / calibCount;
        amplitude = calibPeak - (int)baseline;
        amplitude = max(amplitude, 40);          // 最小振幅保護
        threshold = (int)(baseline + amplitude * THRESH_RATIO);
        state     = RUNNING;
        send(prefix + "/thresh", threshold);
        Serial.print(prefix);
        Serial.print(" calibrated: baseline=");
        Serial.print((int)baseline);
        Serial.print(" amp="); Serial.print(amplitude);
        Serial.print(" thresh="); Serial.println(threshold);
      }
      sendRaw(sig, nowMs);
      return;
    }

    // ── 運作中：緩慢跟蹤 baseline ───────────────
    // 只在訊號低於 threshold 時更新（避免峰值拉高 baseline）
    if (sig < threshold) {
      baseline += (sig - baseline) * BASELINE_ALPHA;
      threshold = (int)(baseline + amplitude * THRESH_RATIO);
    }

    // ── Beat 偵測 ────────────────────────────────
    if (sig < threshold) armed = true;

    if (pulse && (nowMs - lastBeatMs >= BEAT_GATE)) {
      pulse = false;
      send(prefix + "/beat", 0);
    }

    if (sig > threshold && armed && !pulse) {
      long interval = nowMs - lastBeatMs;
      if (interval > REFRACTORY) {
        armed      = false;
        pulse      = true;
        lastBeatMs = nowMs;
        send(prefix + "/beat", 1);
        if (interval > REFRACTORY && interval < 2000) {
          bpm = 60000 / interval;
          if (bpm > 30) send(prefix + "/bpm", bpm);
        }
      }
    }

    sendRaw(sig, nowMs);
  }

  // ─────────────────────────────────────────────
  void sendRaw(int sig, unsigned long nowMs) {
    if (nowMs - lastRawMs >= 10) {   // 100Hz throttle
      lastRawMs = nowMs;
      send(prefix + "/raw", sig);
    }
  }

  void send(String addr, int val) {
    Serial.print(addr); Serial.print(" "); Serial.println(val);
    #ifdef ENABLE_WIFI
      Udp.beginPacket(outIp, outPort);
      OSCMessage msg(addr.c_str());
      msg.add((float)val);
      msg.send(Udp);
      Udp.endPacket();
      msg.empty();
    #endif
  }
};
// ──────────────────────────────────────────────

PulseSensor sensorA(PIN_SENSOR_A, "/pulse/userA");
PulseSensor sensorB(PIN_SENSOR_B, "/pulse/userB");

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("=== Dual Pulse Sensor v2.3 ===");

  analogSetPinAttenuation(PIN_SENSOR_A, ADC_11db);
  analogSetPinAttenuation(PIN_SENSOR_B, ADC_11db);
  analogSetWidth(12);

  #ifdef ENABLE_WIFI
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
    Serial.println("WiFi Connected");
  #endif

  Serial.println("Ready. Place finger on sensor.");
}

void loop() {
  static unsigned long lastSampleUs = 0;
  unsigned long nowUs = micros();

  if (nowUs - lastSampleUs < 2000) return;  // 500Hz
  lastSampleUs = nowUs;

  unsigned long nowMs = millis();

  int rawA = analogRead(PIN_SENSOR_A);
  delayMicroseconds(50);
  int rawB = analogRead(PIN_SENSOR_B);

  sensorA.update(rawA, nowMs);
  sensorB.update(rawB, nowMs);
}
