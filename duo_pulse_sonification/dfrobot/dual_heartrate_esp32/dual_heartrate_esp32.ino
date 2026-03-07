/*
 * Dual Heart Rate Sensor (SEN0203 x2) on ESP32
 * Analog mode — both switches set to A
 * No external library needed
 *
 * Wiring:
 *   Sensor 1 → GPIO 34 (ADC1)
 *   Sensor 2 → GPIO 35 (ADC1)
 *   Both VCC  → 3V3
 *   Both GND  → GND
 *
 * Output (serial, OSC-style tags):
 *   /raw/1 <int>       smoothed PPG waveform, 0-4095 (every 20ms)
 *   /raw/2 <int>
 *   /bpm/1 <int>       heart rate in BPM (only when valid)
 *   /bpm/2 <int>
 *   /state/1 <int>     0 = NO_FINGER, 1 = DETECTING, 2 = FINGER_ON (on change)
 *   /state/2 <int>
 *
 * Max/MSP routing:
 *   [route /raw/1 /raw/2 /bpm/1 /bpm/2 /state/1 /state/2]
 */

// ---- Pin assignments ----
#define SENSOR_PIN_1 34
#define SENSOR_PIN_2 35

// ---- Finger state enum ----
enum FingerState {
  NO_FINGER  = 0,   // no signal variation — nothing on sensor
  DETECTING  = 1,   // signal variation detected — waiting for valid heartbeat
  FINGER_ON  = 2    // valid heartbeat confirmed — actively reading
};

// ---- Per-sensor data structure ----
struct PulseSensor {
  // smoothing
  float ema;
  float prevEma;

  // peak detection
  float peakVal;
  float troughVal;
  bool rising;
  unsigned long lastPeak;

  // BPM (moving average of last 5 beats)
  int bpmBuf[5];
  int bpmIdx;
  int bpmCount;
  int bpmSmooth;

  // finger detection
  int ringBuf[50];
  int ringIdx;
  FingerState state;
  FingerState prevState;
  unsigned long lastValidBeat;
};

PulseSensor sensor[2];

// ---- Tunable parameters ----
const float ALPHA           = 0.15;   // EMA factor (0.0-1.0, lower = smoother)
const float AMP_THRESHOLD   = 80.0;   // min peak-trough diff to count as heartbeat
const int   FINGER_WINDOW   = 50;     // ring buffer size for variation check
const int   FINGER_THRESHOLD = 100;   // min range in window to detect presence
const unsigned long BEAT_TIMEOUT = 3000;  // ms without beat → drop back to DETECTING
const unsigned long BEAT_MIN = 300;   // shortest valid beat interval (200 BPM)
const unsigned long BEAT_MAX = 2000;  // longest valid beat interval (30 BPM)

// ---- Init ----
void initSensor(PulseSensor &s) {
  s.ema         = 0;
  s.prevEma     = 0;
  s.peakVal     = 0;
  s.troughVal   = 4095;
  s.rising      = false;
  s.lastPeak    = 0;
  s.bpmIdx      = 0;
  s.bpmCount    = 0;
  s.bpmSmooth   = 0;
  s.ringIdx     = 0;
  s.state       = NO_FINGER;
  s.prevState   = NO_FINGER;
  s.lastValidBeat = 0;
  memset(s.bpmBuf, 0, sizeof(s.bpmBuf));
  memset(s.ringBuf, 0, sizeof(s.ringBuf));
}

// ---- Determine finger state ----
FingerState getFingerState(PulseSensor &s, int raw) {
  // update ring buffer
  s.ringBuf[s.ringIdx] = raw;
  s.ringIdx = (s.ringIdx + 1) % FINGER_WINDOW;

  // check signal variation
  int minVal = 4095, maxVal = 0;
  for (int i = 0; i < FINGER_WINDOW; i++) {
    if (s.ringBuf[i] < minVal) minVal = s.ringBuf[i];
    if (s.ringBuf[i] > maxVal) maxVal = s.ringBuf[i];
  }
  bool hasVariation = (maxVal - minVal) > FINGER_THRESHOLD;

  if (!hasVariation) {
    return NO_FINGER;
  }

  // has variation — check if we have a recent valid heartbeat
  bool recentBeat = (millis() - s.lastValidBeat) < BEAT_TIMEOUT;

  if (recentBeat) {
    return FINGER_ON;
  }

  return DETECTING;
}

// ---- Process one sensor ----
void processSensor(PulseSensor &s, int pin, int id) {
  int raw = analogRead(pin);

  // EMA smoothing
  s.ema = ALPHA * raw + (1.0 - ALPHA) * s.ema;

  // finger state
  s.state = getFingerState(s, raw);

  // send state on change
  if (s.state != s.prevState) {
    Serial.print("/state/");
    Serial.print(id);
    Serial.print(" ");
    Serial.println((int)s.state);
    s.prevState = s.state;

    // reset BPM when finger removed
    if (s.state == NO_FINGER) {
      s.bpmSmooth   = 0;
      s.bpmCount    = 0;
      s.bpmIdx      = 0;
      s.rising      = false;
      s.peakVal     = 0;
      s.troughVal   = 4095;
      memset(s.bpmBuf, 0, sizeof(s.bpmBuf));
    }
  }

  // peak detection (only when DETECTING or FINGER_ON)
  if (s.state != NO_FINGER) {
    float slope = s.ema - s.prevEma;

    if (slope > 0) {
      if (!s.rising) {
        s.troughVal = s.prevEma;
        s.rising = true;
      }
    } else {
      if (s.rising) {
        s.peakVal = s.prevEma;
        float amplitude = s.peakVal - s.troughVal;

        if (amplitude > AMP_THRESHOLD) {
          unsigned long now = millis();
          unsigned long interval = now - s.lastPeak;

          if (interval > BEAT_MIN && interval < BEAT_MAX) {
            int bpm = 60000 / interval;
            if (bpm > 40 && bpm < 200) {
              s.bpmBuf[s.bpmIdx] = bpm;
              s.bpmIdx = (s.bpmIdx + 1) % 5;
              if (s.bpmCount < 5) s.bpmCount++;

              int sum = 0;
              for (int i = 0; i < s.bpmCount; i++) sum += s.bpmBuf[i];
              s.bpmSmooth = sum / s.bpmCount;

              s.lastValidBeat = now;
            }
          }
          s.lastPeak = now;
        }
        s.rising = false;
      }
    }
  }

  s.prevEma = s.ema;

  // always send raw
  Serial.print("/raw/");
  Serial.print(id);
  Serial.print(" ");
  Serial.println((int)s.ema);

  // send BPM only when FINGER_ON and valid
  if (s.state == FINGER_ON && s.bpmSmooth > 0) {
    Serial.print("/bpm/");
    Serial.print(id);
    Serial.print(" ");
    Serial.println(s.bpmSmooth);
  }
}

// ---- Setup ----
void setup() {
  Serial.begin(115200);
  initSensor(sensor[0]);
  initSensor(sensor[1]);
}

// ---- Loop ----
void loop() {
  processSensor(sensor[0], SENSOR_PIN_1, 1);
  processSensor(sensor[1], SENSOR_PIN_2, 2);
  delay(20);
}
