"""Pretend to be the ESP32-S3 so you can try the whole pipeline without hardware.
   python tools/fake_esp.py            # cycles through fake activities, UDP -> 127.0.0.1:5005
"""
import argparse
import os
import socket
import sys
import time

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from athena import proto  # noqa: E402

# (frequency Hz, amplitude) of the fake body motion per activity
ACTIVITIES = {"empty": (0.0, 0.3), "walking": (1.2, 5), "running": (2.6, 9), "jumping": (1.8, 12), "arm_wave": (3.5, 3.5)}


def frame(t, label, base, rng):
    f, a = ACTIVITIES[label]
    phase = np.linspace(0, 3, 64)
    motion = a * np.sin(2 * np.pi * f * t + phase) if f else 0
    return np.clip(base + motion + rng.normal(0, 0.8, 64), 0, 255)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=5005)
    ap.add_argument("--rate", type=float, default=90)
    ap.add_argument("--hold", type=float, default=25, help="seconds per activity")
    a = ap.parse_args()
    rng = np.random.default_rng(1)
    base = 60 + 25 * np.sin(np.linspace(0, 6, 64))
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    labels = list(ACTIVITIES)
    t0 = time.time()
    seq = 0
    print("fake board streaming; activity changes every", a.hold, "s. Ctrl+C to stop.")
    while True:
        t = time.time() - t0
        label = labels[int(t // a.hold) % len(labels)]
        if seq % int(a.rate * 5) == 0:
            print(f"  now pretending: {label}")
        if rng.random() > 0.03:                      # ~3% packet loss
            s.sendto(proto.build(seq, int(t * 1000), -50, frame(t, label, base, rng)), (a.host, a.port))
        seq += 1
        time.sleep(1.0 / a.rate * rng.uniform(0.7, 1.3))


if __name__ == "__main__":
    main()
