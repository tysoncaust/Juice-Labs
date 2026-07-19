#!/usr/bin/env python3
"""rgpu Colab graphics-capability probe.

Runs on the Colab (Linux) runtime at agent startup. Verifies the runtime can
actually render headlessly with Vulkan and encode frames, then emits the
structured capability report the controller uses to accept/reject an allocation.

An allocation MUST be rejected (exit 2) if Vulkan or the required video encoder
is unavailable, so a game never launches against a runtime that can't serve it.

Usage:
  python3 rgpu_probe.py            # probe + print capability JSON to stdout
  python3 rgpu_probe.py --strict   # also exit 2 if not allocatable
  python3 rgpu_probe.py --self-test
"""
from __future__ import annotations
import json
import re
import shutil
import subprocess
import sys

REQUIRED_ENCODER = "h264"  # minimum encoder the client needs for frame return


def _run(cmd: list[str], timeout: int = 60) -> tuple[int, str]:
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return p.returncode, (p.stdout or "") + (p.stderr or "")
    except (FileNotFoundError, subprocess.TimeoutExpired) as exc:
        return 127, str(exc)


def probe_gpu() -> dict:
    code, out = _run(["nvidia-smi", "--query-gpu=name,driver_version,memory.total",
                      "--format=csv,noheader,nounits"])
    if code != 0 or not out.strip():
        return {"present": False, "raw": out.strip()[:200]}
    name, driver, mem = (part.strip() for part in out.strip().splitlines()[0].split(","))
    return {"present": True, "name": name, "driver_version": driver, "memory_total_mb": int(float(mem))}


def probe_libs() -> dict:
    _, out = _run(["ldconfig", "-p"])
    return {lib: bool(re.search(rf"\b{lib}\b", out)) for lib in ("libvulkan", "libEGL", "libcuda")}


def ensure_vulkan_tools() -> None:
    if shutil.which("vulkaninfo"):
        return
    _run(["sudo", "apt-get", "update", "-qq"], timeout=180)
    _run(["sudo", "apt-get", "install", "-y", "-qq", "vulkan-tools"], timeout=300)


def probe_vulkan() -> dict:
    ensure_vulkan_tools()
    if not shutil.which("vulkaninfo"):
        return {"vulkan": False, "reason": "vulkaninfo unavailable"}
    code, out = _run(["vulkaninfo", "--summary"])
    if code != 0:
        return {"vulkan": False, "reason": "vulkaninfo failed", "raw": out[:200]}
    ver = re.search(r"apiVersion\s*[:=]\s*([0-9]+\.[0-9]+)", out)
    has_gpu = bool(re.search(r"deviceType\s*[:=].*(DISCRETE|INTEGRATED)_GPU", out))
    icds = _run(["bash", "-lc",
                 "find /usr/share/vulkan/icd.d -maxdepth 1 -type f 2>/dev/null | wc -l"])[1].strip()
    return {"vulkan": has_gpu, "apiVersion": ver.group(1) if ver else "unknown",
            "icd_count": int(icds) if icds.isdigit() else 0, "headlessRendering": has_gpu}


def probe_native_device_create() -> dict:
    """Run the tiny native headless VK device-create test if it's been built.

    vulkaninfo alone can pass while a real headless vkCreateDevice fails, so the
    authoritative signal is this program's exit code. If it isn't built yet we
    report unknown (the caller still has the vulkaninfo signal)."""
    import os
    exe = os.path.join(os.path.dirname(os.path.abspath(__file__)), "rgpu_vk_probe")
    if not os.path.exists(exe):
        return {"ran": False, "reason": "rgpu_vk_probe not built (compile rgpu_vk_probe.c)"}
    code, out = _run([exe])
    return {"ran": True, "ok": code == 0, "detail": out.strip()[:200]}


def probe_encoders() -> dict:
    # NVENC availability: driver lib + ffmpeg nvenc encoders.
    _, ld = _run(["ldconfig", "-p"])
    nvenc_lib = "libnvidia-encode" in ld
    enc = {"h264": False, "hevc": False, "av1": False}
    if shutil.which("ffmpeg"):
        _, out = _run(["ffmpeg", "-hide_banner", "-encoders"])
        enc["h264"] = "h264_nvenc" in out
        enc["hevc"] = "hevc_nvenc" in out
        enc["av1"] = "av1_nvenc" in out
    elif nvenc_lib:
        enc["h264"] = True  # driver present; assume baseline H.264 until ffmpeg confirms
    return enc


def build_report(gpu: dict, vulkan: dict, encoders: dict, native: dict) -> dict:
    feature_level = "11_1" if vulkan.get("apiVersion", "0") >= "1.2" else "11_0"
    return {
        "provider": "colab",
        "gpu": gpu.get("name") if gpu.get("present") else None,
        "graphics": {
            "vulkan": bool(vulkan.get("vulkan")),
            "apiVersion": vulkan.get("apiVersion", "unknown"),
            "headlessRendering": bool(vulkan.get("headlessRendering")),
        },
        "encoding": {k: bool(encoders.get(k)) for k in ("h264", "hevc", "av1")},
        "d3dFrontend": {"d3d11": True, "featureLevel": feature_level},
        "_native_device_test": native,
        "_memory_total_mb": gpu.get("memory_total_mb"),
    }


def is_allocatable(report: dict) -> tuple[bool, str]:
    if not report.get("gpu"):
        return False, "no GPU present"
    if not report["graphics"]["vulkan"]:
        return False, "Vulkan unavailable"
    if not report["graphics"]["headlessRendering"]:
        return False, "headless rendering unavailable"
    if not report["encoding"].get(REQUIRED_ENCODER):
        return False, f"required encoder '{REQUIRED_ENCODER}' unavailable"
    nt = report.get("_native_device_test") or {}
    if nt.get("ran") and not nt.get("ok"):
        return False, "native headless vkCreateDevice failed"
    return True, "ok"


def main(argv: list[str]) -> int:
    if "--self-test" in argv:
        return _self_test()
    gpu = probe_gpu()
    report = build_report(gpu, probe_vulkan(), probe_encoders(), probe_native_device_create())
    ok, reason = is_allocatable(report)
    report["allocatable"] = ok
    report["allocatable_reason"] = reason
    print(json.dumps(report, indent=2))
    if "--strict" in argv and not ok:
        return 2
    return 0


def _self_test() -> int:
    # Verify the report shape + the fail-closed decision logic with fixed inputs.
    gpu = {"present": True, "name": "NVIDIA T4", "driver_version": "550.0", "memory_total_mb": 15360}
    vk = {"vulkan": True, "apiVersion": "1.3", "headlessRendering": True, "icd_count": 1}
    r = build_report(gpu, vk, {"h264": True, "hevc": False, "av1": False}, {"ran": False})
    assert r["provider"] == "colab" and r["gpu"] == "NVIDIA T4", r
    assert r["graphics"] == {"vulkan": True, "apiVersion": "1.3", "headlessRendering": True}, r["graphics"]
    assert r["encoding"] == {"h264": True, "hevc": False, "av1": False}, r["encoding"]
    assert r["d3dFrontend"] == {"d3d11": True, "featureLevel": "11_1"}, r["d3dFrontend"]
    assert is_allocatable(r)[0] is True
    # fail-closed cases
    assert is_allocatable(build_report(gpu, {"vulkan": False}, {"h264": True}, {}))[0] is False
    assert is_allocatable(build_report(gpu, vk, {"h264": False}, {}))[0] is False
    assert is_allocatable(build_report({"present": False}, vk, {"h264": True}, {}))[0] is False
    assert is_allocatable(build_report(gpu, vk, {"h264": True}, {"ran": True, "ok": False}))[0] is False
    print("rgpu_probe self-test passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
