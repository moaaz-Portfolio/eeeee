"""Does the system still work in a room (or for a person) it has NEVER seen?

For every room (or person) found in your recordings, this trains on all the OTHER rooms and tests on
the held-out one ("leave-one-room-out"). It also runs a trivial no-AI baseline so you can show how much
the neural network really adds, and it can repeat everything without `normalize_window` (ablation).

    python -m athena.crossval --group room                 # needs torch for the neural network
    python -m athena.crossval --group room --baseline-only # numpy only, runs in seconds
    python -m athena.crossval --group room --ablate        # with vs without removing the room signature
    python -m athena.crossval --group person               # leave-one-person-out

Rooms/persons come from the names typed when recording, so use the SAME spelling every time.
"""
import argparse
import csv
import importlib.util
import json
import os
import time

import numpy as np

from . import data
from .train import confusion, scores

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DEFAULT_DATA = os.path.join(ROOT, "data")
DEFAULT_OUT = os.path.join(ROOT, "results")


# ---------------------------------------------------------------- baseline (no neural network)
def window_features(X):
    """Two numbers per window: how much the signal moves, and how fast it changes (log scale).
    Both ignore the static room signature by construction."""
    spread = X.std(axis=1).mean(axis=1)
    speed = np.abs(np.diff(X, axis=1)).mean(axis=(1, 2))
    return np.log(np.stack([spread, speed], axis=1) + 1e-6)


class CentroidBaseline:
    """Nearest class centre on the two numbers above. If the neural network cannot beat this, say so."""

    def fit(self, X, y, n_cls):
        f = window_features(X)
        self.mu, self.sd = f.mean(0), f.std(0) + 1e-6
        z = (f - self.mu) / self.sd
        self.c = np.stack([z[y == k].mean(0) if (y == k).any() else np.full(z.shape[1], np.inf)
                           for k in range(n_cls)])
        return self

    def predict(self, X):
        z = (window_features(X) - self.mu) / self.sd
        return ((z[:, None, :] - self.c[None]) ** 2).sum(2).argmin(1)


# ------------------------------------------------------------------------------- metrics
def presence_rates(cm, labels, empty="empty"):
    """Presence = anything that is not the 'empty' class.
    false_alarm_rate: empty room reported as occupied.  miss_rate: occupied room reported as empty."""
    if empty not in labels:
        return None
    i = labels.index(empty)
    cm = np.asarray(cm)
    n_empty = cm[i].sum()
    n_occ = cm.sum() - n_empty
    return {"false_alarm_rate": float((n_empty - cm[i, i]) / max(n_empty, 1)),
            "miss_rate": float((cm[:, i].sum() - cm[i, i]) / max(n_occ, 1))}


def summarize(cm, labels, test_labels):
    """Accuracy and macro-F1 over the classes that actually appear in the held-out data."""
    pr, rc, f1, acc = scores(cm)
    present = [labels.index(l) for l in test_labels]
    return {"accuracy": float(acc), "macro_f1": float(np.mean(f1[present])),
            "per_class_recall": {labels[k]: float(rc[k]) for k in present},
            "presence": presence_rates(cm, labels), "confusion_matrix": cm.tolist()}


# ----------------------------------------------------------------------------------- run
def run(root=DEFAULT_DATA, group="room", epochs=20, normalize=True, baseline_only=False,
        fs=50, win=64, hop=16, seed=7, log=print):
    metas = data.list_takes_meta(root)
    if not metas:
        raise data.DatasetError(f"no recordings found in {root}")
    values = data.group_values(metas, group)
    if len(values) < 2:
        raise data.DatasetError(f"found only {len(values)} {group}(s): {values}. "
                                f"Record in at least 2 different {group}s (3+ is much better).")
    labels = sorted({m["label"] for m in metas})
    folds = []
    for held in values:
        try:
            ds, counts = data.load_fold(metas, group, held, labels, fs, win, hop, normalize, seed)
        except data.DatasetError as e:
            log(f"[{group}={held}] skipped: {e}")
            continue
        (Xtr, ytr), _, (Xte, yte) = ds["train"], ds["val"], ds["test"]
        test_labels = [labels[k] for k in sorted(set(yte.tolist()))]
        unseen = [l for l in test_labels if labels.index(l) not in set(ytr.tolist())]
        res = {"held_out": held, "recordings": counts, "windows_test": int(len(yte)),
               "test_labels": test_labels, "labels_missing_from_training": unseen}
        base = CentroidBaseline().fit(Xtr, ytr, len(labels))
        res["baseline"] = summarize(confusion(yte, base.predict(Xte), len(labels)), labels, test_labels)
        if not baseline_only:
            from .train import train
            m = train(root, None, epochs=epochs, fs=fs, win=win, hop=hop, log=lambda *_: None,
                      dataset=(labels, ds, counts), normalize=normalize, save=False)
            res["model"] = summarize(np.array(m["test"]["confusion_matrix"]), labels, test_labels)
        folds.append(res)
        log(_fold_line(group, res))
    if not folds:
        raise data.DatasetError("no usable fold")
    return {"group": group, "normalize": normalize, "epochs": None if baseline_only else epochs,
            "labels": labels, "seed": seed, "created": time.strftime("%Y-%m-%d %H:%M:%S"),
            "folds": folds, "mean": _mean(folds)}


def _mean(folds):
    out = {}
    for who in ("baseline", "model"):
        if all(who in f for f in folds):
            out[who] = {k: float(np.mean([f[who][k] for f in folds])) for k in ("accuracy", "macro_f1")}
            std = float(np.std([f[who]["accuracy"] for f in folds]))
            out[who]["accuracy_std_between_folds"] = std
            pr = [f[who]["presence"] for f in folds if f[who]["presence"]]
            if pr:
                out[who]["false_alarm_rate"] = float(np.mean([p["false_alarm_rate"] for p in pr]))
                out[who]["miss_rate"] = float(np.mean([p["miss_rate"] for p in pr]))
    return out


def _fold_line(group, r):
    parts = [f"[{group}={r['held_out']}]"]
    for who in ("baseline", "model"):
        if who in r:
            parts.append(f"{who}: acc {r[who]['accuracy']:.3f}  F1 {r[who]['macro_f1']:.3f}")
    if r["labels_missing_from_training"]:
        parts.append(f"(never seen in training: {r['labels_missing_from_training']})")
    return "  ".join(parts)


def save(result, out_dir=DEFAULT_OUT):
    os.makedirs(out_dir, exist_ok=True)
    tag = f"{result['group']}_{'norm' if result['normalize'] else 'raw'}_{time.strftime('%Y%m%d-%H%M%S')}"
    jpath = os.path.join(out_dir, f"crossval_{tag}.json")
    data.write_json(jpath, result)
    cpath = os.path.join(out_dir, f"crossval_{tag}.csv")
    with open(cpath, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["held_out", "method", "accuracy", "macro_f1", "false_alarm_rate", "miss_rate"])
        for r in result["folds"]:
            for who in ("baseline", "model"):
                if who in r:
                    p = r[who]["presence"] or {}
                    w.writerow([r["held_out"], who, round(r[who]["accuracy"], 4), round(r[who]["macro_f1"], 4),
                                round(p.get("false_alarm_rate", float("nan")), 4),
                                round(p.get("miss_rate", float("nan")), 4)])
    return jpath, cpath


def main():
    ap = argparse.ArgumentParser(description="Leave-one-room-out / leave-one-person-out evaluation")
    ap.add_argument("--data", default=DEFAULT_DATA)
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--group", choices=data.GROUPS, default="room")
    ap.add_argument("--epochs", type=int, default=20)
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--baseline-only", action="store_true", help="no torch needed")
    ap.add_argument("--no-normalize", action="store_true", help="keep the room signature (ablation)")
    ap.add_argument("--ablate", action="store_true", help="run with AND without normalize_window")
    a = ap.parse_args()

    baseline_only = a.baseline_only
    if not baseline_only and importlib.util.find_spec("torch") is None:
        print("torch is not installed -> running the baseline only (pip install torch for the neural network)")
        baseline_only = True
    modes = [True, False] if a.ablate else [not a.no_normalize]
    for norm in modes:
        print(f"\n=== leave-one-{a.group}-out | normalize_window={'ON' if norm else 'OFF'} ===")
        res = run(a.data, a.group, a.epochs, norm, baseline_only, seed=a.seed)
        for who, m in res["mean"].items():
            fa = f"  false-alarm {m['false_alarm_rate']:.3f}  miss {m['miss_rate']:.3f}" if "false_alarm_rate" in m else ""
            print(f"MEAN {who:8s} acc {m['accuracy']:.3f} (+-{m['accuracy_std_between_folds']:.3f} between folds)  "
                  f"F1 {m['macro_f1']:.3f}{fa}")
        j, c = save(res, a.out)
        print("saved:", j, "and", c)
    print("\nRemember: a result from 2-3 rooms and 1-2 people is a pilot, not a proof. Say so in the report.")


if __name__ == "__main__":
    main()
