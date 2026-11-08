"""Local web app: live view + guided recording + one-click training. Only needs numpy; torch for train/live."""
import json
import os
import threading
import time
import webbrowser
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import numpy as np

from . import data
from .live import Predictor, motion_energy
from .receiver import CsiReceiver
from .monitor import PresenceMonitor, FallEventDetector

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DATA_DIR = os.path.join(ROOT, "data")
MODEL_DIR = os.path.join(ROOT, "models")
MODEL_PATH = os.path.join(MODEL_DIR, "activity_model.pt")
PRETRAINED = os.path.join(ROOT, "pretrained", "activity_model_esp_fi_har.pt")
FS, WIN = 50, 64


class State:
    def __init__(self, port):
        self.rx = CsiReceiver(port)
        self.predictor = None
        self.model_error = ""
        self.pred = None
        self.energy_hist = deque(maxlen=120)
        self.presence = PresenceMonitor(os.path.join(MODEL_DIR, "room_baseline.json"))
        self.fall_detector = FallEventDetector()
        self.calibration = {"status": "idle", "left": 0, "message": ""}
        self._calibration_stop = threading.Event()
        # recording
        self.rec = {"status": "idle", "msg": "", "label": "", "left": 0, "frames": 0}
        self._rec_stop = threading.Event()
        # training
        self.train = {"status": "idle", "epoch": 0, "epochs": 0, "log": [], "metrics": None}
        self._train_stop = threading.Event()
        self.load_model()

    # ------------------------------------------------------------- model
    def load_model(self):
        self.predictor, self.model_error = None, ""
        if not os.path.exists(MODEL_PATH):
            return
        try:
            self.predictor = Predictor(MODEL_PATH)
        except ModuleNotFoundError:
            self.model_error = "torch is not installed (pip install torch)"
        except Exception as e:
            self.model_error = f"could not load model: {e}"

    def loop(self):
        while True:
            time.sleep(0.5)
            if not self.rx.connected():
                self.pred = None
                continue
            ts, amp = self.rx.recent(3.0)
            self.energy_hist.append(round(motion_energy(amp[-min(len(amp), 50):]), 3))
            if len(amp) >= 30:
                pstate = self.presence.update(ts, amp)
            else:
                pstate = self.presence.snapshot()
            if self.predictor:
                try:
                    p_ts, p_amp = self.rx.recent(self.predictor.seconds_needed + 0.5)
                    self.pred = self.predictor.predict(p_ts, p_amp)
                except Exception as e:
                    self.model_error = str(e)
            self.fall_detector.update(
                amp, pstate, self.pred,
                self.presence.impact_threshold(), self.presence.quiet_threshold())

    # ------------------------------------------------------- calibration
    def start_calibration(self, seconds=10):
        if not self.rx.connected():
            return "لا يوجد بث CSI من اللوحة"
        if self.calibration["status"] == "running":
            return "المعايرة شغالة بالفعل"
        if self.rec["status"] in ("countdown", "recording"):
            return "أوقف تسجيل النشاط قبل المعايرة"
        self._calibration_stop.clear()
        self.calibration.update(status="running", left=seconds,
                                message="اترك الغرفة فارغة وثبّت الراوتر واللوحة")
        threading.Thread(target=self._calibrate, args=(seconds,), daemon=True).start()
        return ""

    def _calibrate(self, seconds):
        # Capture exactly the calibration interval; don't include frames from
        # before the user emptied the room and pressed the calibration button.
        self.rx.start_recording()
        for left in range(seconds, 0, -1):
            self.calibration["left"] = left
            if self._calibration_stop.wait(1):
                self.rx.stop_recording()
                self.calibration.update(status="error", left=0, message="تم إلغاء المعايرة")
                return
            if not self.rx.connected():
                self.rx.stop_recording()
                self.calibration.update(status="error", left=0, message="انقطع بث CSI أثناء المعايرة")
                return
        result_data = self.rx.stop_recording()
        try:
            if result_data is None:
                raise ValueError("لم تصل بيانات CSI أثناء المعايرة")
            ts, _rssi, amp = result_data
            result = self.presence.calibrate(ts, amp)
            self.calibration.update(status="done", left=0,
                                    message=f"تمت المعايرة باستخدام {result['frames']} إطار")
        except Exception as exc:
            self.calibration.update(status="error", left=0, message=str(exc))


    # --------------------------------------------------------- recording
    def start_record(self, label, person, room, seconds):
        if self.calibration["status"] == "running":
            return "استنى لحد ما المعايرة تخلص"
        if self.rec["status"] not in ("idle", "done", "error"):
            return "already recording"
        if not self.rx.connected():
            return "no CSI stream - is the board in 'CSI stream' mode and on the same Wi-Fi?"
        if not data.safe_name(label) or not label.strip():
            return "choose a label"
        self._rec_stop.clear()
        threading.Thread(target=self._record, args=(label, person, room, seconds), daemon=True).start()
        return ""

    def _record(self, label, person, room, seconds):
        r = self.rec
        r.update(status="countdown", label=label, msg="", frames=0, left=3)
        for i in (3, 2, 1):
            r["left"] = i
            time.sleep(1)
            if self._rec_stop.is_set():
                r.update(status="idle", msg="cancelled")
                return
        self.rx.start_recording()
        t0 = time.time()
        r["status"] = "recording"
        while time.time() - t0 < seconds and not self._rec_stop.is_set():
            r["left"] = int(seconds - (time.time() - t0)) + 1
            r["frames"] = self.rx.recording_count()
            time.sleep(0.1)
        res = self.rx.stop_recording()
        if self._rec_stop.is_set():
            r.update(status="idle", msg="cancelled")
            return
        if res is None or len(res[0]) < seconds * 15:
            r.update(status="error", msg="too few frames received - move the board closer to the router")
            return
        ts, rssi, amp = res
        data.save_take(DATA_DIR, label, person, room, ts, rssi, amp)
        rate = len(ts) / max(seconds, 1)
        r.update(status="done", msg=f"saved {len(ts)} frames ({rate:.0f}/s)", frames=len(ts))

    def stop_record(self):
        self._rec_stop.set()

    # ----------------------------------------------------------- training
    def start_train(self, epochs, use_pretrained):
        if self.train["status"] == "running":
            return "already training"
        try:
            import torch  # noqa: F401
        except ModuleNotFoundError:
            return "torch is not installed. Run:  pip install torch"
        self._train_stop.clear()
        self.train.update(status="running", epoch=0, epochs=epochs, log=[], metrics=None)
        threading.Thread(target=self._train, args=(epochs, use_pretrained), daemon=True).start()
        return ""

    def _train(self, epochs, use_pretrained):
        from .train import train
        t = self.train
        try:
            m = train(DATA_DIR, MODEL_DIR, epochs=epochs, fs=FS, win=WIN,
                      init=PRETRAINED if use_pretrained else None,
                      log=lambda s: t["log"].append(s),
                      progress=lambda e, n: t.update(epoch=e, epochs=n),
                      should_stop=self._train_stop.is_set)
            t.update(status="done", metrics={"accuracy": m["test"]["accuracy"],
                                             "macro_f1": m["test"]["macro_f1"],
                                             "per_class": m["test"]["per_class"]})
            self.load_model()
        except data.DatasetError as e:
            t["log"].append(f"ERROR: {e}")
            t["status"] = "error"
        except Exception as e:
            t["log"].append(f"ERROR: {type(e).__name__}: {e}")
            t["status"] = "error"

    # ------------------------------------------------------------- state
    def snapshot(self):
        rx = self.rx
        last = rx.last_n(80)
        return {
            "connected": rx.connected(), "fps": round(rx.fps(), 1), "loss": round(rx.loss_pct(), 1),
            "rssi": rx.rssi, "port": rx.port, "error": rx.error,
            "energy": list(self.energy_hist),
            "heat": last.astype(int).tolist(),
            "pred": self.pred,
            "monitor": self.presence.snapshot(),
            "fall": self.fall_detector.snapshot(),
            "calibration": dict(self.calibration),
            "model_has_fall": bool(self.predictor and any(
                str(x).strip().lower() in FallEventDetector.FALL_NAMES for x in self.predictor.labels)),
            "model": {"loaded": self.predictor is not None, "error": self.model_error,
                      "version": self.predictor.version if self.predictor else "",
                      "labels": self.predictor.labels if self.predictor else [],
                      "threshold": self.predictor.threshold if self.predictor else 0.65,
                      "pretrained_available": os.path.exists(PRETRAINED)},
            "rec": dict(self.rec),
            "train": {k: (v[-40:] if k == "log" else v) for k, v in self.train.items()},
            "takes": data.summary(DATA_DIR),
        }


def make_handler(state):
    html_path = os.path.join(HERE, "static", "index.html")

    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def _json(self, obj, code=200):
            b = json.dumps(obj).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(b)))
            self.end_headers()
            self.wfile.write(b)

        def do_GET(self):
            if self.path == "/api/state":
                return self._json(state.snapshot())
            if self.path in ("/", "/index.html"):
                b = open(html_path, "rb").read()
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(b)))
                self.end_headers()
                self.wfile.write(b)
                return
            self.send_error(404)

        def do_POST(self):
            if self.headers.get("X-Athena") != "1":
                return self._json({"ok": False, "error": "forbidden"}, 403)
            n = int(self.headers.get("Content-Length") or 0)
            try:
                body = json.loads(self.rfile.read(n) or b"{}")
            except Exception:
                return self._json({"ok": False, "error": "bad json"}, 400)
            err = ""
            if self.path == "/api/record":
                err = state.start_record(str(body.get("label", "")), str(body.get("person", "")),
                                         str(body.get("room", "")), max(5, min(300, int(body.get("seconds", 20)))))
            elif self.path == "/api/record/stop":
                state.stop_record()
            elif self.path == "/api/train":
                err = state.start_train(max(1, min(200, int(body.get("epochs", 30)))), bool(body.get("pretrained")))
            elif self.path == "/api/train/stop":
                state._train_stop.set()
            elif self.path == "/api/label/delete":
                data.delete_label(DATA_DIR, str(body.get("label", "")))
            elif self.path == "/api/model/reload":
                state.load_model()
            elif self.path == "/api/calibrate":
                err = state.start_calibration(max(8, min(20, int(body.get("seconds", 10)))))
            elif self.path == "/api/calibrate/stop":
                state._calibration_stop.set()
            elif self.path == "/api/fall/ack":
                state.fall_detector.acknowledge()
            else:
                return self.send_error(404)
            self._json({"ok": not err, "error": err})

    return H


def run(port_udp=5005, port_http=8000, open_browser=True):
    os.makedirs(DATA_DIR, exist_ok=True)
    os.makedirs(MODEL_DIR, exist_ok=True)
    st = State(port_udp)
    st.rx.start()
    threading.Thread(target=st.loop, daemon=True).start()
    srv = ThreadingHTTPServer(("127.0.0.1", port_http), make_handler(st))
    url = f"http://127.0.0.1:{port_http}"
    print(f"Athena is running:  {url}\n(listening for the ESP32-S3 on UDP {port_udp})   Ctrl+C to quit")
    if open_browser:
        threading.Timer(0.8, lambda: webbrowser.open(url)).start()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
