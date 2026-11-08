"""Run a trained model on the live stream."""
from collections import deque

import numpy as np

from .data import normalize_window, resample


class Predictor:
    def __init__(self, path):
        import torch
        from .model import ActivityNet
        ck = torch.load(path, map_location="cpu", weights_only=True)
        self.torch = torch
        self.labels = ck["labels"]
        self.win = int(ck["window_length"])
        self.fs = float(ck["fs"])
        self.scale = float(ck["scale"])
        self.threshold = float(ck.get("confidence_threshold", 0.65))
        self.version = ck.get("version", "?")
        self.normalize = bool(ck.get("normalize", True))   # older checkpoints always normalised
        self.model = ActivityNet(int(ck["feature_count"]), len(self.labels))
        self.model.load_state_dict(ck["state_dict"])
        self.model.eval()
        self._hist = deque(maxlen=3)            # smooth over ~1.5 s

    @property
    def seconds_needed(self):
        return self.win / self.fs + 1.0

    def predict(self, ts, amp):
        series = resample(ts, amp, self.fs)
        if len(series) < self.win:
            return None
        w = series[-self.win:]
        x = (normalize_window(w) if self.normalize else w) / self.scale
        with self.torch.no_grad():
            p = self.torch.softmax(self.model(self.torch.from_numpy(x[None]).float()), dim=1)[0].numpy()
        self._hist.append(p)
        avg = np.mean(self._hist, axis=0)
        i = int(avg.argmax())
        conf = float(avg[i])
        return {"label": self.labels[i] if conf >= self.threshold else "uncertain",
                "best": self.labels[i], "confidence": conf,
                "probs": {l: float(v) for l, v in zip(self.labels, avg)}}


def motion_energy(amp):
    """Cheap, model-free 'how much is the signal moving' number for the last second."""
    if len(amp) < 5:
        return 0.0
    return float(np.mean(np.std(amp, axis=0)))
