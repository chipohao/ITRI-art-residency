import cv2
import mediapipe as mp
import time
import math
import sys
from collections import deque
import numpy as np
from pythonosc import udp_client

# ================= 參數設置區 =================
# 1. 平滑係數 (位置): 越小越平滑但越慢 (改為 0.6 提高反應速度)
SMOOTHING_FACTOR = 0.6
# 2. 速度修正係數: 越大反應越快 (改為 0.6)
VELOCITY_ALPHA = 0.6 
# 3. Dead Zone (X軸限制) - Normalized 0.0 ~ 1.0 (960/1280 = 0.75)
DEAD_ZONE_X = 0.75
# 4. Max 的接收設定
IP = "127.0.0.1"
PORT = 8000
# ============================================

def log(msg):
    """將訊息強制刷新到標準輸出，讓 Max Console 立即看到"""
    print(f"[PY] {msg}", flush=True)

class PoseDetector:
    def __init__(self):
        self.mp_hands = mp.solutions.hands
        self.hands = self.mp_hands.Hands(
            max_num_hands=1,
            min_detection_confidence=0.5,
            min_tracking_confidence=0.5
        )

        # 歷史與平滑化變數
        self.position_history = deque(maxlen=5)
        self.time_history = deque(maxlen=5)
        self.smoothed_points = []
        self.smoothed_points = []
        self.smoothed_vx = 0.0
        self.smoothed_vy = 0.0
        
        # 參數
        self.MAX_POINTS = 20
        self.SMOOTHING_FACTOR = SMOOTHING_FACTOR

    def smooth_point(self, new_point):
        """位置 EMA 平滑"""
        if not self.smoothed_points:
            self.smoothed_points.append(new_point)
            return new_point

        last_x, last_y = self.smoothed_points[-1]
        new_x, new_y = new_point

        smooth_x = last_x + self.SMOOTHING_FACTOR * (new_x - last_x)
        smooth_y = last_y + self.SMOOTHING_FACTOR * (new_y - last_y)

        smoothed = (smooth_x, smooth_y)
        self.smoothed_points.append(smoothed)
        
        if len(self.smoothed_points) > self.MAX_POINTS:
            self.smoothed_points.pop(0)
            
        return smoothed

    def calculate_velocity(self):
        """速度 EMA 平滑 (低延遲版) - X, Y 分開計算"""
        if len(self.position_history) < 2:
            return 0.0, 0.0

        (x1, y1) = self.position_history[-2]
        (x2, y2) = self.position_history[-1]
        t1, t2 = self.time_history[-2], self.time_history[-1]

        dt = t2 - t1
        if dt <= 0: return 0.0, 0.0

        # 分別計算 x, y 軸的瞬時速度
        instant_vx = (x2 - x1) / dt
        instant_vy = (y2 - y1) / dt

        # 分別進行 EMA 平滑
        self.smoothed_vx += VELOCITY_ALPHA * (instant_vx - self.smoothed_vx)
        self.smoothed_vy += VELOCITY_ALPHA * (instant_vy - self.smoothed_vy)

        return round(self.smoothed_vx, 2), round(self.smoothed_vy, 2)

    def findFinger(self, img):
        imgRGB = cv2.cvtColor(img, cv2.COLOR_BGR2RGB)
        results = self.hands.process(imgRGB)
        current_time = time.time()

        if results.multi_hand_landmarks:
            for hand_landmarks in results.multi_hand_landmarks:
                finger_tip = hand_landmarks.landmark[8] # 食指尖端
                h, w, _ = img.shape
                
                raw_x = finger_tip.x
                raw_y = finger_tip.y 

                # 1. 位置平滑化
                smooth_x, smooth_y = self.smooth_point((raw_x, raw_y))
                cx, cy = smooth_x, smooth_y

                # 2. Dead Zone 限制
                if cx > DEAD_ZONE_X:
                    cx = DEAD_ZONE_X

                # 3. 紀錄歷史供速度計算
                self.position_history.append((smooth_x, smooth_y))
                self.time_history.append(current_time)
                
                # 計算 velocity returns (vx, vy)
                vx, vy = self.calculate_velocity()
                return cx, cy, vx, vy
        
        return None

def run_tracker():
    # 初始化 OSC
    client = udp_client.SimpleUDPClient(IP, PORT)
    detector = PoseDetector()
    
    # 初始化攝影機
    cap = cv2.VideoCapture(0)
    cap.set(cv2.CAP_PROP_FRAME_WIDTH, 1280) # 為了精度先抓大一點，演算法會自動處理
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 720)
    
    log(f"Camera started. Sending OSC to {IP}:{PORT}")

    # 用來判斷「手是否剛移開」的標記
    hand_visible = False

    try:
        while cap.isOpened():
            success, img = cap.read()
            if not success:
                time.sleep(0.1)
                continue

            img = cv2.flip(img, 1) # 鏡像
            result = detector.findFinger(img)

            if result:
                # 偵測到手 (cx, cy are normalized 0.0-1.0)
                cx, cy, vx, vy = result
                client.send_message("/hand", [cx, cy, vx, vy])
                hand_visible = True
            else:
                # 沒偵測到手
                if hand_visible:
                    # 如果上一幀還有手，這一幀剛消失 -> 強制送出速度 0
                    # 傳送最後已知位置，但速度歸零
                    if detector.position_history:
                        last_x, last_y = detector.position_history[-1]
                        client.send_message("/hand", [last_x, last_y, 0.0, 0.0])
                        log("Hand lost - Velocity reset to 0")
                    
                    # 清空歷史，避免下次手進來時速度爆衝
                    detector.position_history.clear()
                    detector.time_history.clear()
                    detector.smoothed_points.clear()
                    detector.smoothed_vx = 0.0
                    detector.smoothed_vy = 0.0
                    hand_visible = False

            # 稍微控制一下迴圈速度，避免吃滿 CPU
            time.sleep(0.01)

    except KeyboardInterrupt:
        pass
    except Exception as e:
        log(f"Error: {e}")
    finally:
        cap.release()
        log("Camera released.")

if __name__ == "__main__":
    run_tracker()