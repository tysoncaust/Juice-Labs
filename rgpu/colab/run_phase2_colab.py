#!/usr/bin/env python3
"""Strict Colab acceptance runner for the expanded Phase 2 Vulkan executor.

Run from the Juice-Labs repository root in a Colab GPU runtime. The runner rejects
CPU Vulkan devices and rejects software H.264 fallback.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
PHASE2 = ROOT / "rgpu" / "renderd" / "linux" / "phase2"


def run(argv: list[str], *, env: dict[str, str] | None = None) -> None:
    print("+", " ".join(argv), flush=True)
    subprocess.run(argv, cwd=ROOT, env=env, check=True)


def main() -> int:
    if not Path("/content").exists():
        print("warning: /content is absent; this does not look like Colab", file=sys.stderr)
    run(["bash", str(ROOT / "rgpu" / "colab" / "install_nvidia_vulkan.sh")])
    run(["apt-get", "update"])
    run(["apt-get", "install", "-y", "g++", "glslang-tools", "vulkan-tools", "libvulkan-dev", "ffmpeg"])

    env = os.environ.copy()
    env["RGPU_REQUIRE_HARDWARE_VULKAN"] = "1"
    env["RGPU_REQUIRE_NVENC"] = "1"
    run(["bash", str(PHASE2 / "run_phase2.sh")], env=env)

    evidence_path = PHASE2 / "out" / "phase2-evidence.json"
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    required = {
        "hardware_vulkan": True,
        "resource_creation": True,
        "graphics_pipeline_created": True,
        "fence_completed": True,
        "compressed_frame_return": True,
        "compressed_encoder": "h264_nvenc",
    }
    for key, expected in required.items():
        if evidence.get(key) != expected:
            raise RuntimeError(f"Phase 2 acceptance mismatch: {key}={evidence.get(key)!r}, expected {expected!r}")
    print("PHASE2_COLAB_HARDWARE_ACCEPTANCE=PASS")
    print(json.dumps(evidence, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
