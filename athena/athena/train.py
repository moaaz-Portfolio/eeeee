"""Train on your own recordings."""
import copy
import os
import time

import numpy as np

from .data import FEATURES, load_dataset, write_json

CONF_THRESHOLD = 0.65


def confusion(y, p, n):
    m = np.zeros((n, n), dtype=np.int64)
    for a, b in zip(y, p):
        m[a, b] += 1
    return m


def scores(m):
    tp = np.diag(m).astype(float)
    prec = tp / np.maximum(m.sum(0), 1)
    rec = tp / np.maximum(m.sum(1), 1)
    f1 = 2 * prec * rec / np.maximum(prec + rec, 1e-9)
    return prec, rec, f1, float(tp.sum() / max(m.sum(), 1))


def train(root, out_dir, epochs=30, fs=50, win=64, hop=16, init=None,
          log=print, progress=None, should_stop=lambda: False,
          dataset=None, normalize=True, save=True):
    """dataset=(labels, {train,val,test: (X, y)}, counts) lets crossval.py supply its own split;
    save=False evaluates without writing any file."""
    import torch
    import torch.nn as nn
    from .model import ActivityNet, load_pretrained

    labels, data, counts = dataset if dataset is not None else load_dataset(root, fs, win, hop, normalize)
    (Xtr, ytr), (Xva, yva), (Xte, yte) = data["train"], data["val"], data["test"]
    log(f"recordings train/val/test: {counts['train']}/{counts['val']}/{counts['test']}  "
        f"windows: {len(Xtr)}/{len(Xva)}/{len(Xte)}  classes: {labels}")

    scale = float(Xtr.std()) + 1e-6          # one global number, keeps relative motion strength
    dev = "cuda" if torch.cuda.is_available() else "cpu"
    T = lambda a: torch.from_numpy(a / scale if a.dtype == np.float32 else a)
    Xtr_t, Xva_t, Xte_t = T(Xtr).to(dev), T(Xva).to(dev), T(Xte).to(dev)
    ytr_t = torch.from_numpy(ytr).to(dev)

    model = ActivityNet(FEATURES, len(labels)).to(dev)
    if init and os.path.exists(init):
        log(f"initialised {load_pretrained(model, init)} tensors from the pretrained model")

    cnt = np.bincount(ytr, minlength=len(labels)).astype(float)
    w = torch.tensor(cnt.sum() / np.maximum(cnt, 1) / len(labels), dtype=torch.float32, device=dev)
    lossf = nn.CrossEntropyLoss(weight=w, label_smoothing=0.05)
    opt = torch.optim.AdamW(model.parameters(), lr=1e-3, weight_decay=1e-4)
    sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, T_max=max(epochs, 1))

    def predict(X):
        model.eval()
        out = []
        with torch.no_grad():
            for i in range(0, len(X), 512):
                out.append(model(X[i:i + 512]).argmax(1).cpu().numpy())
        return np.concatenate(out)

    best, best_f1, bs = None, -1.0, 128
    for ep in range(1, epochs + 1):
        if should_stop():
            log("stopped by user")
            break
        model.train()
        perm = torch.randperm(len(Xtr_t), device=dev)
        tot = 0.0
        for i in range(0, len(perm), bs):
            idx = perm[i:i + bs]
            x, y = Xtr_t[idx], ytr_t[idx]
            # augmentation: gain, noise, small time shift
            x = x * (0.8 + 0.45 * torch.rand(len(x), 1, 1, device=dev))
            x = x + 0.05 * torch.randn_like(x)
            x = torch.roll(x, int(np.random.randint(-4, 5)), dims=1)
            opt.zero_grad()
            loss = lossf(model(x), y)
            loss.backward()
            nn.utils.clip_grad_norm_(model.parameters(), 2.0)
            opt.step()
            tot += loss.item() * len(idx)
        sched.step()
        m = confusion(yva, predict(Xva_t), len(labels))
        _, _, f1, acc = scores(m)
        mf1 = float(f1.mean())
        if mf1 > best_f1:
            best_f1, best = mf1, copy.deepcopy({k: v.cpu() for k, v in model.state_dict().items()})
        log(f"epoch {ep:>2}/{epochs}  loss {tot / len(perm):.3f}  val acc {acc:.3f}  macro-F1 {mf1:.3f}")
        if progress:
            progress(ep, epochs)

    if best is None:
        raise RuntimeError("training did not run")
    model.load_state_dict(best)
    mt = confusion(yte, predict(Xte_t), len(labels))
    pr, rc, f1, acc = scores(mt)

    ckpt = {
        "version": "athena-own-1",
        "state_dict": {k: v.cpu() for k, v in model.state_dict().items()},
        "labels": labels, "feature_count": FEATURES, "window_length": win,
        "fs": fs, "scale": scale, "confidence_threshold": CONF_THRESHOLD, "normalize": normalize,
    }

    metrics = {
        "model_version": ckpt["version"], "normalize": normalize, "labels": labels, "fs": fs, "window_length": win,
        "recordings": counts, "windows": {"train": len(Xtr), "val": len(Xva), "test": len(Xte)},
        "best_val_macro_f1": best_f1,
        "test": {"accuracy": acc, "macro_f1": float(f1.mean()),
                 "per_class": {l: {"precision": float(pr[i]), "recall": float(rc[i]), "f1": float(f1[i])}
                               for i, l in enumerate(labels)},
                 "confusion_matrix": mt.tolist()},
        "trained_at": time.strftime("%Y-%m-%d %H:%M:%S"),
        "warning": "Test recordings come from the same room/person as training unless you "
                   "recorded in several rooms. Re-test in a new room before trusting it.",
    }
    if save:
        os.makedirs(out_dir, exist_ok=True)
        path = os.path.join(out_dir, "activity_model.pt")
        torch.save(ckpt, path)
        write_json(os.path.join(out_dir, "metrics.json"), metrics)
        log(f"TEST accuracy {acc:.3f}  macro-F1 {float(f1.mean()):.3f}  -> saved {path}")
    else:
        log(f"TEST accuracy {acc:.3f}  macro-F1 {float(f1.mean()):.3f}")
    return metrics
