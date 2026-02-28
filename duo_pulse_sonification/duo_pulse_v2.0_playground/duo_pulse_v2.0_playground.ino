/*
 * Dual Pulse Sensor - ESP32 (v2.4)
 *
 * v2.4 修正：手指偵測改為「脈動振幅」而非絕對值門檻。
 *   原本 FINGER_THRESH = 300，但兩顆感測器靜止值都在 1800 左右，
 *   導致系統永遠誤判為「有手指」，/finger 參數完全失效。
 *
 *   新作法：用包絡線追蹤 envHigh / envLow，
 *   計算 pulsatile amplitude = envHigh - envLow。
 *   有手指 → 有心跳脈衝 → 振幅 > FINGER_AMP_THRESH
 *   沒手指 → 訊號穩定接近直流 → 振幅趨近 0
 *   這樣不管 DC 偏移多高都能正確偵測。
 *
 * 狀態機（三段式）：
 *   1. WAITING     — 等待振幅夠大（手指放上）
 *   2. CALIBRATING — 校準期 3 秒，收集 baseline 和 amplitude
 *   3. RUNNING     — threshold = baseline + amplitude * THRESH_RATIO
 *      baseline 緩慢跟蹤 DC 漂移（tau ≈ 5 秒）
 *
 * 輸出：
 *   /pulse/userX/raw      — 平滑後的 ADC 值（100Hz throttle）
 *   /pulse/userX/finger   — 1 = 手指在 / 0 = 手指拿掉
 *   /pulse/userX/beat     — 1 = 心跳開始 / 0 = 心跳結束（200ms gate）
 *   /pulse/userX/bpm      — 每次心跳時輸出 BPM
 *   /pulse/userX/thresh   — 目前使用的 threshold（debug 用）
 *
 * ── 硬體接線 ──────────────────────────────────────────────
 *   感測器 A 訊號腳 → GPIO34 (ADC1_CH6，輸入專用)
 *   感測器 B 訊號腳 → GPIO35 (ADC1_CH7，輸入專用)
 *   VCC → 3.3V（勿接 5V）    GND → GND
 *   需在 setup() 設定 analogSetPinAttenuation(pin, ADC_11db)
 *   以支援感測器 DC 偏壓（靜止值約 1600–2000，12-bit ADC）
 *
 * ── Max/MSP 接收 ──────────────────────────────────────────
 *   [serial <port> 115200] → [fromsymbol]
 *   → [route /pulse/userA/raw /pulse/userA/beat /pulse/userA/bpm /pulse/userA/finger
 *            /pulse/userB/raw /pulse/userB/beat /pulse/userB/bpm /pulse/userB/finger]
 *   注意：[route] 比對完整路徑，不支援前綴分層比對。
 *
 * ── WiFi OSC（可選）──────────────────────────────────────
 *   解開 #define ENABLE_WIFI，填入 ssid/password/outIp/outPort。
 *   需安裝 CNMAT OSC 函式庫。
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

  // 平滑（低通濾波）
  float smoothed = 0;
  const float SLIDE = 8.0;   // 500Hz 下 ≈ 16ms

  // ── 包絡線手指偵測（v2.4 新增）────────────────
  // envHigh / envLow 追蹤訊號的上下包絡線
  // 快速跟上（ENV_RISE）、緩慢衰退（ENV_FALL）
  // 脈動振幅 = envHigh - envLow
  float envHigh = 2048;
  float envLow  = 2048;
  const float ENV_RISE = 0.15;   // 快：約 7 samples ≈ 14ms 跟上峰值
  const float ENV_FALL = 0.002;  // 慢：tau ≈ 500 samples ≈ 1 秒
  // 判斷有手指的最小脈動振幅（ADC counts）
  // 靜止雜訊 < 10，心跳脈衝通常 50–300+
  const int FINGER_AMP_THRESH = 35;
  // 絕對底線：低於此值代表 ADC 完全沒訊號（斷線或未接）
  const int SIG_FLOOR = 50;

  // 手指狀態
  bool fingerOn = false;

  // 校準
  unsigned long calibStartMs = 0;
  const int     CALIB_MS     = 3000;
  long          calibSum     = 0;
  int           calibCount   = 0;
  int           calibPeak    = 0;

  // 自動 threshold
  float baseline   = 0;
  int   amplitude  = 0;
  int   threshold  = 2048;
  const float THRESH_RATIO   = 0.40;
  const float BASELINE_ALPHA = 0.0004;  // tau ≈ 5s at 500Hz

  // Beat 偵測
  bool          armed      = true;
  bool          pulse      = false;
  unsigned long lastBeatMs = 0;
  int           bpm        = 0;
  const int     REFRACTORY = 350;   // ms，對應 max ~171 BPM
  const int     BEAT_GATE  = 200;   // ms，beat ON 持續時間

  // Raw 輸出節流（100Hz）
  unsigned long lastRawMs = 0;

  PulseSensor(int p, String pfx) : pin(p), prefix(pfx) {}

  // ─────────────────────────────────────────────
  void update(int raw, unsigned long nowMs) {

    // ── 平滑 ────────────────────────────────────
    if (smoothed < 1 || raw < SIG_FLOOR) {
      smoothed = raw;
      // 訊號完全消失時同步重置包絡線
      envHigh = raw;
      envLow  = raw;
    } else {
      smoothed += (raw - smoothed) / SLIDE;
    }
    int sig = (int)smoothed;

    // ── 包絡線更新 ───────────────────────────────
    // 向上：快速跟上峰值；向下：緩慢衰退
    if (sig > envHigh) envHigh += (sig - envHigh) * ENV_RISE;
    else               envHigh -= (envHigh - sig) * ENV_FALL;

    // 向下：快速跟上谷值；向上：緩慢衰退
    if (sig < envLow)  envLow  += (sig - envLow)  * ENV_RISE;
    else               envLow  -= (envLow  - sig)  * ENV_FALL;

    int envAmp   = (int)(envHigh - envLow);
    bool hasFingerSignal = (sig > SIG_FLOOR) && (envAmp > FINGER_AMP_THRESH);

    // ── 手指偵測 ─────────────────────────────────
    if (!hasFingerSignal) {
      if (fingerOn) {
        fingerOn = false;
        send(prefix + "/finger", 0);
        state = WAITING;
        armed = true;
        // 包絡線歸零，避免殘留振幅影響下次偵測
        envHigh = sig;
        envLow  = sig;
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
        state        = CALIBRATING;
        calibStartMs = nowMs;
        calibSum     = 0;
        calibCount   = 0;
        calibPeak    = sig;
        Serial.print(prefix); Serial.println(" calibrating...");
      }
    }

    // ── 校準期 ───────────────────────────────────
    if (state == CALIBRATING) {
      calibSum += sig;
      calibCount++;
      if (sig > calibPeak) calibPeak = sig;

      if (nowMs - calibStartMs >= CALIB_MS) {
        baseline  = (float)calibSum / calibCount;
        amplitude = calibPeak - (int)baseline;
        amplitude = max(amplitude, 40);
        threshold = (int)(baseline + amplitude * THRESH_RATIO);
        state     = RUNNING;
        send(prefix + "/thresh", threshold);
        Serial.print(prefix);
        Serial.print(" calibrated: baseline="); Serial.print((int)baseline);
        Serial.print(" amp=");    Serial.print(amplitude);
        Serial.print(" thresh="); Serial.println(threshold);
      }
      sendRaw(sig, nowMs);
      return;
    }

    // ── 運作中：緩慢跟蹤 baseline ───────────────
    if (sig < threshold) {
      baseline  += (sig - baseline) * BASELINE_ALPHA;
      threshold  = (int)(baseline + amplitude * THRESH_RATIO);
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
        if (interval < 2000) {
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
  Serial.println("=== Dual Pulse Sensor v2.4 ===");

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
