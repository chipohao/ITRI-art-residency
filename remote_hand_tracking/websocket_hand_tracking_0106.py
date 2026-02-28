"""
WebSocket Hand Tracking API
提供即時手部追蹤座標和速度資訊
【已加入 EMA 平滑化修正，改善鋸齒與抖動】
"""

import cv2
import mediapipe as mp
import asyncio
import websockets
import json
import time
import math
from collections import deque
import numpy as np


class PoseDetector:
    def __init__(self, max_num_hands=1, MAX_POINTS=20, SMOOTHING_FACTOR=0.2):
        self.max_num_hands = max_num_hands
        self.mp_hands = mp.solutions.hands
        self.hands = self.mp_hands.Hands(
            max_num_hands=self.max_num_hands,
            min_detection_confidence=0.5,
            min_tracking_confidence=0.5
        )

        # 歷史座標（已改為儲存「平滑後」座標）
        self.position_history = deque(maxlen=5)
        self.time_history = deque(maxlen=5)

        # === 平滑化改善修正 START ===
        self.smoothed_points = []
        self.SMOOTHING_FACTOR = SMOOTHING_FACTOR
        self.MAX_POINTS = MAX_POINTS
        # === 平滑化改善修正 END ===

    # === 平滑化改善修正 START ===
    def smooth_point(self, new_point):
        """
        使用 Exponential Moving Average (EMA) 進行時間平滑
        可有效抑制 MediaPipe landmark 抖動
        """
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
    # === 平滑化改善修正 END ===

    def findFinger(self, img, enable_dead_zone=False, dead_zone_vertical_x_value=960):
        """偵測食指尖端位置（已加入平滑化）"""
        imgRGB = cv2.cvtColor(img, cv2.COLOR_BGR2RGB)
        results = self.hands.process(imgRGB)

        fingerList = []
        current_time = time.time()

        if results.multi_hand_landmarks:
            for hand_landmarks in results.multi_hand_landmarks:
                finger_tip = hand_landmarks.landmark[
                    self.mp_hands.HandLandmark.INDEX_FINGER_TIP
                ]

                h, w, _ = img.shape

                # 原始浮點座標（不先轉 int）
                raw_x = finger_tip.x * w
                raw_y = finger_tip.y * h

                # === 平滑化改善修正 START ===
                smooth_x, smooth_y = self.smooth_point((raw_x, raw_y))
                cx, cy = int(smooth_x), int(smooth_y)
                # === 平滑化改善修正 END ===

                if enable_dead_zone and cx > dead_zone_vertical_x_value:
                    cx = dead_zone_vertical_x_value

                fingerList.append([cx, cy])

                # 使用「平滑後座標」計算速度
                self.position_history.append((smooth_x, smooth_y))
                self.time_history.append(current_time)

        if fingerList:
            return {"fingerList": fingerList, "timestamp": current_time}
        return None

    def findObject(self, img, enable_dead_zone=False, dead_zone_vertical_x_value=960, object_size_th=1500):
        """偵測綠色物體中心點（已加入平滑化）"""
        fingerList = []
        current_time = time.time()

        hsv_image = cv2.cvtColor(img, cv2.COLOR_BGR2HSV)
        lower_green = np.array([40, 40, 40])
        upper_green = np.array([80, 255, 255])

        mask = cv2.inRange(hsv_image, lower_green, upper_green)
        contours, _ = cv2.findContours(mask, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)

        filtered = [c for c in contours if cv2.contourArea(c) >= object_size_th]

        if len(filtered) == 1:
            cnt = filtered[0]
            m = cv2.moments(cnt)

            if m["m00"] != 0:
                raw_x = m["m10"] / m["m00"]
                raw_y = m["m01"] / m["m00"]

                # === 平滑化改善修正 START ===
                smooth_x, smooth_y = self.smooth_point((raw_x, raw_y))
                cx, cy = int(smooth_x), int(smooth_y)
                # === 平滑化改善修正 END ===

                if enable_dead_zone and cx > dead_zone_vertical_x_value:
                    cx = dead_zone_vertical_x_value

                fingerList.append([cx, cy])
                self.position_history.append((smooth_x, smooth_y))
                self.time_history.append(current_time)

        if fingerList:
            return {"fingerList": fingerList, "timestamp": current_time}
        return None

    def calculate_velocity(self):
        """以平滑後座標計算平均速度 (自然去除抖動) """
        if len(self.position_history) < 2:
            return 0.0

        velocities = []

        for i in range(len(self.position_history) - 1):
            x1, y1 = self.position_history[i]
            x2, y2 = self.position_history[i + 1]

            t1 = self.time_history[i]
            t2 = self.time_history[i + 1]

            dt = t2 - t1
            if dt > 0:
                dist = math.hypot(x2 - x1, y2 - y1)
                velocities.append(dist / dt)

        return sum(velocities) / len(velocities) if velocities else 0.0

    def get_tracking_data(self):
        if not self.position_history:
            return None

        x, y = self.position_history[-1]
        return {
            "x": int(x),
            "y": int(y),
            "velocity": round(self.calculate_velocity(), 2),
            "timestamp": self.time_history[-1]
        }


class HandTrackingAPI:
    def __init__(self, camera_id=0, mode="finger", width=1920, height=1080):
        self.cap = cv2.VideoCapture(camera_id)
        self.cap.set(cv2.CAP_PROP_FRAME_WIDTH, 854)
        self.cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 480)

        self.detector = PoseDetector()
        self.mode = mode
        self.width = width
        self.height = height
        self.running = False
        self.clients = set()

    async def handle_client(self, websocket):
        self.clients.add(websocket)

        await websocket.send(json.dumps({
            "type": "connection",
            "status": "connected",
            "mode": self.mode
        }))

        try:
            async for msg in websocket:
                data = json.loads(msg)
                if data.get("command") == "change_mode":
                    self.mode = data.get("mode", "finger")
        finally:
            self.clients.remove(websocket)

    async def broadcast_tracking_data(self, data):
        if self.clients:
            websockets.broadcast(self.clients, json.dumps({
                "type": "tracking_data",
                "data": data
            }))

    async def video_processing_loop(self):
        self.running = True
        while self.running:
            success, img = self.cap.read()
            if not success:
                await asyncio.sleep(1)
                continue

            img = cv2.resize(img, (self.width, self.height))
            img = cv2.flip(img, 1)

            if self.mode == "finger":
                pose = self.detector.findFinger(img, enable_dead_zone=True)
            else:
                pose = self.detector.findObject(img, enable_dead_zone=True)

            if pose:
                data = self.detector.get_tracking_data()
                if data:
                    await self.broadcast_tracking_data(data)

            await asyncio.sleep(0.033)

    async def start_server(self, host="0.0.0.0", port=8765):
        async with websockets.serve(self.handle_client, host, port):
            await self.video_processing_loop()

    def stop(self):
        self.running = False
        self.cap.release()


async def main():
    api = HandTrackingAPI()
    await api.start_server()


if __name__ == "__main__":
    asyncio.run(main())
