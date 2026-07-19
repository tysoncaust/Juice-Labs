#!/usr/bin/env python3
"""rgpu ephemeral render agent.

Runs beside the renderer (on Colab in production, on the RTX host for the
reference). Dials OUT to the controller, registers its capabilities, then for
each COMMAND_BATCH it receives it drives the native renderer and returns the
frame. The renderer is invoked as a subprocess that reads a batch on stdin and
writes [u32 w][u32 h][rgba] on stdout (rgpu-renderd-win CLI, or the Linux Vulkan
renderer with the same contract).

  python rgpu_agent.py --controller ws://127.0.0.1:8791 \
      --renderer <path-to-rgpu_renderd_cli(.exe)> [--caps caps.json]
"""
from __future__ import annotations
import argparse
import asyncio
import json
import os
import subprocess
import sys

import websockets

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "controller"))
import rgpu_wire as w


def default_caps(renderer: str) -> dict:
    # Production: use rgpu/colab/rgpu_probe.py output. Reference (Windows D3D11):
    return {
        "provider": "reference-win-d3d11",
        "gpu": "host D3D11 renderer",
        "graphics": {"vulkan": False, "apiVersion": "n/a", "headlessRendering": True},
        "encoding": {"h264": False, "hevc": False, "av1": False},
        "d3dFrontend": {"d3d11": True, "featureLevel": "11_1"},
        "allocatable": True,
        "renderer": os.path.basename(renderer),
    }


async def run(controller: str, renderer: str, caps: dict, token: str | None):
    async with websockets.connect(controller, max_size=16 * 1024 * 1024) as ws:
        hello = {"role": "agent"}
        if token:
            hello["token"] = token
        await ws.send(w.frame(w.MSG_HELLO, json.dumps(hello).encode()))
        await ws.send(w.frame(w.MSG_CAPS_OFFER, json.dumps(caps).encode()))
        print(f"[agent] registered with controller as {caps.get('gpu')}; waiting for batches")
        async for raw in ws:
            m = w.parse(raw)
            if m["type"] == w.MSG_COMMAND_BATCH:
                proc = subprocess.run([renderer], input=raw, capture_output=True)
                if proc.returncode != 0:
                    await ws.send(w.frame(w.MSG_ERROR, proc.stderr[:200] or b"renderer failed"))
                    continue
                # renderer stdout IS the FRAME_CHUNK payload ([w][h][rgba])
                await ws.send(w.frame(w.MSG_FRAME_CHUNK, proc.stdout, session=m["session"]))
                print(f"[agent] rendered + returned frame ({len(proc.stdout)} bytes)")
            elif m["type"] == w.MSG_SESSION_CLOSE:
                print("[agent] session closed by controller")
                break


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--controller", default="ws://127.0.0.1:8791")
    ap.add_argument("--renderer", required=True)
    ap.add_argument("--caps", help="path to a capability JSON (else a reference default)")
    a = ap.parse_args()
    caps = json.load(open(a.caps)) if a.caps else default_caps(a.renderer)
    asyncio.run(run(a.controller, a.renderer, caps, os.environ.get("RGPU_TOKEN")))


if __name__ == "__main__":
    main()
