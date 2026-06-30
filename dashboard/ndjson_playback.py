#!/usr/bin/env python3
# ndjson_playback.py - replay a recorded NDJSON session over UDP, back through the bridge.
#

import socket, sys, json, time

path, speed, host, port, loop = None, 1.0, "127.0.0.1", 9099, False
for a in sys.argv[1:]:
    if   a.startswith("--speed="): speed = float(a.split("=", 1)[1])
    elif a.startswith("--host="):  host  = a.split("=", 1)[1]
    elif a.startswith("--port="):  port  = int(a.split("=", 1)[1])
    elif a == "--loop":            loop  = True
    elif not a.startswith("--"):   path  = a
if not path:
    sys.exit("usage: ndjson_playback.py <file.ndjson> [--speed=N] [--host=H] [--port=P] [--loop]")

lines = [ln.strip() for ln in open(path) if ln.strip()]
sock  = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
print(f"playback: {len(lines)} frames -> {host}:{port}  speed={speed}x"
      f"{' (looping)' if loop else ''}", flush=True)

def play_once():
    prev_t = None
    for ln in lines:
        try:    t = json.loads(ln).get("t")
        except Exception: t = None
        if prev_t is not None and t is not None and t > prev_t and speed > 0:
            time.sleep((t - prev_t) / speed)     # pace by recorded uptime
        sock.sendto(ln.encode("utf-8"), (host, port))
        if t is not None:
            prev_t = t

try:
    play_once()
    while loop:
        play_once()
except KeyboardInterrupt:
    pass
print("playback: done", flush=True)
