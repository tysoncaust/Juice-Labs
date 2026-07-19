#!/usr/bin/env python3
"""rgpu controller / rendezvous.

A stable endpoint that ephemeral renderers (Colab agents) dial OUT to, so the
changing Colab address is never baked into the game client. Agents register
their capabilities; clients open a session and are matched to a ready agent;
the controller relays framed rgpu protocol messages between them (command
batches client->agent, encoded frames agent->client).

  python rgpu_controller.py [--host 127.0.0.1] [--port 8791]

Prototype transport is ws:// on loopback; production wraps this in wss:// (TLS)
with a bearer/token check in the HELLO — see register_auth().
"""
from __future__ import annotations
import argparse
import asyncio
import json
import os
import secrets
import sys

import websockets

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rgpu_wire as w

READY_AGENTS: "asyncio.Queue[tuple]" = None  # (ws, caps)
STATE = {"agents_registered": 0, "sessions_opened": 0}
AUTH_TOKEN = os.environ.get("RGPU_TOKEN")  # if set, HELLO must present it


def register_auth(hello_payload: dict) -> bool:
    return AUTH_TOKEN is None or hello_payload.get("token") == AUTH_TOKEN


async def handle_agent(ws, hello: dict):
    caps_msg = w.parse(await ws.recv())
    if caps_msg["type"] != w.MSG_CAPS_OFFER:
        await ws.send(w.frame(w.MSG_ERROR, b"expected CAPS_OFFER")); return
    caps = json.loads(caps_msg["payload"] or b"{}")
    if not caps.get("allocatable", True):
        await ws.send(w.frame(w.MSG_ERROR, b"agent not allocatable")); return
    STATE["agents_registered"] += 1
    parked = asyncio.Event()
    ws.rgpu_parked = parked  # released when a session using it ends
    await READY_AGENTS.put((ws, caps))
    print(f"[controller] agent registered: {caps.get('gpu')} "
          f"(vulkan={caps.get('graphics',{}).get('vulkan')}); ready pool={READY_AGENTS.qsize()}")
    await parked.wait()  # do NOT recv here; the paired session owns this socket


async def handle_client(ws, hello: dict):
    open_msg = w.parse(await ws.recv())
    if open_msg["type"] != w.MSG_SESSION_OPEN:
        await ws.send(w.frame(w.MSG_ERROR, b"expected SESSION_OPEN")); return
    try:
        agent_ws, caps = await asyncio.wait_for(READY_AGENTS.get(), timeout=30)
    except asyncio.TimeoutError:
        await ws.send(w.frame(w.MSG_ERROR, b"no ready remote GPU")); return
    session_id = secrets.randbits(31)
    STATE["sessions_opened"] += 1
    await ws.send(w.frame(w.MSG_SESSION_OPEN, json.dumps(caps).encode(), session=session_id))
    print(f"[controller] session {session_id} opened -> agent {caps.get('gpu')}")
    try:
        async for raw in ws:
            m = w.parse(raw)
            if m["type"] == w.MSG_COMMAND_BATCH:
                await agent_ws.send(raw)               # relay batch to the renderer
                resp = await agent_ws.recv()           # renderer returns a frame
                await ws.send(resp)                    # relay frame to the client
            elif m["type"] == w.MSG_SESSION_CLOSE:
                break
    finally:
        print(f"[controller] session {session_id} closed")
        try:
            await agent_ws.send(w.frame(w.MSG_SESSION_CLOSE))
        except Exception:
            pass
        getattr(agent_ws, "rgpu_parked", asyncio.Event()).set()  # release the agent


async def router(ws):
    try:
        hello = w.parse(await ws.recv())
        if hello["type"] != w.MSG_HELLO:
            await ws.send(w.frame(w.MSG_ERROR, b"expected HELLO")); return
        info = json.loads(hello["payload"] or b"{}")
        if not register_auth(info):
            await ws.send(w.frame(w.MSG_ERROR, b"unauthorized")); return
        if info.get("role") == "agent":
            await handle_agent(ws, info)
        elif info.get("role") == "client":
            await handle_client(ws, info)
        else:
            await ws.send(w.frame(w.MSG_ERROR, b"unknown role"))
    except websockets.ConnectionClosed:
        pass


async def main():
    global READY_AGENTS
    READY_AGENTS = asyncio.Queue()
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8791)
    a = ap.parse_args()
    async with websockets.serve(router, a.host, a.port, max_size=16 * 1024 * 1024):
        print(f"[controller] rgpu rendezvous on ws://{a.host}:{a.port}  (auth={'on' if AUTH_TOKEN else 'off'})")
        await asyncio.Future()


if __name__ == "__main__":
    asyncio.run(main())
