"""Synthetic tests for empty-room calibration and integrated event logic."""
import os
import sys
import tempfile
import time

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from athena.monitor import PresenceMonitor, FallEventDetector  # noqa: E402


def test_presence_calibration_and_hysteresis():
    rng = np.random.default_rng(8)
    base = 70 + 15 * np.sin(np.linspace(0, 4, 64))
    empty = base + rng.normal(0, 0.5, (500, 64))
    with tempfile.TemporaryDirectory() as d:
        m = PresenceMonitor(os.path.join(d, "baseline.json"))
        m.calibrate(np.arange(len(empty)), empty)
        assert m.calibrated
        for _ in range(10):
            state = m.update(np.arange(180), empty[-180:])
        assert state["status"] == "empty", state
        occupied = empty[-180:] + np.linspace(5, 9, 64)[None, :]
        for _ in range(3):
            state = m.update(np.arange(180), occupied)
        assert state["status"] == "present", state
        loaded = PresenceMonitor(os.path.join(d, "baseline.json"))
        assert loaded.calibrated
    print("presence calibration/hysteresis ok")


def test_fall_candidate_requires_impact_then_quiet_presence():
    rng = np.random.default_rng(4)
    base = 70 + 15 * np.sin(np.linspace(0, 4, 64))
    detector = FallEventDetector()
    p = {"status": "present", "calibrated": True}
    # Establish movement history before the abrupt change.
    for i in range(8):
        t = np.arange(100) / 50
        moving = base + 7 * np.sin(2 * np.pi * 1.2 * t[:, None] + np.linspace(0, 2, 64)[None, :])
        detector.update(moving, p, None, 5.0, 1.8)
        time.sleep(0.002)
    # A synthetic abrupt CSI change creates an impact-like peak.
    impact = np.tile(base, (100, 1)).astype(np.float32)
    impact[55] += 45
    detector.update(impact, p, None, 5.0, 1.8)
    assert detector.snapshot()["candidate"]
    quiet = np.tile(base, (100, 1)).astype(np.float32)
    time.sleep(1.55)
    result = detector.update(quiet, p, None, 5.0, 1.8)
    assert result["status"] == "alert", result
    detector.acknowledge()
    assert detector.snapshot()["status"] == "ready"
    print("fall candidate/acknowledge ok")


if __name__ == "__main__":
    test_presence_calibration_and_hysteresis()
    test_fall_candidate_requires_impact_then_quiet_presence()
    print("ALL MONITOR TESTS PASSED")
