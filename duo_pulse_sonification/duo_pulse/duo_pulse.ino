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
    
    boolean beatArmed = true;
    
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
      if (signalMax > Signal) signalMax -= 1; // Decay max down (Max 慢慢下降)
      if (signalMin < Signal) signalMin += 1; // Rise min up   (Min 慢慢上升)

      // C. Calculate Dynamic Threshold (計算動態閥值)
      // 閥值永遠維持在波形範圍的中間。
      currentThreshold = (signalMax + signalMin) / 2;
      
      int amplitude = signalMax - signalMin;
      int hysteresis = amplitude / 4; // 增加遲滯區間，必須爬升得夠高才算心跳
      if (hysteresis < 20) hysteresis = 20;

      // 3. 超穩定心跳偵測 (Debounce & Refractory Period)
      
      // [武裝機制]：波形必須掉回平均值以下，系統才會準備抓下一次的心跳
      if (Signal < currentThreshold) {
          beatArmed = true; 
      }

      // [強制關閉]：心跳發送後，強制維持 ON 一段時間 (200ms) 再關閉 (如同 pulse_gyro 控制 LED)
      if (Pulse && (now - lastBeatTime >= 200)) {
         Pulse = false;
         output(Signal, BPM, 0); // Beat OFF
      }

      // [觸發機制]：波形上升超過閥值 + 武裝完畢 + 距離上次心跳超過 300ms (防止過快連續觸發)
      if (Signal > (currentThreshold + hysteresis) && beatArmed && !Pulse) {
         if (now - lastBeatTime > 300) { 
             beatArmed = false; // 解除武裝，直到波形再次下降
             Pulse = true;
             
             // Calculate BPM (計算 BPM)
             long interval = now - lastBeatTime;
             if (interval > 300 && interval < 2000) { // 30-200 BPM range (限制合理範圍)
                 BPM = 60000 / interval;
                 output(Signal, BPM, 1); // Beat ON
             } else {
                 output(Signal, 0, 1); // Beat ON (剛開始或無效的區間)
             }
             lastBeatTime = now;
         }
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
