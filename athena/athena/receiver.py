"""Listens for the ESP32-S3 UDP stream and keeps the most recent frames."""
import socket
import threading
import time
from collections import deque

import numpy as np

from . import proto

FEATURES = 64


class CsiReceiver:
    def __init__(self, port=5005, keep=4000):
        self.port = port
        self.frames = deque(maxlen=keep)       # (ts_ms, amp[64] uint8)
        self.lock = threading.Lock()
        self.total = 0
        self.lost = 0
        self.rssi = 0
        self.last_rx = 0.0
        self._arrivals = deque(maxlen=400)     # local clock, for fps
        self._last_seq = None
        self._rec = None                       # list while recording
        self._stop = threading.Event()
        self._thread = None
        self.error = ""

    # ------------------------------------------------------------ socket
    def start(self):
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def stop(self):
        self._stop.set()

    def _run(self):
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            s.bind(("", self.port))
            s.settimeout(0.5)
        except OSError as e:
            self.error = f"cannot listen on UDP {self.port}: {e}"
            return
        while not self._stop.is_set():
            try:
                pkt, _ = s.recvfrom(2048)
            except socket.timeout:
                continue
            except OSError:
                break
            r = proto.parse(pkt)
            if r is None:
                continue
            self._on_frame(*r)

    def _on_frame(self, seq, ts, rssi, amp):
        now = time.time()
        with self.lock:
            if self._last_seq is not None:
                gap = (seq - self._last_seq - 1) & 0xFFFFFFFF
                if gap < 10000:
                    self.lost += gap
                else:                       # device rebooted
                    self.frames.clear()
            self._last_seq = seq
            self.total += 1
            self.rssi = rssi
            self.last_rx = now
            self._arrivals.append(now)
            item = (ts, amp.copy())
            self.frames.append(item)
            if self._rec is not None:
                self._rec.append((ts, rssi, item[1]))

    # ------------------------------------------------------------- stats
    def fps(self):
        now = time.time()
        with self.lock:
            n = sum(1 for t in self._arrivals if now - t <= 2.0)
        return n / 2.0

    def connected(self):
        return (time.time() - self.last_rx) < 2.0

    def loss_pct(self):
        with self.lock:
            tot = self.total + self.lost
            return 100.0 * self.lost / tot if tot else 0.0

    # ------------------------------------------------------------ access
    def recent(self, seconds):
        """Frames from the last `seconds` of device time -> (ts_ms[], amp[N,64])."""
        with self.lock:
            if not self.frames:
                return np.zeros(0), np.zeros((0, FEATURES))
            last = self.frames[-1][0]
            sel = [f for f in self.frames if last - f[0] <= seconds * 1000]
        ts = np.array([f[0] for f in sel], dtype=np.float64)
        amp = np.stack([f[1] for f in sel]).astype(np.float32)
        return ts, amp

    def last_n(self, n):
        with self.lock:
            sel = list(self.frames)[-n:]
        if not sel:
            return np.zeros((0, FEATURES), np.float32)
        return np.stack([f[1] for f in sel]).astype(np.float32)

    # --------------------------------------------------------- recording
    def start_recording(self):
        with self.lock:
            self._rec = []

    def stop_recording(self):
        with self.lock:
            rec, self._rec = self._rec, None
        if not rec:
            return None
        ts = np.array([r[0] for r in rec], dtype=np.uint32)
        rssi = np.array([r[1] for r in rec], dtype=np.int8)
        amp = np.stack([r[2] for r in rec]).astype(np.uint8)
        return ts, rssi, amp

    def recording_count(self):
        with self.lock:
            return len(self._rec) if self._rec is not None else 0
