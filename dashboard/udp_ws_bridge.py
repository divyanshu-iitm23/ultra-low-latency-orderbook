#!/usr/bin/env python3
# udp_ws_bridge.py - bridge the C++ UDP snapshot publisher to browser WebSockets.
# one snapshot == one datagram == one WebSocket message (and one NDJSON line when logging)
#
import asyncio
import sys
import time
from websockets.asyncio.server import serve, broadcast

# args: positional ports, plus an optional --log[=file] flag
pos, LOG_FILE = [], None
for a in sys.argv[1:]:
    if a == "--log":            LOG_FILE = time.strftime("session-%Y%m%d-%H%M%S.ndjson")
    elif a.startswith("--log="): LOG_FILE = a[len("--log="):]
    else:                        pos.append(a)
UDP_PORT = int(pos[0]) if len(pos) > 0 else 9099
WS_PORT  = int(pos[1]) if len(pos) > 1 else 8765

CLIENTS = set()                                  # currently-connected dashboard sockets
logf = open(LOG_FILE, "w") if LOG_FILE else None # NDJSON recording, if requested

async def ws_handler(ws):
    CLIENTS.add(ws)
    try:
        await ws.wait_closed()
    finally:
        CLIENTS.discard(ws)

class UdpRelay(asyncio.DatagramProtocol):
    def datagram_received(self, data, addr):
        line = data.decode("utf-8", "replace")
        if CLIENTS:
            broadcast(CLIENTS, line)             # fan out to dashboards
        if logf:
            logf.write(line + "\n"); logf.flush() # tee to the NDJSON recording

async def main():
    loop = asyncio.get_running_loop()
    await loop.create_datagram_endpoint(UdpRelay, local_addr=("127.0.0.1", UDP_PORT))
    async with serve(ws_handler, "127.0.0.1", WS_PORT):
        log = f"  ·  logging -> {LOG_FILE}" if logf else ""
        print(f"bridge: UDP :{UDP_PORT}  ->  ws://127.0.0.1:{WS_PORT}{log}", flush=True)
        await asyncio.Future()

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nbridge: stopped")
    finally:
        if logf:
            logf.close()
