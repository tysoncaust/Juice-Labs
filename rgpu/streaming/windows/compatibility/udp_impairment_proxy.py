from __future__ import annotations

import argparse
import heapq
import json
import random
import selectors
import socket
import time
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from pathlib import Path


@dataclass
class DirectionStats:
    received_packets: int = 0
    received_bytes: int = 0
    forwarded_packets: int = 0
    forwarded_bytes: int = 0
    dropped_packets: int = 0
    dropped_bytes: int = 0


@dataclass(order=True)
class ScheduledPacket:
    release_time: float
    sequence: int
    payload: bytes
    destination: tuple[str, int]
    direction: str


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Bidirectional UDP impairment proxy for deterministic SRT loss/jitter testing"
    )
    parser.add_argument("--listen-host", default="127.0.0.1")
    parser.add_argument("--listen-port", type=int, required=True)
    parser.add_argument("--server-host", required=True)
    parser.add_argument("--server-port", type=int, required=True)
    parser.add_argument("--loss-percent", type=float, default=2.0)
    parser.add_argument("--max-jitter-ms", type=float, default=12.0)
    parser.add_argument("--duration", type=float, default=45.0)
    parser.add_argument("--seed", type=int, default=20260724)
    parser.add_argument("--evidence", required=True)
    args = parser.parse_args()

    if not 0.0 <= args.loss_percent <= 25.0:
        raise SystemExit("--loss-percent must be between 0 and 25")
    if not 0.0 <= args.max_jitter_ms <= 500.0:
        raise SystemExit("--max-jitter-ms must be between 0 and 500")
    if not 3.0 <= args.duration <= 600.0:
        raise SystemExit("--duration must be between 3 and 600 seconds")
    for port in (args.listen_port, args.server_port):
        if not 1 <= port <= 65535:
            raise SystemExit("ports must be between 1 and 65535")

    random_source = random.Random(args.seed)
    server_address = (socket.gethostbyname(args.server_host), args.server_port)
    socket_handle = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    socket_handle.setblocking(False)
    socket_handle.bind((args.listen_host, args.listen_port))

    selector = selectors.DefaultSelector()
    selector.register(socket_handle, selectors.EVENT_READ)
    client_address: tuple[str, int] | None = None
    pending: list[ScheduledPacket] = []
    sequence = 0
    maximum_queue_depth = 0
    directions = {
        "client_to_server": DirectionStats(),
        "server_to_client": DirectionStats(),
    }
    start_monotonic = time.monotonic()
    started_utc = utc_now()
    stop_at = start_monotonic + args.duration

    try:
        while time.monotonic() < stop_at or pending:
            now = time.monotonic()
            while pending and pending[0].release_time <= now:
                item = heapq.heappop(pending)
                sent = socket_handle.sendto(item.payload, item.destination)
                stats = directions[item.direction]
                stats.forwarded_packets += 1
                stats.forwarded_bytes += sent

            if time.monotonic() >= stop_at and not pending:
                break

            next_release = pending[0].release_time if pending else stop_at
            timeout = max(0.0, min(0.1, next_release - time.monotonic(), stop_at - time.monotonic()))
            for key, _ in selector.select(timeout):
                if key.fileobj is not socket_handle:
                    continue
                try:
                    payload, source = socket_handle.recvfrom(65535)
                except BlockingIOError:
                    continue

                if source[0] == server_address[0] and source[1] == server_address[1]:
                    direction = "server_to_client"
                    if client_address is None:
                        continue
                    destination = client_address
                else:
                    direction = "client_to_server"
                    client_address = source
                    destination = server_address

                stats = directions[direction]
                stats.received_packets += 1
                stats.received_bytes += len(payload)
                if random_source.random() < args.loss_percent / 100.0:
                    stats.dropped_packets += 1
                    stats.dropped_bytes += len(payload)
                    continue

                jitter_seconds = random_source.uniform(0.0, args.max_jitter_ms / 1000.0)
                sequence += 1
                heapq.heappush(
                    pending,
                    ScheduledPacket(
                        release_time=time.monotonic() + jitter_seconds,
                        sequence=sequence,
                        payload=payload,
                        destination=destination,
                        direction=direction,
                    ),
                )
                maximum_queue_depth = max(maximum_queue_depth, len(pending))
    finally:
        selector.close()
        socket_handle.close()

    ended_utc = utc_now()
    total_received = sum(item.received_packets for item in directions.values())
    total_forwarded = sum(item.forwarded_packets for item in directions.values())
    total_dropped = sum(item.dropped_packets for item in directions.values())
    actual_loss_percent = (100.0 * total_dropped / total_received) if total_received else 0.0
    passed = (
        client_address is not None
        and total_received >= 100
        and total_forwarded >= 50
        and total_dropped >= 1
    )

    summary = {
        "schema": "rgpu-udp-impairment-proxy-v1",
        "timestamp_utc": ended_utc,
        "started_utc": started_utc,
        "passed": passed,
        "listen_address": f"{args.listen_host}:{args.listen_port}",
        "server_address": f"{server_address[0]}:{server_address[1]}",
        "client_address_observed": f"{client_address[0]}:{client_address[1]}"
        if client_address
        else None,
        "configured_loss_percent": args.loss_percent,
        "actual_loss_percent": round(actual_loss_percent, 4),
        "maximum_jitter_ms": args.max_jitter_ms,
        "seed": args.seed,
        "duration_seconds": round(time.monotonic() - start_monotonic, 3),
        "maximum_queue_depth": maximum_queue_depth,
        "total_received_packets": total_received,
        "total_forwarded_packets": total_forwarded,
        "total_dropped_packets": total_dropped,
        "directions": {name: asdict(stats) for name, stats in directions.items()},
        "payload_inspection": False,
        "game_process_access": False,
    }
    evidence_path = Path(args.evidence).expanduser().resolve()
    evidence_path.parent.mkdir(parents=True, exist_ok=True)
    evidence_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")

    print(f"UDP_IMPAIRMENT_PROXY={'PASS' if passed else 'FAIL'}")
    print(f"CONFIGURED_LOSS_PERCENT={args.loss_percent}")
    print(f"ACTUAL_LOSS_PERCENT={summary['actual_loss_percent']}")
    print(f"MAXIMUM_JITTER_MS={args.max_jitter_ms}")
    print(f"TOTAL_RECEIVED_PACKETS={total_received}")
    print(f"TOTAL_DROPPED_PACKETS={total_dropped}")
    print(f"EVIDENCE={evidence_path}")
    return 0 if passed else 2


if __name__ == "__main__":
    raise SystemExit(main())
