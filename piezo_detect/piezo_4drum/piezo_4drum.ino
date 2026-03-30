/*
 * Piezo 4-Drum — ESP32-C3 SuperMini + 4x Piezo + 4x LED
 *
 * Non-blocking version:
 *   1) Dynamic baseline tracking
 *   2) Relative threshold trigger
 *   3) Consecutive hit debounce
 *   4) IIR filter
 *   5) Non-blocking peak capture (no while loop)
 *   6) Non-blocking LED flash
 *   7) Serial buffer overflow protection (prevent blocking delay)
 *   8) WHO identification protocol for multi-device setup
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
 *   /piezo <channel> <velocity> <rawPeak>
 *   Example:
 *   /piezo 1 88 3084
 *   /piezo 3 112 3560
 *
 *   channel:  1~4
 *   velocity: 1~127 (mapped from peak value)
 *   rawPeak:  raw ADC peak value (0~4095)
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
const int BASELINE_ALPHA_SHIFT = 6;   // baseline += (filtered - baseline) / 64

// IIR filter strength: larger = smoother but slower response
const int FILTER_SHIFT = 2;           // filtered += (raw - filtered) / 4

// LED on duration (non-blocking)
const int LED_ON_MS = 30;

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

  updateLEDs();
}

// =========================
// Serial command handler (WHO protocol)
// =========================
void handleSerialCommand() {
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd == "WHO") {
      Serial.println("ID:" + String(DEVICE_ID));
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
  // Always update IIR filter & baseline (even during peak scan)
  // This prevents baseline drift when scanning frequently
  // -------------------------
  st.filtered += (raw - st.filtered) >> FILTER_SHIFT;
  st.baseline += (st.filtered - st.baseline) >> BASELINE_ALPHA_SHIFT;

  // -------------------------
  // A. Peak scanning: update peak, send when time's up
  // -------------------------
  if (st.scanningPeak) {
    if (raw > st.peakValue) {
      st.peakValue = raw;
    }

    if (millis() - st.scanStartTime >= SCAN_TIME_MS) {
      sendSimpleMessage(index, st.peakValue);
      st.scanningPeak = false;
      st.hitCount = 0;
      lastTriggerTime[index] = millis();
    }
    return;
  }

  int diff = st.filtered - st.baseline;

  if (diff > THRESHOLD_DELTA[index]) {
    st.hitCount++;
  } else {
    st.hitCount = 0;
  }

  if (st.hitCount >= HITS_REQUIRED &&
      (millis() - lastTriggerTime[index] > COOLDOWN_MS)) {

    // Trigger: LED on immediately
    triggerLED(index);

    // Enter non-blocking peak scan state
    st.scanningPeak = true;
    st.scanStartTime = millis();
    st.peakValue = raw;
  }
}

// =========================
// LED trigger
// =========================
void triggerLED(int index) {
  digitalWrite(ledPins[index], HIGH);
  ledOffTime[index] = millis() + LED_ON_MS;
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
