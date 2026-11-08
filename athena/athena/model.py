"""CNN + BiLSTM + attention. Layer names/shapes match the pretrained 'athena-cnn-bilstm' file,
so its weights can optionally be used as a starting point."""
import torch
import torch.nn as nn


class ActivityNet(nn.Module):
    def __init__(self, n_feat=64, n_cls=7, drop=0.3):
        super().__init__()
        self.encoder = nn.Sequential(
            nn.Conv1d(n_feat, 64, 5, padding=2), nn.BatchNorm1d(64), nn.ReLU(), nn.Dropout(drop * 0.3),
            nn.Conv1d(64, 96, 5, padding=2), nn.BatchNorm1d(96), nn.ReLU(), nn.Dropout(drop * 0.3),
        )
        self.temporal = nn.LSTM(96, 64, batch_first=True, bidirectional=True)
        self.attention = nn.Sequential(nn.Linear(128, 32), nn.Tanh(), nn.Linear(32, 1))
        self.head = nn.Sequential(nn.Linear(128, 64), nn.ReLU(), nn.Dropout(drop), nn.Linear(64, n_cls))

    def forward(self, x):                       # x: (batch, time, features)
        h = self.encoder(x.transpose(1, 2)).transpose(1, 2)
        h, _ = self.temporal(h)
        a = torch.softmax(self.attention(h), dim=1)
        return self.head((a * h).sum(dim=1))


def load_pretrained(model, path):
    """Copy every layer whose shape matches (the class head is skipped). Returns #tensors copied."""
    ck = torch.load(path, map_location="cpu", weights_only=True)
    src = ck["state_dict"] if "state_dict" in ck else ck
    dst = model.state_dict()
    ok = {k: v for k, v in src.items()
          if k in dst and dst[k].shape == v.shape and not k.startswith("head.3")}
    model.load_state_dict(ok, strict=False)
    return len(ok)
