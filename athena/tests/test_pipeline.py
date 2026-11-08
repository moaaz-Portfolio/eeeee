"""End-to-end check without hardware or torch: fake board -> UDP -> record -> dataset."""
import json
import os
import shutil
import sys
import tempfile
import threading
import time
import urllib.request

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
from athena import app, data, proto  # noqa: E402
import fake_esp  # noqa: E402

UDP, HTTP = 5611, 8611
tmp = tempfile.mkdtemp()
app.DATA_DIR = os.path.join(tmp, "data")
app.MODEL_DIR = os.path.join(tmp, "models")
app.MODEL_PATH = os.path.join(app.MODEL_DIR, "activity_model.pt")


def test_proto():
    pkt = proto.build(7, 123456, -48, np.arange(64))
    assert len(pkt) == 14 + 64
    seq, ts, rssi, amp = proto.parse(pkt)
    assert (seq, ts, rssi) == (7, 123456, -48) and amp[5] == 5
    assert proto.parse(b"junk") is None and proto.parse(pkt[:20]) is None
    print("proto ok")


def test_resample():
    ts = np.cumsum(np.random.default_rng(0).uniform(8, 14, 500))
    amp = np.tile(np.sin(ts / 200)[:, None], (1, 64)).astype(np.float32) * 20 + 60
    out = data.resample(ts, amp, 50)
    assert out.shape[1] == 64 and abs(len(out) - (ts[-1] - ts[0]) / 20) < 2
    w = data.make_windows(out, 64, 16)
    assert w.shape[1:] == (64, 64)
    assert abs(data.normalize_window(w[0]).mean()) < 1e-3
    print("resample/windows ok", out.shape, w.shape)


def fake_board(stop):
    import socket
    rng = np.random.default_rng(3)
    base = 60 + 25 * np.sin(np.linspace(0, 6, 64))
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    t0, seq = time.time(), 0
    while not stop.is_set():
        t = time.time() - t0
        label = fake_board.label
        s.sendto(proto.build(seq, int(t * 1000), -50, fake_esp.frame(t, label, base, rng)), ("127.0.0.1", UDP))
        seq += 1
        time.sleep(0.011)
fake_board.label = "empty"


def call(path, body=None):
    req = urllib.request.Request(f"http://127.0.0.1:{HTTP}{path}", data=None if body is None else json.dumps(body).encode(),
                                 headers={"X-Athena": "1"} if body is not None else {})
    return json.loads(urllib.request.urlopen(req, timeout=5).read())


def test_app():
    st = app.State(UDP)
    st.rx.start()
    threading.Thread(target=st.loop, daemon=True).start()
    from http.server import ThreadingHTTPServer
    srv = ThreadingHTTPServer(("127.0.0.1", HTTP), app.make_handler(st))
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    stop = threading.Event()
    threading.Thread(target=fake_board, args=(stop,), daemon=True).start()
    time.sleep(2.5)

    s = call("/api/state")
    assert s["connected"] and s["fps"] > 40, s["fps"]
    assert len(s["heat"]) == 80 and len(s["heat"][0]) == 64
    print(f"live ok: {s['fps']} fps, loss {s['loss']}%")

    # CSRF guard
    try:
        urllib.request.urlopen(urllib.request.Request(f"http://127.0.0.1:{HTTP}/api/record", data=b"{}"))
        raise SystemExit("POST without header must fail")
    except urllib.error.HTTPError as e:
        assert e.code == 403
    page = urllib.request.urlopen(f"http://127.0.0.1:{HTTP}/").read()
    assert b"Athena" in page

    # record 3 takes of 2 activities (7 s each, shortened by patching the countdown)
    orig_sleep = time.sleep
    for label in ("empty", "walking"):
        fake_board.label = label
        for k in range(3):
            r = call("/api/record", {"label": label, "person": "p", "room": "r", "seconds": 5})
            assert r["ok"], r
            # The recording worker starts asynchronously. Wait until this request
            # has actually entered countdown/recording before accepting its result;
            # otherwise a previous take's "done" status can race this poll.
            deadline = time.monotonic() + 18
            saw_busy = False
            rec = None
            while time.monotonic() < deadline:
                rec = call("/api/state")["rec"]
                if rec["status"] in ("countdown", "recording"):
                    saw_busy = True
                elif saw_busy and rec["status"] in ("done", "error"):
                    break
                orig_sleep(0.2)
            assert saw_busy, f"recording worker never started: {rec}"
            assert rec["status"] == "done", rec
        print("recorded", label)
    st.rec["status"] = "idle"
    stop.set()

    tk = data.list_takes(app.DATA_DIR)
    assert {k: len(v) for k, v in tk.items()} == {"empty": 3, "walking": 3}
    labels, ds, counts = data.load_dataset(app.DATA_DIR, 50, 64, 16)
    for k, (X, y) in ds.items():
        assert X.shape[1:] == (64, 64) and len(X) == len(y) > 0
    print("dataset ok:", labels, counts, {k: v[0].shape for k, v in ds.items()})
    # the two fake activities must differ in window energy (sanity of the pipeline)
    X, y = ds["train"]
    e = [float(np.abs(X[y == i]).mean()) for i in range(2)]
    assert e[1] > e[0] * 2, e
    print("signal separation ok", [round(v, 2) for v in e])

    # too few takes -> clear error
    shutil.rmtree(os.path.join(app.DATA_DIR, "walking"))
    try:
        data.load_dataset(app.DATA_DIR)
    except data.DatasetError as ex:
        print("error message ok:", ex)
    print("summary", data.summary(app.DATA_DIR))


if __name__ == "__main__":
    test_proto()
    test_resample()
    test_app()
    shutil.rmtree(tmp, ignore_errors=True)
    print("ALL PASSED")
