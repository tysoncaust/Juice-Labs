"""rgpu wire framing (Python) — matches proto/rgpu_protocol.h + rgpu_cmds.h.

One framed message = rgpu_frame_header (20 bytes) + payload. Sent as a single
binary WebSocket message. Used by the controller, the ephemeral agent, and
clients so the exact same protocol flows over the network as in the C++ loopback.
"""
from __future__ import annotations
import struct

MAGIC = 0x52475055
VERSION = 1

# message types (rgpu_msg_type)
(
    MSG_HELLO,
    MSG_CAPS_OFFER,
    MSG_CAPS_REQUIRE,
    MSG_SESSION_OPEN,
    MSG_SESSION_CLOSE,
    MSG_COMMAND_BATCH,
    MSG_RESOURCE_UPLOAD,
    MSG_QUERY,
    MSG_QUERY_RESULT,
    MSG_FRAME_CHUNK,
    MSG_HEARTBEAT,
    MSG_ERROR,
    MSG_DEVICE_LOST,
) = range(1, 14)

# command opcodes (rgpu_cmd_op)
CMD_CREATE_TEXTURE_2D = 101
CMD_CLEAR_RTV = 142
CMD_PRESENT = 180

_HDR = struct.Struct("<IHHIII")   # magic, version, type, session, sequence, payload_len
_REC = struct.Struct("<III")      # op, handle, arg_len


def frame(msg_type: int, payload: bytes = b"", session: int = 0, seq: int = 0) -> bytes:
    return _HDR.pack(MAGIC, VERSION, msg_type, session, seq, len(payload)) + payload


def parse(data: bytes):
    magic, ver, mtype, session, seq, plen = _HDR.unpack_from(data, 0)
    if magic != MAGIC:
        raise ValueError("bad magic")
    return {"type": mtype, "session": session, "seq": seq,
            "payload": data[_HDR.size:_HDR.size + plen]}


def cmd(op: int, handle: int, args: bytes) -> bytes:
    return _REC.pack(op, handle, len(args)) + args


def build_clear_present_batch(width=64, height=64, rgba=(0.2, 0.4, 0.8, 1.0), tex=1) -> bytes:
    """The demo batch: create a render-target texture, clear it, present it."""
    fmt = 28  # DXGI_FORMAT_R8G8B8A8_UNORM
    body = b""
    body += cmd(CMD_CREATE_TEXTURE_2D, tex, struct.pack("<III", width, height, fmt))
    body += cmd(CMD_CLEAR_RTV, tex, struct.pack("<ffff", *rgba))
    body += cmd(CMD_PRESENT, tex, struct.pack("<I", 0))
    return body


def parse_frame_chunk(payload: bytes):
    """FRAME_CHUNK payload = [u32 width][u32 height][rgba...]"""
    w, h = struct.unpack_from("<II", payload, 0)
    return w, h, payload[8:]
