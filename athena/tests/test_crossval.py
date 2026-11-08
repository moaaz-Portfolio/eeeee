"""Cross-room evaluation checks. Numpy only: no board, no torch.   python tests/test_crossval.py"""
import os
import shutil
import sys
import tempfile

import numpy as np
import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
from athena import crossval, data  # noqa: E402
import fake_esp  # noqa: E402

ROOMS = {"bedroom": 0, "living": 1, "kitchen": 2}      # each room gets its own static signature
PERSONS = ("ali", "sara")
LABELS = ("empty", "walking", "running")


def make_dataset(root, takes=3, seconds=6, rate=90):
    rng = np.random.default_rng(5)
    for room, ri in ROOMS.items():
        base = 55 + 10 * ri + (20 + 5 * ri) * np.sin(np.linspace(0, 5 + ri, 64))
        for person in PERSONS:
            for label in LABELS:
                for _ in range(takes):
                    n = seconds * rate
                    ts = np.cumsum(rng.uniform(8, 14, n))
                    amp = np.stack([fake_esp.frame(t / 1000.0, label, base, rng) for t in ts]).astype(np.uint8)
                    data.save_take(root, label, person, room, ts.astype(np.uint32),
                                   np.full(n, -50, np.int8), amp)


def test_save_take_never_overwrites(tmp_path):
    root = os.path.join(str(tmp_path), "collide")
    ts, rssi, amp = np.arange(10), np.zeros(10, np.int8), np.zeros((10, 64), np.uint8)
    paths = {data.save_take(root, "x", "p", "r", ts, rssi, amp) for _ in range(5)}
    assert len(paths) == 5 and all(os.path.exists(p) for p in paths)
    print("save_take collision-safe ok")


@pytest.fixture
def crossval_dataset(tmp_path):
    root = str(tmp_path / "data")
    make_dataset(root)
    return root


def test_splits_have_no_leakage(crossval_dataset):
    root = crossval_dataset
    metas = data.list_takes_meta(root)
    assert len(metas) == len(ROOMS) * len(PERSONS) * len(LABELS) * 3
    for group, values in (("room", ROOMS), ("person", PERSONS)):
        assert data.group_values(metas, group) == sorted(values)
        for held in values:
            sp = data.split_leave_one_group_out(metas, group, held)
            by_path = {m["path"]: m for m in metas}
            assert sp["test"] and sp["train"] and sp["val"]
            assert all(by_path[p][group] == held for p, _ in sp["test"])
            for part in ("train", "val"):
                assert all(by_path[p][group] != held for p, _ in sp[part]), f"{part} leaks {held}"
            seen = [p for part in sp.values() for p, _ in part]
            assert len(seen) == len(set(seen)) == len(metas), "a recording is in two splits or missing"
    print("no leakage ok (room + person)")


def test_run_baseline(crossval_dataset):
    root = crossval_dataset
    res = crossval.run(root, "room", baseline_only=True, log=lambda *_: None)
    assert [f["held_out"] for f in res["folds"]] == sorted(ROOMS)
    acc = res["mean"]["baseline"]["accuracy"]
    assert acc > 0.8, acc                       # empty / walking / running differ clearly in energy
    pres = res["mean"]["baseline"]
    assert pres["false_alarm_rate"] < 0.2 and pres["miss_rate"] < 0.2, pres
    res_p = crossval.run(root, "person", baseline_only=True, log=lambda *_: None)
    assert len(res_p["folds"]) == len(PERSONS)
    res_raw = crossval.run(root, "room", normalize=False, baseline_only=True, log=lambda *_: None)
    assert abs(res_raw["mean"]["baseline"]["accuracy"] - acc) < 1e-9     # baseline ignores the room signature
    out = tempfile.mkdtemp()
    j, c = crossval.save(res, out)
    assert os.path.getsize(j) > 0 and open(c, encoding="utf-8").read().count("\n") == len(ROOMS) + 1
    shutil.rmtree(out)
    print(f"baseline ok: acc {acc:.3f}, false-alarm {pres['false_alarm_rate']:.3f}, miss {pres['miss_rate']:.3f}")


def test_presence_rates():
    labels = ["empty", "sitting", "walking"]
    cm = np.array([[90, 8, 2],       # 10 of 100 empty windows raised an alarm
                   [20, 70, 10],     # 20 of 100 sitting windows were missed
                   [0, 10, 90]])
    r = crossval.presence_rates(cm, labels)
    assert abs(r["false_alarm_rate"] - 0.10) < 1e-9
    assert abs(r["miss_rate"] - 20 / 200) < 1e-9
    assert crossval.presence_rates(cm, ["a", "b", "c"]) is None
    print("presence rates ok")


def test_needs_two_groups(tmp_path):
    root = os.path.join(str(tmp_path), "one_room")
    ts, rssi = np.arange(400) * 11, np.zeros(400, np.int8)
    amp = np.random.default_rng(0).integers(40, 80, (400, 64)).astype(np.uint8)
    for lab in ("empty", "walking"):
        for _ in range(3):
            data.save_take(root, lab, "ali", "only_room", ts, rssi, amp)
    try:
        crossval.run(root, "room", baseline_only=True, log=lambda *_: None)
        raise SystemExit("one room must be rejected")
    except data.DatasetError as e:
        print("one-room error ok:", e)


if __name__ == "__main__":
    tmp = tempfile.mkdtemp()
    try:
        root = os.path.join(tmp, "data")
        make_dataset(root)
        test_save_take_never_overwrites(tmp)
        test_splits_have_no_leakage(root)
        test_run_baseline(root)
        test_presence_rates()
        test_needs_two_groups(tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("ALL CROSSVAL TESTS PASSED")
