# Phase 0 status

Run `build_phase0.ps1` and use `out/phase0-evidence.txt` as the machine-generated status. Phase 0 is split deliberately:

1. **Completed user-mode feasibility proof:** bounded protocol, submission queue, remote-style executor boundary, returned BGRA frame, fence completion, CRC verification, and local presentation window.
2. **Prepared but not deployed driver boundary:** root-enumerated Display-class INF and named KMD/UMD package contract.
3. **Remaining WDDM proof:** implement actual KMD/UMD callbacks with the installed WDK, build/catalog/sign them, then install only in a disposable test VM and verify DXGI enumeration.

No claim of a working virtual adapter is made until Device Manager and DXGI enumerate the installed adapter and a D3D12 device can be created through its UMD.
