#!/usr/bin/env python3
"""Generate rgpu D3D12 COM wrapper classes (device/queue/graphics-command-list)
from mingw-w64's d3d12.h. Every method forwards to the real object; a small set of
methods (create-queue/list, ExecuteCommandLists, the recording calls) are left as
hand-written overrides injected here. Deriving from the TOP interface of each chain
(ID3D12Device10 / ...GraphicsCommandList7 / ...CommandQueue) makes QueryInterface
able to return our wrapper for every version the game requests, so no raw pointer
ever leaks. Output: src/rgpu_d3d12_wrappers.h."""
import re, sys, os

HDR = sys.argv[1] if len(sys.argv) > 1 else None
OUT = sys.argv[2] if len(sys.argv) > 2 else None
raw = open(HDR, "r", errors="ignore").read()

# Resolve WIDL_EXPLICIT_AGGREGATE_RETURNS as DEFINED. mingw declares aggregate-return
# methods (GetResourceAllocationInfo/GetCustomHeapProperties/GetAdapterLuid/descriptor
# handle getters) twice: an `#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS` form whose vtable
# slot takes a hidden `*__ret` pointer (this MATCHES the MSVC D3D12Core ABI exactly) and
# an `#else` plain by-value form (which mismatches MSVC for <=8-byte returns like LUID).
# We MUST compile the __ret form, so keep the #ifdef body and drop the #else body; without
# this the generator would also emit the #else virtuals -> duplicate/ABI-wrong overrides.
def _resolve_agg(text):
    out, stack = [], []   # stack frames: "AGG"(keep), "AGG_ELSE"(drop), "OTHER"(passthru)
    for ln in text.splitlines(keepends=True):
        s = ln.strip()
        is_if = s.startswith("#if")
        top = stack[-1] if stack else None
        if is_if and ("WIDL_EXPLICIT_AGGREGATE_RETURNS" in s):
            stack.append("AGG"); continue
        if is_if:
            stack.append("OTHER")
            if top != "AGG_ELSE": out.append(ln)
            continue
        if s.startswith("#else") and top == "AGG":
            stack[-1] = "AGG_ELSE"; continue
        if s.startswith("#endif") and top in ("AGG", "AGG_ELSE"):
            stack.pop(); continue
        if any(f == "AGG_ELSE" for f in stack):
            continue                                 # inside a dropped #else body
        out.append(ln)
    return "".join(out)

src = _resolve_agg(raw)

# ---- parse a C++ interface's OWN methods (between "NAME : public PARENT{" .. "};")
def iface_methods(name):
    m = re.search(r'^%s : public (\w+)\s*\{(.*?)^\};' % re.escape(name), src, re.S | re.M)
    if not m:
        return None, []
    parent = m.group(1); body = m.group(2)
    methods = []
    # each: virtual RET STDMETHODCALLTYPE Name( params ) = 0;
    for mm in re.finditer(r'virtual\s+(.*?)\s+STDMETHODCALLTYPE\s+(\w+)\s*\((.*?)\)\s*=\s*0\s*;', body, re.S):
        ret = mm.group(1).strip(); nm = mm.group(2); params = mm.group(3).strip()
        methods.append((ret, nm, params))
    return parent, methods

def chain(top, stop):
    """collect (ret,name,params) for the whole vtable in base->derived order."""
    order = []
    cur = top
    while cur and cur != stop:
        parent, ms = iface_methods(cur)
        order.append((cur, ms))
        cur = parent
    order.reverse()   # base-most first
    result = []
    for _, ms in order:
        result.extend(ms)
    return result

def split_params(params):
    """split 'TYPE a, TYPE b[4], const T* c' into [(text, argname), ...]."""
    if params.strip() == "" or params.strip() == "void":
        return []
    parts, depth, cur = [], 0, ""
    for ch in params:
        if ch in "<([": depth += 1
        elif ch in ">)]": depth -= 1
        if ch == "," and depth == 0:
            parts.append(cur); cur = ""
        else:
            cur += ch
    if cur.strip(): parts.append(cur)
    out = []
    for p in parts:
        p = p.strip()
        # arg name = last identifier, ignoring trailing [..]
        base = re.sub(r'\[.*?\]\s*$', '', p).strip()
        mm = re.search(r'(\w+)\s*$', base)
        name = mm.group(1) if mm else ""
        out.append((p, name))
    return out

def forwarder(ret, nm, params):
    ps = split_params(params)
    sig = ", ".join(p[0] for p in ps)
    call = ", ".join(p[1] for p in ps)
    ret2 = ret.strip()
    body = ("return " if ret2 != "void" else "") + "real_->%s(%s);" % (nm, call)
    return "    %s STDMETHODCALLTYPE %s(%s) override { %s }" % (ret2, nm, sig, call and ("real_->%s(%s);" % (nm, call)) if False else body, )

# The above f-string got messy; build cleanly:
def forwarder(ret, nm, params):
    ps = split_params(params)
    sig = ", ".join(p[0] for p in ps)
    call = ", ".join(p[1] for p in ps)
    ret2 = ret.strip()
    body = ("return " if ret2 != "void" else "") + "real_->" + nm + "(" + call + ");"
    return "    " + ret2 + " STDMETHODCALLTYPE " + nm + "(" + sig + ") override { " + body + " }"

# ---- overrides: methods we DON'T forward verbatim (provided in the .h manually) --
DEVICE_OVERRIDES  = {"QueryInterface","AddRef","Release",
                     "CreateCommandQueue","CreateCommandQueue1","CreateCommandList","CreateCommandList1"}
QUEUE_OVERRIDES   = {"QueryInterface","AddRef","Release","ExecuteCommandLists","GetDevice"}
GCL_OVERRIDES     = {"QueryInterface","AddRef","Release","GetDevice",
                     "DrawInstanced","DrawIndexedInstanced","Dispatch","ExecuteIndirect","DispatchMesh",
                     "ResourceBarrier","Barrier","ClearRenderTargetView","OMSetRenderTargets",
                     "CopyResource","CopyBufferRegion","CopyTextureRegion","Close","Reset"}

def gen_forwarders(top, overrides):
    ms = chain(top, "IUnknown")
    lines = []
    for ret, nm, params in ms:
        if nm in overrides:
            continue
        lines.append(forwarder(ret, nm, params))
    return "\n".join(lines), [ (r,n,p) for (r,n,p) in ms ]

dev_fwd, dev_all = gen_forwarders("ID3D12Device10", DEVICE_OVERRIDES)
q_fwd,   q_all   = gen_forwarders("ID3D12CommandQueue", QUEUE_OVERRIDES)
gcl_fwd, gcl_all = gen_forwarders("ID3D12GraphicsCommandList7", GCL_OVERRIDES)

open(OUT, "w").write(
"/* GENERATED by tools/gen_wrappers.py from mingw d3d12.h. Forwarding methods for the\n"
" * RgpuD3D12Device / RgpuD3D12CommandQueue / RgpuD3D12GraphicsCommandList wrappers.\n"
" * Included inside the class bodies in rgpu_d3d12_wrappers_impl.h. Do not edit. */\n"
"#ifndef RGPU_D3D12_WRAPPERS_GEN_H\n#define RGPU_D3D12_WRAPPERS_GEN_H\n\n"
"#define RGPU_DEVICE_FORWARDERS \\\n" + dev_fwd.replace("\n"," \\\n") + "\n\n"
"#define RGPU_QUEUE_FORWARDERS \\\n" + q_fwd.replace("\n"," \\\n") + "\n\n"
"#define RGPU_GCL_FORWARDERS \\\n" + gcl_fwd.replace("\n"," \\\n") + "\n\n"
"#endif\n"
)
print("device forwarders: %d, queue: %d, gcl: %d methods" %
      (len([m for m in dev_all if m[1] not in DEVICE_OVERRIDES]),
       len([m for m in q_all if m[1] not in QUEUE_OVERRIDES]),
       len([m for m in gcl_all if m[1] not in GCL_OVERRIDES])))
