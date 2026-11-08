"""Wire format sent by the ESP32-S3 (role 'CSI stream')."""
import struct
import numpy as np

MAGIC = b"CSI1"
_HDR = struct.Struct("<4sIIbB")   # magic, seq, ts_ms, rssi, n


def parse(pkt: bytes):
    """Returns (seq, ts_ms, rssi, amp[n] uint8) or None for a bad packet."""
    if len(pkt) < _HDR.size:
        return None
    magic, seq, ts, rssi, n = _HDR.unpack_from(pkt)
    # The firmware streams exactly 64 LLTF subcarriers. Reject other lengths
    # here so malformed or unrelated UDP packets never reach the 64-feature UI.
    if magic != MAGIC or n != 64 or len(pkt) != _HDR.size + n:
        return None
    amp = np.frombuffer(pkt, dtype=np.uint8, count=n, offset=_HDR.size)
    return seq, ts, rssi, amp


def build(seq, ts_ms, rssi, amp) -> bytes:
    """Used by the fake ESP simulator and the tests."""
    amp = np.asarray(amp, dtype=np.uint8)
    return _HDR.pack(MAGIC, seq & 0xFFFFFFFF, ts_ms & 0xFFFFFFFF, rssi, len(amp)) + amp.tobytes()
