from __future__ import annotations

import re
from pathlib import Path

HERE = Path(__file__).resolve().parent
PROXY = HERE / "src" / "rgpu_d3d12_proxy.cpp"


def read_normalized(path: Path) -> str:
    return path.read_text(encoding="utf-8").replace("\r\n", "\n")


def write_crlf(path: Path, text: str) -> None:
    path.write_text(text.replace("\r\n", "\n").replace("\n", "\r\n"), encoding="utf-8", newline="")


backup = PROXY.with_suffix(PROXY.suffix + ".bak-device-inline")
if not backup.exists():
    backup.write_bytes(PROXY.read_bytes())

text = read_normalized(PROXY)
pattern = re.compile(
    r"static void \*g_hooked_base_dev_vtbl = nullptr;.*?\n"
    r"static bool adapter_is_warp",
    re.S,
)
replacement = r'''/* Inline-hook device methods from the real returned interface. This survives
 * UE4SS/overlay vtable replacement just like the queue/list inline hooks. */
static void arm_from_device(ID3D12Device *dev, const char *which) {
    if (!dev) return;

    ID3D12Device *base = nullptr;
    ID3D12Device4 *d4 = nullptr;
    ID3D12Device9 *d9 = nullptr;
    dev->QueryInterface(__uuidof(ID3D12Device), (void **)&base);
    dev->QueryInterface(__uuidof(ID3D12Device4), (void **)&d4);
    dev->QueryInterface(__uuidof(ID3D12Device9), (void **)&d9);

    void *bv = base ? *(void **)base : nullptr;
    void *v4 = d4 ? *(void **)d4 : nullptr;
    void *v9 = d9 ? *(void **)d9 : nullptr;

    HookItem items[5]; int n = 0;
    auto add = [&](void *obj, unsigned slot, void *detour, void **orig, const char *name) {
        if (obj && !*orig && n < 5) items[n++] = { vslot(obj, slot), detour, orig, name };
    };
    add(base, RGPU_SLOT_Device_CheckFeatureSupport, (void *)h_CFS, (void **)&o_CFS,
        "Device::CheckFeatureSupport");
    add(base, RGPU_SLOT_Device_CreateCommandQueue, (void *)h_DevCCQ, (void **)&o_DevCCQ,
        "Device::CreateCommandQueue");
    add(base, RGPU_SLOT_Device_CreateCommandList, (void *)h_DevCCL, (void **)&o_DevCCL,
        "Device::CreateCommandList");
    add(d4, RGPU_SLOT_Device4_CreateCommandList1, (void *)h_DevCCL1, (void **)&o_DevCCL1,
        "Device4::CreateCommandList1");
    add(d9, RGPU_SLOT_Device9_CreateCommandQueue1, (void *)h_DevCCQ1, (void **)&o_DevCCQ1,
        "Device9::CreateCommandQueue1");
    install_batch_frozen(items, n);

    rgpu_log("armed-inline %s device input=%p base=%p/vtbl=%p d4=%p/vtbl=%p d9=%p/vtbl=%p originals CFS=%p CCQ=%p CCL=%p CCQ1=%p CCL1=%p",
             which, (void *)dev, (void *)base, bv, (void *)d4, v4, (void *)d9, v9,
             (void *)o_CFS, (void *)o_DevCCQ, (void *)o_DevCCL,
             (void *)o_DevCCQ1, (void *)o_DevCCL1);

    if (d9) d9->Release();
    if (d4) d4->Release();
    if (base) base->Release();
}

static bool adapter_is_warp'''
text, count = pattern.subn(replacement, text, count=1)
if count != 1:
    raise RuntimeError(f"Expected one interface-patching block, found {count}")

write_crlf(PROXY, text)
print("Replaced device vtable hooks with thread-frozen inline hooks.")
