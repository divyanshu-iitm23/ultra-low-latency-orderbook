#!/usr/bin/env python3
# udp_ws_bridge.py - bridge the C++ UDP snapshot publisher to browser WebSockets.
#
#   python3 dashboard/udp_ws_bridge.py [udp_port] [ws_port]    (defaults: 9099 8765)
#
# one snapshot == one datagram == one WebSocket message

import asyncio
import sys
from websockets.asyncio.server import serve, broadcast

UDP_PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 9099
WS_PORT  = int(sys.argv[2]) if len(sys.argv) > 2 else 8765

CLIENTS = set()          # currently-connected dashboard sockets

async def ws_handler(ws):
    # We only ever push to the browser; anything it sends is ignored.
    CLIENTS.add(ws)
    try:
        await ws.wait_closed()
    finally:
        CLIENTS.discard(ws)

class UdpRelay(asyncio.DatagramProtocol):
    def datagram_received(self, data, addr):
        if CLIENTS:
            # forward as a TEXT frame so the browser gets a JSON string in event.data
            broadcast(CLIENTS, data.decode("utf-8", "replace"))

async def main():
    loop = asyncio.get_running_loop()
    await loop.create_datagram_endpoint(UdpRelay, local_addr=("127.0.0.1", UDP_PORT))
    async with serve(ws_handler, "127.0.0.1", WS_PORT):
        print(f"bridge: UDP :{UDP_PORT}  ->  ws://127.0.0.1:{WS_PORT}", flush=True)
        await asyncio.Future()     # run forever (until Ctrl-C)

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nbridge: stopped")
