from pathlib import Path
p = Path(r'C:\Users\email\Documents\GitHub\Juice-Labs\rgpu\proxy-d3d12\src\rgpu_d3d12_proxy.cpp')
s = p.read_text(encoding='utf-8').replace('\r\n','\n')
needle = '''/* ---------------- system d3d12.dll forwarding ----------------------------- */\n'''
insert = '''extern "C" __declspec(dllexport) void WINAPI rgpu_d3d12_arm_external_device(ID3D12Device *dev, const char *source) {\n    rgpu_log("external device arm source=%s device=%p vtbl=%p",\n             source ? source : "unknown", (void *)dev, dev ? *(void **)dev : nullptr);\n    arm_from_device(dev, source ? source : "external");\n}\n\n/* ---------------- system d3d12.dll forwarding ----------------------------- */\n'''
if 'rgpu_d3d12_arm_external_device' not in s:
    if s.count(needle) != 1:
        raise RuntimeError(f'Expected system-forward marker once, found {s.count(needle)}')
    s = s.replace(needle, insert, 1)
p.write_text(s.replace('\n','\r\n'), encoding='utf-8', newline='')
print('Added external-device arm export to D3D12 proxy.')
