#!/usr/bin/env python3
"""Emit an rgpu protocol COMMAND_BATCH (matches proto/rgpu_protocol.h + rgpu_cmds.h)
for the Vulkan backend to consume: create a 64x64 RT, clear it to a known color,
present. Proves cross-language wire compatibility with the C++ frontend."""
import struct, sys

MAGIC = 0x52475055           # 'RGPU'
VER, TYPE = 1, 6             # RGPU_MSG_COMMAND_BATCH
CREATE_TEXTURE_2D, CREATE_RTV, CLEAR_RTV, PRESENT = 101, 103, 142, 180

def rec(op, handle, args=b""):
    return struct.pack("<III", op, handle, len(args)) + args

color = (0.2, 0.4, 0.8, 1.0)
body  = rec(CREATE_TEXTURE_2D, 1, struct.pack("<III", 64, 64, 28))  # 28 = DXGI_FORMAT_R8G8B8A8_UNORM
body += rec(CREATE_RTV, 1)
body += rec(CLEAR_RTV, 1, struct.pack("<4f", *color))
body += rec(PRESENT, 1, struct.pack("<I", 0))
hdr   = struct.pack("<IHHIII", MAGIC, VER, TYPE, 1, 1, len(body))

out = sys.argv[1] if len(sys.argv) > 1 else "batch.bin"
open(out, "wb").write(hdr + body)
print(f"wrote {out} ({len(hdr)+len(body)} bytes), clear color {color}")
