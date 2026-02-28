/*
 * ESP32 Single Channel Piezo Streamer for FluCoMa / MuBu
 * 腳位說明：Piezo 紅線接 VP (GPIO 36)，黑線接 GND
 * 並聯電阻：5.1k (或 1M) 接在 VP 與 GND 之間
 */

const int PIEZO_PIN = 36; // VP 腳位在程式中就是 36

// 採樣頻率設定為 8000Hz (適合大多數機器學習特徵提取)
const int SAMPLE_RATE_HZ = 8000;
const int SAMPLE_INTERVAL_US = 1000000 / SAMPLE_RATE_HZ;

unsigned long nextSampleTime = 0;

void setup() {
  // 設定極高波特率以降低序列埠傳輸延遲
  Serial.begin(921600); 
  
  // 設定 ADC 衰減，讓輸入範圍為 0V - 3.3V
  analogSetAttenuation(ADC_11db);
  
  nextSampleTime = micros();
}

void loop() {
  unsigned long now = micros();

  // 定時採樣確保時間間隔固定 (對 FFT/MFCC 至關重要)
  if (now >= nextSampleTime) {
    int raw = analogRead(PIEZO_PIN);
    
    // 直接輸出數值，供電腦端分析
    Serial.println(raw);

    nextSampleTime += SAMPLE_INTERVAL_US;
    
    // 預防計時溢位或處理延遲
    if (now > nextSampleTime + SAMPLE_INTERVAL_US) {
      nextSampleTime = now + SAMPLE_INTERVAL_US;
    }
  }
}