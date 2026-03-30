/*
 * Piezo 4-Drum — ESP32-C3 SuperMini + 4x Piezo + 4x LED
 *
 * Non-blocking version:
 *   1) Dynamic baseline tracking
 *   2) Relative threshold trigger
 *   3) Consecutive hit debounce
 *   4) IIR filter
 *   5) Non-blocking peak capture (no while loop)
 *   6) Non-blocking LED with fade tail (activity-driven, new signal resets)
 *   7) Serial buffer overflow protection (prevent blocking delay)
 *   8) WHO identification protocol for multi-device setup
 *   9) Continuous stream output (like a microphone level meter)
 *
 * -- Hardware Wiring (ESP32-C3 SuperMini) ---------------------
 *   Piezo 1    -> GPIO0     Piezo sensor 1 (+ 1M ohm to GND)
 *   Piezo 2    -> GPIO1     Piezo sensor 2 (+ 1M ohm to GND)
 *   Piezo 3    -> GPIO3     Piezo sensor 3 (+ 1M ohm to GND)
 *   Piezo 4    -> GPIO4     Piezo sensor 4 (+ 1M ohm to GND)
 *   LED 1      -> GPIO5     LED + 220 ohm resistor
 *   LED 2      -> GPIO6     LED + 220 ohm resistor
 *   LED 3      -> GPIO7     LED + 220 ohm resistor
 *   LED 4      -> GPIO10    LED + 220 ohm resistor
 *   Button     -> GPIO2     Calibration button (press = LOW)
 *   GND        -> GND       All sensors & LEDs share ground
 *
 * -- Serial Output Format -------------------------------------
 *
 *   1) Continuous stream (50Hz, always running):
 *      /piezo/stream d1 d2 d3 d4
 *      d1~d4: activity level per channel (filtered - baseline, min 0)
 *             idle = 0, touch/scrape = low values, hit = spike
 *
 *   2) Peak hit event (on strong transient):
 *      /piezo <channel> <velocity> <rawPeak>
 *      channel:  1~4
 *      velocity: 1~127 (mapped from peak value)
 *      rawPeak:  raw ADC peak value (0~4095)
 *
 * -- WHO Identification Protocol ------------------------------
 *   Send "WHO\n" via Serial -> responds with "ID:piezo"
 */

// =========================
// Device ID (for WHO identification)
// =========================
const char* DEVICE_ID = "piezo";

// =========================
// Pin assignment
// =========================
const int NUM_PIEZOS = 4;

// Piezo pins (ADC capable)
const int piezoPins[NUM_PIEZOS] = {0, 1, 3, 4};   // GPIO0, GPIO1, GPIO3, GPIO4

// LED pins
const int ledPins[NUM_PIEZOS]   = {5, 6, 7, 10};  // GPIO5, GPIO6, GPIO7, GPIO10

// =========================
// Tunable params
// =========================
const int THRESHOLD_DELTA[NUM_PIEZOS] = {80, 80, 80, 80};
const int MAX_READING[NUM_PIEZOS]     = {3000, 3000, 3000, 3000};

// Peak scan duration (non-blocking)
const int SCAN_TIME_MS = 15;

// Cooldown (prevent multi-trigger from single hit)
const int COOLDOWN_MS = 120;

// Consecutive hits required to confirm trigger
const int HITS_REQUIRED = 2;

// Baseline tracking speed: larger = slower
// Loop runs ~100kHz with no delay, so shift must be high
// shift 10 (1/1024) → baseline takes ~10ms to catch up
const int BASELINE_ALPHA_SHIFT = 10;

// IIR filter strength: larger = smoother but slower response
const int FILTER_SHIFT = 2;           // filtered += (raw - filtered) / 4

// LED tail settings
const int LED_TAIL_MIN_MS = 50;    // 最短拖尾（輕觸）
const int LED_TAIL_MAX_MS = 500;   // 最長拖尾（重擊）
const int LED_ACTIVITY_THRESHOLD = 50;  // rawDiff 超過多少就點亮 LED (no resistor = noisy, need higher)

// Continuous stream output interval
const unsigned long STREAM_INTERVAL_MS = 20;  // 50Hz

// =========================
// State
// =========================
struct PiezoState {
  int baseline = 0;
  int filtered = 0;
  int hitCount = 0;
  bool inited = false;

  // Non-blocking peak scan state
  bool scanningPeak = false;
  unsigned long scanStartTime = 0;
  int peakValue = 0;
};

PiezoState states[NUM_PIEZOS];
unsigned long lastTriggerTime[NUM_PIEZOS] = {0, 0, 0, 0};
unsigned long ledOffTime[NUM_PIEZOS]      = {0, 0, 0, 0};
unsigned long lastStreamTime = 0;
unsigned long lastDebugTime = 0;

// =========================
// Setup
// =========================
void setup() {
  Serial.begin(115200);

  for (int i = 0; i < NUM_PIEZOS; i++) {
    pinMode(ledPins[i], OUTPUT);
    digitalWrite(ledPins[i], LOW);
  }

  analogReadResolution(12);       // 0~4095
  analogSetAttenuation(ADC_11db); // ~0~3.3V range

  // --- LED hardware test: blink all 4 LEDs on startup ---
  for (int i = 0; i < NUM_PIEZOS; i++) {
    digitalWrite(ledPins[i], HIGH);
  }
  delay(300);
  for (int i = 0; i < NUM_PIEZOS; i++) {
    digitalWrite(ledPins[i], LOW);
  }

  for (int i = 0; i < NUM_PIEZOS; i++) {
    initPiezo(states[i], piezoPins[i]);
  }

  Serial.println("Ready");
}

// =========================
// Main loop
// =========================
void loop() {
  // Handle WHO identification command
  handleSerialCommand();

  for (int i = 0; i < NUM_PIEZOS; i++) {
    checkPiezo(i);
  }

  sendStream();
  updateLEDs();
  debugPrint();
}

// =========================
// Continuous stream output (50Hz)
// Format: /piezo/stream d1 d2 d3 d4
// =========================
void sendStream() {
  unsigned long now = millis();
  if (now - lastStreamTime < STREAM_INTERVAL_MS) return;
  lastStreamTime = now;

  // Skip if serial TX buffer is low
  if (Serial.availableForWrite() < 40) return;

  Serial.print("/piezo/stream");
  for (int i = 0; i < NUM_PIEZOS; i++) {
    int diff = states[i].filtered - states[i].baseline;
    if (diff < 0) diff = 0;
    Serial.print(" ");
    Serial.print(diff);
  }
  Serial.println();
}

// =========================
// Serial command handler (WHO protocol, non-blocking)
// =========================
char cmdBuf[16];
int cmdBufIdx = 0;

void handleSerialCommand() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (cmdBufIdx > 0) {
        cmdBuf[cmdBufIdx] = '\0';
        if (strcmp(cmdBuf, "WHO") == 0) {
          Serial.println("ID:" + String(DEVICE_ID));
        }
        cmdBufIdx = 0;
      }
    } else if (cmdBufIdx < 15) {
      cmdBuf[cmdBufIdx++] = c;
    }
  }
}

// =========================
// Init baseline
// =========================
void initPiezo(PiezoState &st, int pin) {
  long sum = 0;
  const int N = 50;

  for (int i = 0; i < N; i++) {
    sum += analogRead(pin);
    delay(2);
  }

  int avg = (int)(sum / N);
  st.baseline = avg;
  st.filtered = avg;
  st.hitCount = 0;
  st.inited = true;
  st.scanningPeak = false;
  st.scanStartTime = 0;
  st.peakValue = avg;
}

// =========================
// Check one piezo
// =========================
void checkPiezo(int index) {
  int pin = piezoPins[index];
  PiezoState &st = states[index];

  int raw = analogRead(pin);

  // -------------------------
  // Always update IIR filter (even during peak scan)
  // -------------------------
  st.filtered += (raw - st.filtered) >> FILTER_SHIFT;

  // -------------------------
  // LED: use RAW - baseline (not filtered) for instant response
  // New signal always resets the timer (no waiting for previous)
  // -------------------------
  int rawDiff = raw - st.baseline;
  if (rawDiff < 0) rawDiff = 0;

  if (rawDiff > LED_ACTIVITY_THRESHOLD) {
    // Map rawDiff to tail duration: stronger = longer tail
    int tailMs = map(rawDiff, LED_ACTIVITY_THRESHOLD, MAX_READING[index], LED_TAIL_MIN_MS, LED_TAIL_MAX_MS);
    tailMs = constrain(tailMs, LED_TAIL_MIN_MS, LED_TAIL_MAX_MS);

    digitalWrite(ledPins[index], HIGH);
    ledOffTime[index] = millis() + tailMs;  // Always overwrite — new signal resets
  }

  // -------------------------
  // A. Peak scanning: update peak, send when time's up
  //    (baseline NOT updated during scan to avoid inflation)
  // -------------------------
  if (st.scanningPeak) {
    if (raw > st.peakValue) {
      st.peakValue = raw;
    }

    if (millis() - st.scanStartTime >= SCAN_TIME_MS) {
      sendSimpleMessage(index, st.peakValue);
      // Force LED on for peak hit (guaranteed visual feedback)
      digitalWrite(ledPins[index], HIGH);
      ledOffTime[index] = millis() + LED_TAIL_MAX_MS;
      st.scanningPeak = false;
      st.hitCount = 0;
      lastTriggerTime[index] = millis();
    }
    return;
  }

  // Update baseline only in normal mode (not during peak scan)
  st.baseline += (st.filtered - st.baseline) >> BASELINE_ALPHA_SHIFT;

  if (rawDiff > THRESHOLD_DELTA[index]) {
    st.hitCount++;
  } else {
    st.hitCount = 0;
  }

  if (st.hitCount >= HITS_REQUIRED &&
      (millis() - lastTriggerTime[index] > COOLDOWN_MS)) {

    // Enter non-blocking peak scan state
    st.scanningPeak = true;
    st.scanStartTime = millis();
    st.peakValue = raw;
  }
}

// =========================
// Serial output
// Format: /piezo <channel> <velocity> <rawPeak>
// =========================
void sendSimpleMessage(int index, int rawPeak) {
  // Skip if serial TX buffer is nearly full (prevent blocking delay)
  // A typical message is ~25 bytes, require at least 30 bytes free
  if (Serial.availableForWrite() < 30) {
    return;  // Drop this message to keep loop responsive
  }

  int effectiveThreshold = states[index].baseline + THRESHOLD_DELTA[index];

  int velocity = map(rawPeak, effectiveThreshold, MAX_READING[index], 1, 127);
  velocity = constrain(velocity, 1, 127);

  int channel = index + 1;

  Serial.print("/piezo ");
  Serial.print(channel);
  Serial.print(" ");
  Serial.print(velocity);
  Serial.print(" ");
  Serial.println(rawPeak);
}

// =========================
// Non-blocking LED update
// =========================
void updateLEDs() {
  unsigned long now = millis();

  for (int i = 0; i < NUM_PIEZOS; i++) {
    if (ledOffTime[i] > 0 && now >= ledOffTime[i]) {
      digitalWrite(ledPins[i], LOW);
      ledOffTime[i] = 0;
    }
  }
}

// =========================
// Debug: print diff values every 500ms
// (remove this once LED is working)
// =========================
void debugPrint() {
  unsigned long now = millis();
  if (now - lastDebugTime < 500) return;
  lastDebugTime = now;

  if (Serial.availableForWrite() < 60) return;

  Serial.print("[DBG] filt:");
  for (int i = 0; i < NUM_PIEZOS; i++) {
    Serial.print(" ");
    Serial.print(states[i].filtered);
  }
  Serial.print("  base:");
  for (int i = 0; i < NUM_PIEZOS; i++) {
    Serial.print(" ");
    Serial.print(states[i].baseline);
  }
  Serial.println();
}
