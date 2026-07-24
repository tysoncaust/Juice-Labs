#!/usr/bin/env python3
"""One-shot compatibility input server for the owned RGPU test window only."""

from __future__ import annotations

import argparse
import socket
import sys


def receive_line(connection: socket.socket, maximum: int = 512) -> str:
    data = bytearray()
    while len(data) < maximum:
        chunk = connection.recv(1)
        if not chunk:
            raise ConnectionError("peer closed before newline")
        if chunk == b"\n":
            return data.decode("utf-8", errors="strict").rstrip("\r")
        data.extend(chunk)
    raise ValueError("protocol line exceeded limit")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--listen", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=50001)
    parser.add_argument("--token", required=True)
    parser.add_argument("--timeout", type=float, default=30.0)
    arguments = parser.parse_args()

    if len(arguments.token) < 16:
        raise SystemExit("token must contain at least 16 characters")

    expected_hello = f"RGPU1 {arguments.token} READY"
    command = f"RGPU1 {arguments.token} KEY_A\n".encode("utf-8")

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind((arguments.listen, arguments.port))
        listener.listen(1)
        listener.settimeout(arguments.timeout)
        connection, peer = listener.accept()
        with connection:
            connection.settimeout(arguments.timeout)
            hello = receive_line(connection)
            if hello != expected_hello:
                print("MAC_REMOTE_INPUT_CLIENT=FAIL reason=bad_hello", file=sys.stderr)
                return 2
            connection.sendall(command)
            result = receive_line(connection)

    passed = result == "PASS"
    print(f"MAC_REMOTE_INPUT_CLIENT={'PASS' if passed else 'FAIL'}")
    print(f"PEER_ADDRESS={peer[0]}")
    print("COMMAND=KEY_A")
    print("TARGET_CONTRACT=owned_rgpu_test_window_only")
    print("PROTECTED_GAME_TARGETED=false")
    return 0 if passed else 3


if __name__ == "__main__":
    raise SystemExit(main())
