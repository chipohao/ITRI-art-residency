# Duo Pulse — Sonification 設計筆記

## 系統架構（Max patch）

```
ESP32 (serial 115200)
    |
[serial] → parse → [route /pulse/userA/bpm /pulse/userA/beat ...]
                        |
              [prepend script setMaxBpmA]
                        |
                    [jweb heartbeats-harmony.html]   ← 視覺化 + 計算 pen 座標
                        |
              [route /pen/x /pen/y]
                    |        |
                  x 座標   y 座標  (-1.0 ~ +1.0)
```

---

## 視覺化狀態機制

頁面根據 finger 狀態控制 Lissajous 的維度：

| fingerA | fingerB | pen dot 行為 | 視覺 |
|---------|---------|-------------|------|
| off | off | 靜止在中心 | 兩個圓盤都暗，顯示「— —」 |
| on | off | 水平線段來回 | A 圓盤亮、旋轉；B 暗 |
| off | on | 垂直線段來回 | B 圓盤亮、旋轉；A 暗 |
| on | on | 完整 Lissajous 曲線 | 兩圓盤都亮 |

切換時有平滑過渡（指數衰減，約 2-3 秒）。

頁面下方有 `A on/off`、`B on/off` toggle 按鈕，可手動模擬 finger 狀態。接上硬體或 Max jweb 時，按鈕會自動同步。

---

## Pen Dot 座標說明

`heartbeats-harmony.html` 中，pen dot 以 Lissajous 方程式運動：

```
x = sin(freqA × t) × ampX          ← Heart A 驅動，ampX 由 fingerA 控制
y = sin(freqB × t + phase) × ampY  ← Heart B 驅動，ampY 由 fingerB 控制
```

- 輸出範圍：-1.0 ~ +1.0（正規化，**0 = 圖形正中央**）
- 輸出位址：`/pen/x`、`/pen/y`（每幀輸出，約 60Hz，從 [jweb] outlet 送出）
- `freqA` = BPM_A / 60，`freqB` = BPM_B / 60
- 視覺比例：LRX=255, LRY=125（長方形，寬高比約 2:1）

---

## 兩種節拍來源的差異

| | 硬體 beat (`/pulse/userX/beat`) | Lissajous zero crossing |
|--|------|------|
| 來源 | ESP32 直接偵測脈搏波峰 | `sin()` 自由振盪過零 |
| 節奏感 | 有 HRV（心率變異），每下間距自然不同 | 等速、機械式 |
| 時間對齊 | 跟實際脈搏同步 | 跟視覺同步，跟實際脈搏有相位差 |
| 適合用途 | 觸發跟「心跳」直覺相關的聲音 | 跟畫面連動的連續 modulation |

建議不要用 zero crossing 來替代硬體 beat。Lissajous 的價值在於提供 2D 連續信號，硬體 beat 才是真正的心跳時間點。

---

## Max 接線：極座標轉換（`[cartopol]`）

一個物件同時取得 radius 和 angle：

```
[route /pen/x /pen/y]
 |              |
 |         [t f b]
 |          |   |
 [f 0.]←────┘   |     ← y 到的時候，bang 把存好的 x 從 [f] 送出
  |              |
  [cartopol]←────┘
  |        |
radius    angle
(0~1.414) (-π ~ +π)
```

### radius 的意義

pen dot 離中心的距離。radius 接近 0 代表兩個軸的振盪同時接近零——兩顆心跳「交會」的瞬間。

### angle 的意義

pen dot 在 2D 空間中的方位角。隨 Lissajous 軌跡連續旋轉或擺動，變化速度取決於兩心跳的頻率比。

---

## Max 接線：signal-rate 插值

60Hz 的離散值做 audio modulation 會有階梯感。接收後用 `[line~]` 插值：

```
[route /pen/x]
 |
[pack 0. 16]        ← 值 + 16ms 過渡時間（≈ 1 frame interval）
 |
[line~]             ← 插值到 signal rate
 |
→ 可直接接 [*~]、[phasor~] 等 signal 物件
```

---

## 聲音化：技術參數映射

以下是所有可用的映射參數，都已實測過：

| 參數 | 計算方式 | 值域 | 映射目標 |
|------|----------|------|----------|
| x | 直接用 | -1.0 ~ +1.0 | stereo pan |
| y | 直接用 | -1.0 ~ +1.0 | filter cutoff、pitch |
| radius | `[cartopol]` 左 outlet | 0 ~ 1.414 | 音量、reverb wet、filter |
| angle | `[cartopol]` 右 outlet | -π ~ +π | 音高選擇、LFO rate |
| speed | 連續兩幀的 pen 距離 | ≥ 0 | granular 密度 |
| 硬體 beat | `/pulse/userX/beat 1` | bang | 觸發音符、打擊 |
| 交會偵測 | radius < 0.15 | bang | 特殊事件觸發 |

---

## 聲音化：設計問題

以上參數映射都試過了。技術上可行，但效果不理想：

### 觀眾端的問題

- 連續 modulation（pan、filter、reverb）聽起來像「有東西在動」，但觀眾無法理解「這跟心跳有什麼關係」
- 缺乏可辨識的錨點。觀眾沒有心跳的聽覺參照，只聽到持續變化的聲音
- x → pan、radius → filter 這些映射在技術上合理，但感知上太抽象

### 參與者端的問題

- 把心跳映射成 kick/snare：節奏正確，但感受不到「這是我的心跳」
- 連續 modulation 太平滑，參與者感覺不到自己的心跳正在影響什麼
- 參與者需要「因果感」——我的心跳 → 一個可辨識的聲音反應

### 關係表達的問題

- 兩顆心跳各自觸發各自的聲音（A 打大鼓、B 打小鼓）= 兩條平行線，沒有交集
- polyrhythm 在音樂理論上成立，但普通觀眾聽不出 BPM 比例的微妙變化
- 「交會」的概念需要有聽覺上的對比——交會 vs 不交會之間要有明確的聲響差異

---

## 待解決：sonification 策略方向

以下是幾個尚未嘗試、可能值得探索的方向。核心目標是讓觀眾「聽懂」、讓參與者「感受到」。

### 方向 A：以心跳聲為錨點

用接近真實心跳的聲音（低頻的 lub-dub）作為基礎層。觀眾立刻知道「這是心跳」。在此基礎上：

- 每個人的心跳音色略有不同（音高、泛音結構）
- 兩顆心跳接近同步時，合在一起產生共鳴或和聲
- 不同步時，各自的聲音保持分離

優點：直覺、不需要解釋。缺點：可能太 literal，缺乏音樂性。

### 方向 B：持續音景 + 心跳事件

一層持續的 drone 或 pad，由 Lissajous 的 radius/angle 控制音色（暗 ↔ 亮、薄 ↔ 厚）。這是「環境」。

硬體 beat 觸發短促的音符事件疊在上面。這是「脈搏」。

- drone 讓觀眾持續感受到「有東西活著」
- beat 事件讓參與者感受到「這是我的心跳在觸發」
- drone 的音色變化跟視覺同步（radius 大 = 聲音開 = 畫面在邊緣），觀眾可以視聽對照

### 方向 C：聚焦「交會」作為戲劇高點

大部分時間保持安靜或極簡。只有兩顆心跳交會時（radius 接近 0）才有明顯的聲音事件。

- 交會 = 和弦解決、鐘聲、共振
- 非交會 = 環境音、呼吸聲、極低的 drone
- 交會的頻率取決於 BPM 比例，觀眾能直覺感受到「它們有時候會對在一起」

優點：高對比，戲劇性強。缺點：大部分時間可能太安靜。

### 方向 D：身體共振

用 sub-bass（< 60Hz）在低音喇叭或震動元件上輸出，讓參與者身體感受到自己的心跳被放大。不是用耳朵「聽」，而是用身體「感覺」。

- 每個硬體 beat → 一個短促的低頻脈衝
- 頻率可以對應 BPM（更低的心率 = 更低沉的脈衝）
- 兩人的脈衝在同一個空間交疊，同步時身體感受到加強，不同步時感受到干涉

---

## jweb 使用方式

1. Max patch 新增 `[jweb]` 物件
2. 送 `[loadbang]` → `[read <絕對路徑>/heartbeats-harmony.html]` → `[jweb]`
3. 視窗縮放：送 `[zoom 0.8]` → `[jweb]`（等比縮小至 80%）
4. 頁面偵測到 Max 環境後自動進入 **max mode**（隱藏 serial 控制項）

### Max → jweb（送 BPM / beat / finger）

```
[prepend script setMaxBpmA]   → [jweb]   ← 更新 Heart A 的 BPM
[prepend script setMaxBpmB]   → [jweb]   ← 更新 Heart B 的 BPM
[prepend script setMaxBeatA]  → [jweb]   ← 觸發 Heart A 的 beat flash
[prepend script setMaxBeatB]  → [jweb]   ← 觸發 Heart B 的 beat flash
[prepend script setMaxFingerA 1] → [jweb] ← 設定手指狀態（0 或 1）
[prepend script setMaxFingerB 1] → [jweb]
```

### jweb → Max（pen 座標）

```
[jweb] outlet
    |
[route /pen/x /pen/y]
```
