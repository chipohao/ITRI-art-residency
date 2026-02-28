/*
 * Dual Pulse Sensor - ESP32 (v2.0 PulseSensorPlayground Version)
 * 
 * Features:
 * - Uses the official PulseSensorPlayground library.
 * - Industry-standard beat detection (derivatives & filtering).
 * - Throttled RAW output (100Hz) to prevent OSC/Serial flooding.
 * - Automatic Finger-On / Off detection.
 * - Simulated 200ms Heartbeat Gate (Beat OFF signal).
 */

// #define ENABLE_WIFI 

#ifdef ENABLE_WIFI
  #include <WiFi.h>
  #include <WiFiUdp.h>
  #include <OSCMessage.h>
  
  const char* ssid = "YOUR_WIFI_SSID";          
  const char* password = "YOUR_WIFI_PASSWORD";  
  IPAddress outIp(192, 168, 1, 100);            
  const unsigned int outPort = 8000;            
  WiFiUDP Udp;
#endif

// Configure PulseSensorPlayground
#define USE_ARDUINO_INTERRUPTS false // Use polling for stability across ESP32 cores and custom logic
#include <PulseSensorPlayground.h>

const int PIN_SENSOR_A = 34;
const int PIN_SENSOR_B = 35;
const int FINGER_THRESHOLD = 500; // Raw values below this mean no finger

PulseSensorPlayground pulseSensor(2); // 2 sensors

// State Variables for Sensor A
boolean fingerOnA = false;
boolean beatStateA = false;
unsigned long lastBeatTimeA = 0;

// State Variables for Sensor B
boolean fingerOnB = false;
boolean beatStateB = false;
unsigned long lastBeatTimeB = 0;

unsigned long lastRawTime = 0;

void sendSerial(String address, int val) {
  Serial.print(address); Serial.print(" "); Serial.println(val);
  
  #ifdef ENABLE_WIFI
    Udp.beginPacket(outIp, outPort);
    OSCMessage msg(address.c_str());
    msg.add((float)val);
    msg.send(Udp);
    Udp.endPacket();
    msg.empty();
  #endif
}

void setup() {
  Serial.begin(115200);
  
  #ifdef ENABLE_WIFI
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
    Serial.println("\nWiFi Connected");
  #endif
  
  // Set global threshold for the algorithm (middle of the ESP32 12-bit range is 2048)
  pulseSensor.setThreshold(2048); 
  
  pulseSensor.analogInput(PIN_SENSOR_A, 0);
  pulseSensor.analogInput(PIN_SENSOR_B, 1);
  
  if (pulseSensor.begin()) {
    Serial.println("PulseSensor Playground Initialized! (v2.0)");
  }
}

void loop() {
  unsigned long now = millis();

  // sawNewSample() reads the analog pins and processes the official algorithm at 500Hz.
  // This must be called frequently when USE_ARDUINO_INTERRUPTS is false
  if (pulseSensor.sawNewSample()) {
      
      int rawA = pulseSensor.getLatestSample(0);
      int rawB = pulseSensor.getLatestSample(1);
      
      // ---------- SENSOR A ----------
      if (rawA < FINGER_THRESHOLD) {
          if (fingerOnA) { fingerOnA = false; sendSerial("/pulse/userA/finger", 0); }
      } else {
          if (!fingerOnA) { fingerOnA = true; sendSerial("/pulse/userA/finger", 1); }
          
          if (pulseSensor.sawStartOfBeat(0)) {
              beatStateA = true;
              lastBeatTimeA = now;
              int bpmA = pulseSensor.getBeatsPerMinute(0);
              sendSerial("/pulse/userA/beat", 1);
              if (bpmA > 30) sendSerial("/pulse/userA/bpm", bpmA);
          }
      }

      // 关闭心跳訊號 (200ms 後發送 OFF)
      if (beatStateA && (now - lastBeatTimeA > 200)) {
          beatStateA = false;
          sendSerial("/pulse/userA/beat", 0);
      }

      // ---------- SENSOR B ----------
      if (rawB < FINGER_THRESHOLD) {
          if (fingerOnB) { fingerOnB = false; sendSerial("/pulse/userB/finger", 0); }
      } else {
          if (!fingerOnB) { fingerOnB = true; sendSerial("/pulse/userB/finger", 1); }
          
          if (pulseSensor.sawStartOfBeat(1)) {
              beatStateB = true;
              lastBeatTimeB = now;
              int bpmB = pulseSensor.getBeatsPerMinute(1);
              sendSerial("/pulse/userB/beat", 1);
              if (bpmB > 30) sendSerial("/pulse/userB/bpm", bpmB);
          }
      }

      // 关闭心跳訊號 (200ms 後發送 OFF)
      if (beatStateB && (now - lastBeatTimeB > 200)) {
          beatStateB = false;
          sendSerial("/pulse/userB/beat", 0);
      }

      // ---------- OUTPUT RAW DATA (Throttled to 100Hz) ----------
      // 官方套件內部是 500Hz 運作，如果我們用 500Hz 發送 Serial / OSC 會造成網路與序列埠塞車。
      // 因此我們限制每 10ms (100Hz) 傳送一次波形給 MaxMSP 畫圖即可。
      if (now - lastRawTime >= 10) {
          sendSerial("/pulse/userA/raw", rawA);
          sendSerial("/pulse/userB/raw", rawB);
          lastRawTime = now;
      }
  }
}
