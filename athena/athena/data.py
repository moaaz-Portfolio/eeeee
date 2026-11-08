"""Recordings on disk -> windows ready for training / live inference."""
import json
import os
import random
import re
import time

import numpy as np

FEATURES = 64


# --------------------------------------------------------------- saving
def safe_name(s):
    return re.sub(r"[^a-zA-Z0-9_-]+", "_", s.strip()).strip("_") or "x"


def save_take(root, label, person, room, ts, rssi, amp):
    label = safe_name(label)
    d = os.path.join(root, label)
    os.makedirs(d, exist_ok=True)
    stem = f"{time.strftime('%Y%m%d-%H%M%S')}_{safe_name(person)}_{safe_name(room)}"
    path, k = os.path.join(d, stem + ".npz"), 1
    while os.path.exists(path):                      # two takes in the same second must not overwrite
        k += 1
        path = os.path.join(d, f"{stem}-{k}.npz")
    np.savez_compressed(path, ts=ts, rssi=rssi, amp=amp, label=label,
                        person=person or "", room=room or "")
    return path


def list_takes(root):
    """{label: [paths]}"""
    out = {}
    if not os.path.isdir(root):
        return out
    for lab in sorted(os.listdir(root)):
        d = os.path.join(root, lab)
        if os.path.isdir(d):
            files = sorted(f for f in os.listdir(d) if f.endswith(".npz"))
            if files:
                out[lab] = [os.path.join(d, f) for f in files]
    return out


# ----------------------------------------------------------- processing
def resample(ts_ms, amp, fs):
    """Irregular router replies -> a steady `fs` Hz grid (linear interpolation)."""
    ts = np.asarray(ts_ms, dtype=np.float64)
    if len(ts) < 2:
        return np.zeros((0, amp.shape[1] if amp.ndim == 2 else FEATURES), np.float32)
    order = np.argsort(ts)
    ts, amp = ts[order], np.asarray(amp, dtype=np.float32)[order]
    keep = np.concatenate([[True], np.diff(ts) > 0])      # strictly increasing
    ts, amp = ts[keep], amp[keep]
    if len(ts) < 2:
        return np.zeros((0, amp.shape[1]), np.float32)
    grid = np.arange(ts[0], ts[-1], 1000.0 / fs)
    out = np.empty((len(grid), amp.shape[1]), np.float32)
    for k in range(amp.shape[1]):
        out[:, k] = np.interp(grid, ts, amp[:, k])
    return out


def normalize_window(w):
    """Remove the static room signature (per-subcarrier mean of the window).
    What is left is how the signal MOVES, which transfers better between rooms."""
    return w - w.mean(axis=0, keepdims=True)


def make_windows(series, win, hop):
    n = (len(series) - win) // hop + 1
    if n <= 0:
        return np.zeros((0, win, series.shape[1]), np.float32)
    return np.stack([series[i * hop:i * hop + win] for i in range(n)]).astype(np.float32)


# -------------------------------------------------------------- dataset
class DatasetError(Exception):
    pass


def split_takes(takes, seed=7):
    """Split BY RECORDING (never inside one), per label: ~70/15/15."""
    rng = random.Random(seed)
    split = {"train": [], "val": [], "test": []}
    for lab, files in takes.items():
        if len(files) < 3:
            raise DatasetError(
                f"'{lab}' has only {len(files)} recording(s). Record at least 3 per activity "
                "(10+ is much better), ideally on different days / positions.")
        f = files[:]
        rng.shuffle(f)
        n_test = max(1, round(len(f) * 0.15))
        n_val = max(1, round(len(f) * 0.15))
        split["test"] += [(p, lab) for p in f[:n_test]]
        split["val"] += [(p, lab) for p in f[n_test:n_test + n_val]]
        split["train"] += [(p, lab) for p in f[n_test + n_val:]]
    return split


def build_split(files, labels, fs, win, hop, normalize=True):
    """normalize=False is only for the ablation study (does removing the room signature help?)."""
    if not files:
        raise DatasetError("this split has no recordings")
    X, y = [], []
    for path, lab in files:
        z = np.load(path, allow_pickle=False)
        series = resample(z["ts"], z["amp"], fs)
        w = make_windows(series, win, hop)
        if len(w):
            X.append(np.stack([normalize_window(x) if normalize else x for x in w]))
            y += [labels.index(lab)] * len(w)
    if not X:
        raise DatasetError("recordings are too short for one window")
    return np.concatenate(X), np.array(y, dtype=np.int64)


def load_dataset(root, fs=50, win=64, hop=16, normalize=True):
    takes = list_takes(root)
    if len(takes) < 2:
        raise DatasetError("Record at least 2 different activities first.")
    labels = sorted(takes)
    split = split_takes(takes)
    out = {k: build_split(v, labels, fs, win, hop, normalize) for k, v in split.items()}
    return labels, out, {k: len(v) for k, v in split.items()}


# ------------------------------------------- cross-room / cross-person split
GROUPS = ("room", "person")


def take_meta(path, label):
    """Label comes from the folder; room/person from the recording itself (lower-cased so 'Room1' == 'room1')."""
    z = np.load(path, allow_pickle=False)
    clean = lambda k: (str(z[k]).strip().lower() if k in z.files else "") or "unknown"
    return {"path": path, "label": label, "room": clean("room"), "person": clean("person")}


def list_takes_meta(root):
    return [take_meta(p, lab) for lab, files in list_takes(root).items() for p in files]


def group_values(metas, group):
    if group not in GROUPS:
        raise DatasetError(f"group must be one of {GROUPS}")
    return sorted({m[group] for m in metas})


def split_leave_one_group_out(metas, group, held, seed=7):
    """Test = EVERY recording whose room (or person) is `held`. The remaining recordings are split
    into train / validation BY RECORDING (about 85/15, per label). Same return format as split_takes."""
    test = [(m["path"], m["label"]) for m in metas if m[group] == held]
    if not test:
        raise DatasetError(f"no recordings with {group} = '{held}'")
    rng = random.Random(seed)
    by_label = {}
    for m in metas:
        if m[group] != held:
            by_label.setdefault(m["label"], []).append(m["path"])
    train, val = [], []
    for lab, files in sorted(by_label.items()):
        files = sorted(files)
        rng.shuffle(files)
        n_val = max(1, round(len(files) * 0.15)) if len(files) >= 2 else 0
        val += [(p, lab) for p in files[:n_val]]
        train += [(p, lab) for p in files[n_val:]]
    return {"train": train, "val": val, "test": test}


def load_fold(metas, group, held, labels, fs=50, win=64, hop=16, normalize=True, seed=7):
    split = split_leave_one_group_out(metas, group, held, seed)
    out = {k: build_split(v, labels, fs, win, hop, normalize) for k, v in split.items()}
    return out, {k: len(v) for k, v in split.items()}


def summary(root):
    """For the UI: takes per label and total seconds."""
    res = {}
    for lab, files in list_takes(root).items():
        secs = 0.0
        for p in files:
            try:
                z = np.load(p, allow_pickle=False)
                t = z["ts"].astype(np.float64)
                if len(t) > 1:
                    secs += (t[-1] - t[0]) / 1000.0
            except Exception:
                pass
        res[lab] = {"takes": len(files), "seconds": round(secs)}
    return res


def delete_label(root, label):
    import shutil
    d = os.path.join(root, safe_name(label))
    if os.path.isdir(d):
        shutil.rmtree(d)


def write_json(path, obj):
    with open(path, "w", encoding="utf-8") as f:
        json.dump(obj, f, indent=2)
