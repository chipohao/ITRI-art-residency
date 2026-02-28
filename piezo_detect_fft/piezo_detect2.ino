#include <arduinoFFT.h>
#include "driver/i2s.h"
#include "driver/adc.h"

// ========== 1. 硬體與參數配置 ==========
#define LED_PIN 2            // 板載藍燈
#define RX_PIN 16            // 定義 RX (雖然沒用到，但避免衝突)
#define TX_PIN 17            // 定義 TX

// 降低 HOP_SIZE 到 1024 (約 15 fps)，確保數據絕對乾淨
static const int SAMPLE_RATE = 16000;
static const int FFT_SIZE = 1024;
static const int HOP_SIZE = 1024; 
static const int NUM_BANDS = 32;

static const adc1_channel_t ADC_CH = ADC1_CHANNEL_0; // VP (GPIO36)
static const i2s_port_t I2S_PORT = I2S_NUM_0;

// ========== 2. 全域變數 ==========
double vReal[FFT_SIZE];
double vImag[FFT_SIZE];
double timeBuffer[FFT_SIZE];

ArduinoFFT<double> FFT(vReal, vImag, FFT_SIZE, SAMPLE_RATE);

int16_t i2sRaw[HOP_SIZE];
int sampleFill = 0;
uint16_t bandStart[NUM_BANDS];
uint16_t bandEnd[NUM_BANDS];
float emaEnergy = 0.0f;

// ========== 3. 工具函式 ==========
static inline float fastLogMag(float x) { return log1pf(x); }

void buildLogBands() {
  const int binMin = 2; // 避開 DC (0Hz) 與極低頻雜訊
  const int binMax = (FFT_SIZE / 2) - 1;
  for (int b = 0; b < NUM_BANDS; b++) {
    float t0 = (float)b / (float)NUM_BANDS;
    float t1 = (float)(b + 1) / (float)NUM_BANDS;
    int s = (int)roundf(binMin * powf((float)binMax / (float)binMin, t0));
    int e = (int)roundf(binMin * powf((float)binMax / (float)binMin, t1));
    if (e <= s) e = s + 1;
    bandStart[b] = (uint16_t)s;
    bandEnd[b] = (uint16_t)e;
  }
}

void setupI2S() {
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_ADC_BUILT_IN),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S_MSB,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 256
  };
  i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
  i2s_set_adc_mode(ADC_UNIT_1, ADC_CH);
  i2s_adc_enable(I2S_PORT);
}

// ========== 4. 主程式 ==========
void setup() {
  // 改回最穩定的 115200
  Serial.begin(115200);
  
  pinMode(LED_PIN, OUTPUT);
  
  // --- 硬體檢查：開機閃爍三下 ---
  // 如果你在這時候沒看到藍燈閃，代表板子壞了或 LED 腳位不是 2
  for(int i=0; i<3; i++) {
    digitalWrite(LED_PIN, HIGH); delay(100);
    digitalWrite(LED_PIN, LOW);  delay(100);
  }
  
  buildLogBands();
  setupI2S();
  
  // 初始化 buffer
  for (int i = 0; i < FFT_SIZE; i++) timeBuffer[i] = 0.0;
  sampleFill = 0;

  Serial.println("\n=== System Ready: 115200 baud ===");
}

void loop() {
  size_t bytesRead = 0;
  i2s_read(I2S_PORT, (void*)i2sRaw, sizeof(i2sRaw), &bytesRead, portMAX_DELAY);
  int count = bytesRead / sizeof(int16_t);

  for (int i = 0; i < count; i++) {
    if (sampleFill < FFT_SIZE) {
      uint16_t raw = (uint16_t)i2sRaw[i];
      uint16_t adc12 = (raw >> 4) & 0x0FFF;
      // 轉換並移除 DC (假設中心點 2048)
      timeBuffer[sampleFill++] = ((double)adc12 - 2048.0) / 2048.0;
    }

    if (sampleFill >= FFT_SIZE) {
      memcpy(vReal, timeBuffer, sizeof(double) * FFT_SIZE);
      memset(vImag, 0, sizeof(double) * FFT_SIZE);

      FFT.windowing(FFT_WIN_TYP_HAMMING, FFT_FORWARD);
      FFT.compute(FFT_FORWARD);
      FFT.complexToMagnitude();

      float totalEnergy = 0.0f;
      float hfEnergy = 0.0f;
      
      // 使用 String 建構完整字串，避免 UART 中斷打架
      String msg = "FFT ";

      for (int b = 0; b < NUM_BANDS; b++) {
        float sum = 0.0f;
        for (int k = bandStart[b]; k <= bandEnd[b]; k++) {
          sum += (float)vReal[k];
        }
        float v = fastLogMag(sum / (float)(bandEnd[b] - bandStart[b] + 1));
        
        msg += String(v, 2); // 只留兩位小數
        msg += " ";
        
        totalEnergy += v;
        if (b >= 21) hfEnergy += v;
      }

      emaEnergy = (0.9f * emaEnergy) + (0.1f * totalEnergy);
      float hfRatio = (totalEnergy > 0.1f) ? (hfEnergy / totalEnergy) : 0.0f;

      // 修正後的 LED 邏輯：只要有能量波動就亮，方便你除錯
      // 如果總能量持續過大(雜訊)，LED 就不會亮(被 ema 追上)
      int hit = (totalEnergy > (emaEnergy * 2.0f + 1.0f)) ? 1 : 0;
      int scrub = (hfRatio > 0.45f && totalEnergy > (emaEnergy * 1.2f)) ? 1 : 0;

      // 除錯：如果訊號爆表 (4.78 * 32 bands approx > 100)，強制亮燈警告
      bool signalSaturated = (totalEnergy > 100.0f); 

      if (hit || scrub || signalSaturated) {
         digitalWrite(LED_PIN, HIGH);
      } else {
         digitalWrite(LED_PIN, LOW);
      }

      msg += String(hit);
      msg += " ";
      msg += String(scrub);
      
      // 一次送出整行，保證不亂碼
      Serial.println(msg);

      // Overlap Shift (這次是 100% update，因為 HOP=FFT_SIZE，這是最安全的模式)
      // 如果要滑動窗口，這裡邏輯要改，但為了除錯先不重疊
      sampleFill = 0; 
    }
  }
}