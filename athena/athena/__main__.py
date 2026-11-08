import argparse

from .app import run

p = argparse.ArgumentParser(description="Athena - Wi-Fi CSI activity recognition for ESP32-S3")
p.add_argument("--udp", type=int, default=5005, help="UDP port the board streams to")
p.add_argument("--http", type=int, default=8000, help="local web page port")
p.add_argument("--no-browser", action="store_true")
a = p.parse_args()
run(a.udp, a.http, not a.no_browser)
