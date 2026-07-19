#!/usr/bin/env python3
"""rgpu client — end-to-end transport proof.

Connects to the controller, opens a session (matched to a ready remote renderer),
sends a COMMAND_BATCH (create RT, clear to a known color, present) over the
WebSocket, receives the FRAME_CHUNK, and verifies the returned pixels. Proves the
full networked path: client -> controller -> agent -> native renderer -> frame back.
"""
from __future__ import annotations
import asyncio
import json
import os
import sys

import websockets

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rgpu_wire as w

EXPECT = (51, 102, 204, 255)


async def run(controller: str, token: str | None) -> int:
    async with websockets.connect(controller, max_size=16 * 1024 * 1024) as ws:
        hello = {"role": "client"}
        if token:
            hello["token"] = token
        await ws.send(w.frame(w.MSG_HELLO, json.dumps(hello).encode()))
        await ws.send(w.frame(w.MSG_SESSION_OPEN))
        opened = w.parse(await ws.recv())
        if opened["type"] != w.MSG_SESSION_OPEN:
            print("FAIL: session not opened:", opened["payload"][:120]); return 1
        caps = json.loads(opened["payload"] or b"{}")
        print(f"[client] session {opened['session']} on remote GPU: {caps.get('gpu')} "
              f"(d3d11={caps.get('d3dFrontend',{}).get('d3d11')})")

        batch = w.build_clear_present_batch()
        await ws.send(w.frame(w.MSG_COMMAND_BATCH, batch, session=opened["session"], seq=1))
        resp = w.parse(await ws.recv())
        if resp["type"] != w.MSG_FRAME_CHUNK:
            print("FAIL: expected FRAME_CHUNK, got", resp["type"], resp["payload"][:120]); return 1
        width, height, rgba = w.parse_frame_chunk(resp["payload"])
        px = tuple(rgba[:4])
        print(f"[client] received frame {width}x{height}, top-left pixel = {px}")
        await ws.send(w.frame(w.MSG_SESSION_CLOSE, session=opened["session"]))

        ok = width == 64 and height == 64 and all(abs(px[i] - EXPECT[i]) <= 1 for i in range(4))
        print("RESULT:", "networked remote frame verified" if ok else f"FAIL pixel {px} != {EXPECT}")
        return 0 if ok else 1


if __name__ == "__main__":
    controller = sys.argv[1] if len(sys.argv) > 1 else "ws://127.0.0.1:8791"
    raise SystemExit(asyncio.run(run(controller, os.environ.get("RGPU_TOKEN"))))
