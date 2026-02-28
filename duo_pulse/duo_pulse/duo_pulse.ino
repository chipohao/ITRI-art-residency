/*
 * Dual Pulse Sensor - ESP32 (Smart Dynamic Version)
 * 
 * Features:
 * - Dynamic Thresholding: Automatically adjusts to signal strength (solves "Pulse B is weak" issue).
 * - Noise Filtering: Ignores small jitters.
 * - Auto-Calibration: Tracks Peaks (P) and Troughs (T) to find the perfect heartbeat cutoff.
 * 
 * Wiring:
 * - Sensor A: GPIO 34
 * - Sensor B: GPIO 35
 */

// Uncomment the following line to enable WiFi & OSC
// 解開下面這行註解以啟用 WiFi 功能
// #define ENABLE_WIFI 

#ifdef ENABLE_WIFI
  #include <WiFi.h>
  #include <WiFiUdp.h>
  #include <OSCMessage.h>
  
  // WiFi Settings
  const char* ssid = "YOUR_WIFI_SSID";          
  const char* password = "YOUR_WIFI_PASSWORD";  
  IPAddress outIp(192, 168, 1, 100);            
  const unsigned int outPort = 8000;            
  WiFiUDP Udp;
#endif

// Pin Definitions
const int PIN_SENSOR_A = 34;
const int PIN_SENSOR_B = 35;

// ==========================================
// Smart Pulse Sensor Class
// ==========================================
class PulseSensor {
  public:
    int pin;
    int ID;
    String oscPrefix;
    
    // --- Improved Rolling Min/Max Variables ---
    int signalMax = 0;
    int signalMin = 4095;
    int currentThreshold = 2048;
    
    // Beat Timing
    unsigned long lastBeatTime = 0;
    long rate[10]; 
    
    // Restored member variables
    int BPM = 0;
    int Signal = 0;
    boolean Pulse = false;
    boolean fingerOn = false;
    int fingerThreshold = 500;
    
    // --- Smoothing (Low-Pass Filter) Variables ---
    float smoothedSignal = 0;
    float slideAmount = 3.0; // 相當於 Max 中的 [slide 3 3]，數值越大越平滑，但會增加延遲
    
    PulseSensor(int pin, int id, String prefix) {
      this->pin = pin;
      this->ID = id;
      this->oscPrefix = prefix;
    }

    void update(unsigned long now) {
      int rawRead = analogRead(pin);
      
      // Smoothing Filter (指數平滑濾波，對應 Max 裡的 [slide 3 3])
      if (smoothedSignal == 0 || rawRead < fingerThreshold) {
        smoothedSignal = rawRead; // 初始化，或是無手指時直接套用不濾波
      } else {
        smoothedSignal = smoothedSignal + (rawRead - smoothedSignal) / slideAmount;
      }
      
      Signal = (int)smoothedSignal; // 將平滑後的數值交給後續系統運算
      
      // 1. Finger Detection (手指偵測)
      // If signal is very low, assume no finger and reset. (訊號太低代表沒手指，重置)
      if (Signal < fingerThreshold) {
        if (fingerOn) {
            fingerOn = false;
            sendFingerState(0); // Send Finger OFF event
        }
        
        // Reset adaptive range when finger is removed (手指移開時重置範圍)
        signalMax = 0;
        signalMin = 4095;
        output(0, 0, 0); 
        return;
      } else {
        if (!fingerOn) {
            fingerOn = true;
            sendFingerState(1); // Send Finger ON event
        }
      }

      // 2. Adaptive Range Tracking (Rolling Min/Max Algorithm) (動態範圍追蹤演算法)
      // This allows the system to adapt to any DC offset (e.g. 1700-2000 range).
      //這讓系統能適應任何訊號區間 (例如 1700-2000)
      
      // A. Expand range immediately (快速擴張範圍)
      if (Signal > signalMax) signalMax = Signal;
      if (Signal < signalMin) signalMin = Signal;

      // B. Shrink range slowly (Decay) to adapt to baseline changes (緩慢縮小範圍以適應基線變化)
      // This ensures the Min/Max doesn't get stuck at extreme values.
      // 這確保 Max/Min 不會卡在極端值，而是會慢慢貼近目前的波形。
      if (signalMax > Signal) signalMax -= 1; // Decay max down (Max 慢慢下降)
      if (signalMin < Signal) signalMin += 1; // Rise min up   (Min 慢慢上升)

      // C. Calculate Dynamic Threshold (計算動態閥值)
      // The threshold is always the midpoint of the current range.
      // 閥值永遠維持在波形範圍的中間。
      currentThreshold = (signalMax + signalMin) / 2;
      
      // D. Noise Gate (振幅檢查)
      // Ignore if amplitude is too small (noise). (如果振幅太小視為雜訊)
      int amplitude = signalMax - signalMin;
      if (amplitude < 50) { 
         // Signal too flat, probably just noise (波形太扁，可能是雜訊)
         // return;
      }

      // 3. Beat Detection (Crossing the Dynamic Threshold) (心跳偵測)
      int hysteresis = amplitude / 10; // Auto-scale hysteresis (10% of amp) (自動調整遲滯區間)
      if (hysteresis < 20) hysteresis = 20; // Min hysteresis (最小遲滯)

      // Beat Start (Rising edge) (波形上升穿過閥值 -> 心跳開始)
      if (Signal > (currentThreshold + hysteresis) && !Pulse) {
         Pulse = true;
         
         // Calculate BPM (計算 BPM)
         long interval = now - lastBeatTime;
         if (interval > 300 && interval < 2000) { // 30-200 BPM range (限制合理範圍)
             BPM = 60000 / interval;
             output(Signal, BPM, 1); // Beat ON
         } else {
             output(Signal, 0, 1); // Beat ON (invalid BPM)
         }
         lastBeatTime = now;
      }

      // Beat End (Falling edge) (波形下降穿過閥值 -> 心跳結束)
      if (Signal < (currentThreshold - hysteresis) && Pulse) {
         Pulse = false;
         output(Signal, BPM, 0); // Beat OFF
      }

      // Debug / Raw Output
      sendRaw(Signal);
    }
    
    void output(int raw, int bpm, int beatState) {
       // beatState: 0=OFF, 1=ON
       if (beatState == 1) {
          sendSerial(oscPrefix + "/beat", 1);
          if (bpm > 30) sendSerial(oscPrefix + "/bpm", bpm);
       } else if (beatState == 0) {
          sendSerial(oscPrefix + "/beat", 0);
       }
    }


    void sendFingerState(int state) {
       sendSerial(oscPrefix + "/finger", state);
    }
    
    void sendRaw(int raw) {
       sendSerial(oscPrefix + "/raw", raw);
    }
    
    void sendSerial(String address, int val) {
      Serial.print(address); Serial.print(" "); Serial.println(val);
      
      #ifdef ENABLE_WIFI
        // WiFi Send Logic
        Udp.beginPacket(outIp, outPort);
        OSCMessage msg(address.c_str());
        msg.add((float)val);
        msg.send(Udp);
        Udp.endPacket();
        msg.empty();
      #endif
    }
};

// Create Sensor Objects
PulseSensor sensorA(PIN_SENSOR_A, 0, "/pulse/userA");
PulseSensor sensorB(PIN_SENSOR_B, 1, "/pulse/userB");

void setup() {
  Serial.begin(115200);
  
  #ifdef ENABLE_WIFI
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
    Serial.println("\nWiFi Connected");
  #endif
  
  pinMode(PIN_SENSOR_A, INPUT);
  pinMode(PIN_SENSOR_B, INPUT);
}

void loop() {
  unsigned long now = millis();
  
  sensorA.update(now);
  sensorB.update(now);
  
  delay(10); // 100Hz Sample Rate
}
