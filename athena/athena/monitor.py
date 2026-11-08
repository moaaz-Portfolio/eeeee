"""Integrated CSI presence, motion and fall-event monitoring.

Important: this module deliberately reports *possible* presence/fall events. CSI
amplitude-only sensing is environment-dependent and must not be used as a
medical, emergency, or life-safety alarm without a validated study.
"""
from __future__ import annotations

import json
import os
import threading
import time
from collections import deque

import numpy as np


class PresenceMonitor:
    """Room-empty calibration + conservative presence/motion estimates."""

    def __init__(self, baseline_path: str):
        self.path = baseline_path
        self._lock = threading.RLock()
        self.baseline = None
        self.scale = None
        self.motion_floor = 0.0
        self.delta_floor = 0.0
        self.calibrated_at = None
        self.status = "unknown"  # unknown | present | empty
        self.motion_status = "unknown"
        self.score = 0.0
        self.motion_score = 0.0
        self.motion_level = 0.0
        self._present_hits = 0
        self._empty_hits = 0
        self.last_error = ""
        self._load()

    def _load(self):
        try:
            with open(self.path, "r", encoding="utf-8") as f:
                obj = json.load(f)
            base = np.asarray(obj["baseline"], dtype=np.float32)
            scale = np.asarray(obj["scale"], dtype=np.float32)
            if base.shape != (64,) or scale.shape != (64,):
                return
            with self._lock:
                self.baseline = base
                self.scale = np.maximum(scale, 1.5)
                self.motion_floor = float(obj.get("motion_floor", 0.5))
                self.delta_floor = float(obj.get("delta_floor", 0.5))
                self.calibrated_at = str(obj.get("calibrated_at", ""))
        except FileNotFoundError:
            pass
        except Exception as exc:
            self.last_error = f"baseline load failed: {exc}"

    def calibrate(self, ts, amp):
        x = np.asarray(amp, dtype=np.float32)
        if x.ndim != 2 or x.shape[1] != 64 or len(x) < 200:
            raise ValueError("محتاج 200 إطار CSI على الأقل لمعايرة الغرفة")
        x = x[-min(len(x), 1600):]
        base = np.median(x, axis=0)
        mad = 1.4826 * np.median(np.abs(x - base[None, :]), axis=0)
        scale = np.maximum(mad, 1.5)
        motion_floor = float(np.median(np.std(x, axis=0)))
        delta = np.mean(np.abs(np.diff(x, axis=0)), axis=1)
        delta_floor = float(np.percentile(delta, 95)) if len(delta) else 0.5
        stamp = time.strftime("%Y-%m-%d %H:%M:%S")
        payload = {
            "baseline": base.tolist(), "scale": scale.tolist(),
            "motion_floor": motion_floor, "delta_floor": delta_floor,
            "calibrated_at": stamp,
        }
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        tmp = self.path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(payload, f)
        os.replace(tmp, self.path)
        with self._lock:
            self.baseline, self.scale = base, scale
            self.motion_floor, self.delta_floor = motion_floor, delta_floor
            self.calibrated_at = stamp
            self.status = "unknown"
            self._present_hits = self._empty_hits = 0
            self.last_error = ""
        return {"frames": len(x), "calibrated_at": stamp}

    @property
    def calibrated(self):
        return self.baseline is not None

    def update(self, ts, amp):
        x = np.asarray(amp, dtype=np.float32)
        if x.ndim != 2 or x.shape[1] != 64 or len(x) < 30:
            return self.snapshot()
        x = x[-min(len(x), 180):]  # roughly the last 2 s at a 90 Hz stream
        motion = float(np.median(np.std(x, axis=0)))
        with self._lock:
            self.motion_level = motion
            motion_th = max(1.25, self.motion_floor * 2.5) if self.calibrated else 1.5
            self.motion_score = motion / max(motion_th, 0.1)
            self.motion_status = "moving" if motion >= motion_th else "still"

            if self.calibrated:
                current = np.median(x, axis=0)
                z = np.abs(current - self.baseline) / np.maximum(self.scale, 1.5)
                self.score = float(np.median(z))
                # A modest signature shift across many subcarriers is stronger
                # evidence than a single noisy channel.
                raw_shift = float(np.median(np.abs(current - self.baseline)))
                present_now = self.score >= 2.5 and raw_shift >= 2.5
                if present_now:
                    self._present_hits += 1
                    self._empty_hits = 0
                    if self._present_hits >= 2:
                        self.status = "present"
                else:
                    self._empty_hits += 1
                    self._present_hits = 0
                    if self._empty_hits >= 8 and motion < max(2.0, motion_th * 1.5):
                        self.status = "empty"
        return self.snapshot()

    def snapshot(self):
        with self._lock:
            return {
                "calibrated": self.baseline is not None,
                "status": self.status,
                "motion": self.motion_status,
                "score": round(float(self.score), 2),
                "motion_score": round(float(self.motion_score), 2),
                "motion_level": round(float(self.motion_level), 3),
                "calibrated_at": self.calibrated_at or "",
                "error": self.last_error,
            }

    def impact_threshold(self):
        with self._lock:
            return max(5.0, self.delta_floor * 5.0) if self.calibrated else 7.0

    def quiet_threshold(self):
        with self._lock:
            return max(1.8, self.motion_floor * 2.8) if self.calibrated else 1.8


class FallEventDetector:
    """Conservative fall-candidate detector with a trained-classifier hook.

    A heuristic event requires an empty-room calibration, an identifiable
    impact-like change, preceding movement, presence, and a subsequent quiet
    period. If a user-trained model contains a `fall`/`falling` class, sustained
    model confidence can also raise an alert.
    """

    FALL_NAMES = {"fall", "falling", "fall_event", "fall_detected", "fall_detection"}

    def __init__(self):
        self.history = deque(maxlen=24)
        self.candidate = None
        self.alert = None
        self.fall_hits = 0
        self.last_impact = 0.0
        self.last_motion = 0.0
        self.last_model_probability = 0.0
        self.acknowledged = False

    @staticmethod
    def _fall_probability(pred):
        probs = (pred or {}).get("probs", {})
        vals = [float(v) for k, v in probs.items() if str(k).strip().lower() in FallEventDetector.FALL_NAMES]
        return max(vals, default=0.0)

    def update(self, amp, presence, pred, impact_threshold, quiet_threshold):
        x = np.asarray(amp, dtype=np.float32)
        if x.ndim != 2 or x.shape[1] != 64 or len(x) < 8:
            return self.snapshot()
        x = x[-min(len(x), 100):]
        motion = float(np.median(np.std(x, axis=0)))
        deltas = np.mean(np.abs(np.diff(x, axis=0)), axis=1)
        impact = float(np.max(deltas)) if len(deltas) else 0.0
        now = time.monotonic()
        self.last_impact, self.last_motion = impact, motion
        fall_p = self._fall_probability(pred)
        self.last_model_probability = fall_p
        self.fall_hits = self.fall_hits + 1 if fall_p >= 0.82 else 0

        was_moving = any(t > now - 3.0 and m >= max(2.0, quiet_threshold * 1.5)
                         for t, m, _ in self.history)
        present = presence.get("status") == "present"
        calibrated = bool(presence.get("calibrated"))

        if self.alert is None and self.fall_hits >= 2:
            self.alert = {"time": time.strftime("%H:%M:%S"), "method": "trained_model",
                          "confidence": round(fall_p, 3), "message": "نموذج السقوط المدرّب أعطى إشارة متكررة"}
            self.candidate = None
        elif (self.alert is None and calibrated and present and was_moving
              and impact >= impact_threshold and self.candidate is None):
            self.candidate = {"at": now, "time": time.strftime("%H:%M:%S"),
                              "impact": impact, "confirmed": False}

        if self.candidate and self.alert is None:
            age = now - self.candidate["at"]
            if age > 4.5:
                self.candidate = None
            elif age >= 1.5 and motion <= quiet_threshold and present:
                self.alert = {"time": self.candidate["time"], "method": "csi_heuristic",
                              "confidence": None,
                              "message": "تغيّر مفاجئ ثم سكون مع وجود محتمل؛ يلزم التحقق البشري"}
                self.candidate = None

        self.history.append((now, motion, impact))
        return self.snapshot()

    def acknowledge(self):
        self.alert = None
        self.candidate = None
        self.fall_hits = 0
        self.acknowledged = True

    def snapshot(self):
        if self.alert:
            state = "alert"
        elif self.candidate:
            state = "checking"
        else:
            state = "ready"
        return {
            "status": state,
            "event": dict(self.alert) if self.alert else None,
            "candidate": bool(self.candidate),
            "impact": round(float(self.last_impact), 2),
            "motion": round(float(self.last_motion), 3),
            "model_probability": round(float(self.last_model_probability), 3),
        }
