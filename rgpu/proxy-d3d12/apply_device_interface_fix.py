from __future__ import annotations

import re
from pathlib import Path

HERE = Path(__file__).resolve().parent
PROXY = HERE / "src" / "rgpu_d3d12_proxy.cpp"


def read_normalized(path: Path) -> str:
    return path.read_text(encoding="utf-8").replace("\r\n", "\n")


def write_crlf(path: Path, text: str) -> None:
    path.write_text(text.replace("\r\n", "\n").replace("\n", "\r\n"), encoding="utf-8", newline="")


backup = PROXY.with_suffix(PROXY.suffix + ".bak-device-interfaces")
if not backup.exists():
    backup.write_bytes(PROXY.read_bytes())

text = read_normalized(PROXY)

old_pattern = re.compile(
    r"static void \*g_hooked_dev_vtbl = nullptr;\n"
    r"static void arm_from_device\(ID3D12Device \*dev, const char \*which\) \{.*?\n\}\n\n"
    r"static bool adapter_is_warp",
    re.S,
)

new_block = r'''static void *g_hooked_base_dev_vtbl = nullptr;
static void *g_hooked_dev4_vtbl = nullptr;
static void *g_hooked_dev9_vtbl = nullptr;

/* Patch the correct COM interface pointer for each method. The previous version
 * queried ID3D12Device4/9 but accidentally passed the original base pointer to
 * patch_slot with Device4/9 slot numbers. That could write beyond a base vtable
 * and, more importantly, did not hook calls made through the actual versioned
 * interface that UE5 holds. */
static void arm_from_device(ID3D12Device *dev, const char *which) {
    if (!dev) return;

    ID3D12Device *base = nullptr;
    ID3D12Device4 *d4 = nullptr;
    ID3D12Device9 *d9 = nullptr;
    dev->QueryInterface(__uuidof(ID3D12Device), (void **)&base);
    dev->QueryInterface(__uuidof(ID3D12Device4), (void **)&d4);
    dev->QueryInterface(__uuidof(ID3D12Device9), (void **)&d9);

    bool pcfs = false, pq = false, pl = false, pq1 = false, pl1 = false;
    void *bv = base ? *(void **)base : nullptr;
    void *v4 = d4 ? *(void **)d4 : nullptr;
    void *v9 = d9 ? *(void **)d9 : nullptr;

    if (base && bv != g_hooked_base_dev_vtbl) {
        pcfs = patch_slot(base, RGPU_SLOT_Device_CheckFeatureSupport,
                          (void *)h_CFS, (void **)&o_CFS);
        pq = patch_slot(base, RGPU_SLOT_Device_CreateCommandQueue,
                        (void *)h_DevCCQ, (void **)&o_DevCCQ);
        pl = patch_slot(base, RGPU_SLOT_Device_CreateCommandList,
                        (void *)h_DevCCL, (void **)&o_DevCCL);
        if (pcfs && pq && pl) g_hooked_base_dev_vtbl = bv;
    }
    if (d4 && v4 != g_hooked_dev4_vtbl) {
        pl1 = patch_slot(d4, RGPU_SLOT_Device4_CreateCommandList1,
                         (void *)h_DevCCL1, (void **)&o_DevCCL1);
        if (pl1) g_hooked_dev4_vtbl = v4;
    }
    if (d9 && v9 != g_hooked_dev9_vtbl) {
        pq1 = patch_slot(d9, RGPU_SLOT_Device9_CreateCommandQueue1,
                         (void *)h_DevCCQ1, (void **)&o_DevCCQ1);
        if (pq1) g_hooked_dev9_vtbl = v9;
    }

    rgpu_log("armed %s device input=%p base=%p/vtbl=%p d4=%p/vtbl=%p d9=%p/vtbl=%p: CFS=%d CCQ=%d CCL=%d CCQ1=%d CCL1=%d",
             which, (void *)dev, (void *)base, bv, (void *)d4, v4, (void *)d9, v9,
             pcfs, pq, pl, pq1, pl1);

    if (d9) d9->Release();
    if (d4) d4->Release();
    if (base) base->Release();
}

static bool adapter_is_warp'''

text, count = old_pattern.subn(new_block, text, count=1)
if count != 1:
    raise RuntimeError(f"Expected one arm_from_device block, found {count}")

old_log = '''    rgpu_log("D3D12CreateDevice fl=0x%X adapter=%s -> hr=0x%08lX device=%p", (unsigned)fl,
             warp ? "WARP" : "hardware", (unsigned long)hr, ppDevice ? *ppDevice : nullptr);
'''
new_log = '''    rgpu_log("D3D12CreateDevice riid={%08lX-%04X-%04X-...} fl=0x%X adapter=%s -> hr=0x%08lX device=%p vtbl=%p",
             (unsigned long)riid.Data1, (unsigned)riid.Data2, (unsigned)riid.Data3, (unsigned)fl,
             warp ? "WARP" : "hardware", (unsigned long)hr, ppDevice ? *ppDevice : nullptr,
             (ppDevice && *ppDevice) ? *(void **)*ppDevice : nullptr);
'''
if old_log not in text:
    raise RuntimeError("D3D12CreateDevice log fragment not found")
text = text.replace(old_log, new_log, 1)

write_crlf(PROXY, text)
print("Applied interface-correct ID3D12Device4/9 vtable patching and IID diagnostics.")
