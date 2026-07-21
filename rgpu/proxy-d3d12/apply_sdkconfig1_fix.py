from __future__ import annotations

from pathlib import Path

HERE = Path(__file__).resolve().parent
PROXY = HERE / "src" / "rgpu_d3d12_proxy.cpp"
SLOTS = HERE / "src" / "rgpu_d3d12_slots.cpp"
HARNESS = HERE / "test" / "rgpu_d3d12_harness.cpp"


def read_normalized(path: Path) -> str:
    return path.read_text(encoding="utf-8").replace("\r\n", "\n")


def write_crlf(path: Path, text: str) -> None:
    path.write_text(text.replace("\r\n", "\n").replace("\n", "\r\n"), encoding="utf-8", newline="")


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"Expected exactly one match for {label}; found {count}")
    return text.replace(old, new, 1)


for path in (PROXY, SLOTS, HARNESS):
    backup = path.with_suffix(path.suffix + ".bak-sdkconfig1")
    if not backup.exists():
        backup.write_bytes(path.read_bytes())

proxy = read_normalized(PROXY)
if "RGPU_SLOT_SDKConfig1_CreateDeviceFactory" not in proxy:
    proxy = replace_once(
        proxy,
        """    RGPU_SLOT_GCL_ExecuteIndirect, RGPU_SLOT_GCL6_DispatchMesh, RGPU_SLOT_GCL7_Barrier,\n    RGPU_SLOT_Device_CheckFeatureSupport;\n""",
        """    RGPU_SLOT_GCL_ExecuteIndirect, RGPU_SLOT_GCL6_DispatchMesh, RGPU_SLOT_GCL7_Barrier,\n    RGPU_SLOT_Device_CheckFeatureSupport, RGPU_SLOT_SDKConfig1_CreateDeviceFactory;\n""",
        "extern slot declaration",
    )

    proxy = replace_once(
        proxy,
        """static fnFactoryCreateDevice o_FactoryCreateDevice;\nstatic volatile LONG g_hooked_factory = 0;\n\nstatic HRESULT STDMETHODCALLTYPE h_FactoryCreateDevice(ID3D12DeviceFactory *This, IUnknown *adapter, D3D_FEATURE_LEVEL fl, REFIID riid, void **pp) {\n    HRESULT hr = o_FactoryCreateDevice(This, adapter, fl, riid, pp);\n    bool warp = adapter_is_warp(adapter);\n    rgpu_log(\"ID3D12DeviceFactory::CreateDevice fl=0x%X adapter=%s -> hr=0x%08lX device=%p\",\n             (unsigned)fl, warp ? \"WARP\" : \"hardware\", (unsigned long)hr, pp ? *pp : nullptr);\n    if (SUCCEEDED(hr) && pp && *pp && !warp) arm_from_device((ID3D12Device *)*pp, \"DeviceFactory\");\n    return hr;\n}\n""",
        """static fnFactoryCreateDevice o_FactoryCreateDevice;\nstatic void *g_hooked_factory_vtbl = nullptr;\nstatic volatile LONG g_sdk_config_hooks = 0;\nstatic volatile LONG g_sdk_factory_calls = 0;\nstatic volatile LONG g_factory_device_calls = 0;\n\nstatic HRESULT STDMETHODCALLTYPE h_FactoryCreateDevice(ID3D12DeviceFactory *This, IUnknown *adapter,\n                                                        D3D_FEATURE_LEVEL fl, REFIID riid, void **pp);\n\nstatic bool hook_factory_object(void *obj) {\n    if (!obj) return false;\n    ID3D12DeviceFactory *factory = nullptr;\n    if (FAILED(((IUnknown *)obj)->QueryInterface(__uuidof(ID3D12DeviceFactory), (void **)&factory)) || !factory)\n        return false;\n    void *vtbl = *(void **)factory;\n    bool ok = true;\n    if (vtbl != g_hooked_factory_vtbl) {\n        ok = patch_slot(factory, RGPU_SLOT_Factory_CreateDevice,\n                        (void *)h_FactoryCreateDevice, (void **)&o_FactoryCreateDevice);\n        if (ok) {\n            g_hooked_factory_vtbl = vtbl;\n            rgpu_log(\"hooked ID3D12DeviceFactory::CreateDevice factory=%p vtbl=%p\",\n                     (void *)factory, vtbl);\n        }\n    }\n    factory->Release();\n    return ok;\n}\n\nstatic HRESULT STDMETHODCALLTYPE h_FactoryCreateDevice(ID3D12DeviceFactory *This, IUnknown *adapter, D3D_FEATURE_LEVEL fl, REFIID riid, void **pp) {\n    InterlockedIncrement(&g_factory_device_calls);\n    HRESULT hr = o_FactoryCreateDevice(This, adapter, fl, riid, pp);\n    bool warp = adapter_is_warp(adapter);\n    rgpu_log(\"ID3D12DeviceFactory::CreateDevice fl=0x%X adapter=%s -> hr=0x%08lX device=%p\",\n             (unsigned)fl, warp ? \"WARP\" : \"hardware\", (unsigned long)hr, pp ? *pp : nullptr);\n    if (SUCCEEDED(hr) && pp && *pp && !warp) arm_from_device((ID3D12Device *)*pp, \"DeviceFactory\");\n    return hr;\n}\n\ntypedef HRESULT (STDMETHODCALLTYPE *fnSDKCreateDeviceFactory)(ID3D12SDKConfiguration1 *, UINT, LPCSTR, REFIID, void **);\nstatic fnSDKCreateDeviceFactory o_SDKCreateDeviceFactory;\nstatic void *g_hooked_sdkconfig_vtbl = nullptr;\n\nstatic HRESULT STDMETHODCALLTYPE h_SDKCreateDeviceFactory(ID3D12SDKConfiguration1 *This, UINT sdkVersion,\n                                                          LPCSTR sdkPath, REFIID riid, void **ppFactory) {\n    InterlockedIncrement(&g_sdk_factory_calls);\n    HRESULT hr = o_SDKCreateDeviceFactory(This, sdkVersion, sdkPath, riid, ppFactory);\n    rgpu_log(\"ID3D12SDKConfiguration1::CreateDeviceFactory sdk=%u path=\\\"%s\\\" -> hr=0x%08lX factory=%p\",\n             sdkVersion, sdkPath ? sdkPath : \"(null)\", (unsigned long)hr,\n             ppFactory ? *ppFactory : nullptr);\n    if (SUCCEEDED(hr) && ppFactory && *ppFactory) hook_factory_object(*ppFactory);\n    return hr;\n}\n\nstatic bool hook_sdk_configuration(void *obj) {\n    if (!obj) return false;\n    ID3D12SDKConfiguration1 *sdk = nullptr;\n    if (FAILED(((IUnknown *)obj)->QueryInterface(__uuidof(ID3D12SDKConfiguration1), (void **)&sdk)) || !sdk)\n        return false;\n    void *vtbl = *(void **)sdk;\n    bool ok = true;\n    if (vtbl != g_hooked_sdkconfig_vtbl) {\n        ok = patch_slot(sdk, RGPU_SLOT_SDKConfig1_CreateDeviceFactory,\n                        (void *)h_SDKCreateDeviceFactory, (void **)&o_SDKCreateDeviceFactory);\n        if (ok) {\n            g_hooked_sdkconfig_vtbl = vtbl;\n            InterlockedIncrement(&g_sdk_config_hooks);\n            rgpu_log(\"hooked ID3D12SDKConfiguration1::CreateDeviceFactory sdkconfig=%p vtbl=%p\",\n                     (void *)sdk, vtbl);\n        }\n    }\n    sdk->Release();\n    return ok;\n}\n""",
        "factory and SDKConfiguration1 hooks",
    )

    proxy = replace_once(
        proxy,
        """    HRESULT hr = fn(rclsid, riid, ppvDebug);\n    if (SUCCEEDED(hr) && ppvDebug && *ppvDebug && riid == __uuidof(ID3D12DeviceFactory)) {\n        if (InterlockedCompareExchange(&g_hooked_factory, 1, 0) == 0 &&\n            patch_slot(*ppvDebug, RGPU_SLOT_Factory_CreateDevice, (void *)h_FactoryCreateDevice, (void **)&o_FactoryCreateDevice))\n            rgpu_log(\"hooked ID3D12DeviceFactory::CreateDevice (Agility real-device path)\");\n    }\n    return hr;\n}\n""",
        """    HRESULT hr = fn(rclsid, riid, ppvDebug);\n    if (SUCCEEDED(hr) && ppvDebug && *ppvDebug) {\n        /* The normal Agility path is:\n         * D3D12GetInterface(CLSID_D3D12SDKConfiguration, IID_ID3D12SDKConfiguration1)\n         *   -> ID3D12SDKConfiguration1::CreateDeviceFactory\n         *   -> ID3D12DeviceFactory::CreateDevice.\n         * Hook the intermediate SDKConfiguration1 object; checking only for a factory\n         * directly from D3D12GetInterface misses this documented path. */\n        if (IsEqualCLSID(rclsid, CLSID_D3D12SDKConfiguration))\n            hook_sdk_configuration(*ppvDebug);\n\n        /* Retain support for runtimes/tools that request a factory directly. */\n        hook_factory_object(*ppvDebug);\n    }\n    return hr;\n}\n\n__declspec(dllexport) void WINAPI rgpu_d3d12_path_stats(long *sdkConfigHooks, long *sdkFactoryCalls,\n                                                        long *factoryDeviceCalls) {\n    if (sdkConfigHooks) *sdkConfigHooks = g_sdk_config_hooks;\n    if (sdkFactoryCalls) *sdkFactoryCalls = g_sdk_factory_calls;\n    if (factoryDeviceCalls) *factoryDeviceCalls = g_factory_device_calls;\n}\n""",
        "D3D12GetInterface implementation",
    )
    write_crlf(PROXY, proxy)

slots = read_normalized(SLOTS)
if "RGPU_SLOT_SDKConfig1_CreateDeviceFactory" not in slots:
    slots = replace_once(
        slots,
        "unsigned RGPU_SLOT_Factory_CreateDevice      = SLOT(ID3D12DeviceFactoryVtbl, CreateDevice);\n",
        "unsigned RGPU_SLOT_Factory_CreateDevice      = SLOT(ID3D12DeviceFactoryVtbl, CreateDevice);\n"
        "unsigned RGPU_SLOT_SDKConfig1_CreateDeviceFactory = SLOT(ID3D12SDKConfiguration1Vtbl, CreateDeviceFactory);\n",
        "SDKConfiguration1 vtable slot",
    )
    write_crlf(SLOTS, slots)

harness = read_normalized(HARNESS)
if "pPathStats" not in harness:
    harness = replace_once(
        harness,
        """typedef HRESULT (WINAPI *pCreate)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);\ntypedef void    (WINAPI *pStats)(long *, long *, long *, long *, long *, long *, long *, unsigned *);\n""",
        """typedef HRESULT (WINAPI *pCreate)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);\ntypedef HRESULT (WINAPI *pGetInterface)(REFCLSID, REFIID, void **);\ntypedef void    (WINAPI *pStats)(long *, long *, long *, long *, long *, long *, long *, unsigned *);\ntypedef void    (WINAPI *pPathStats)(long *, long *, long *);\n""",
        "harness typedefs",
    )

    harness = replace_once(
        harness,
        """    pCreate CreateDevice = (pCreate)GetProcAddress(dll, \"D3D12CreateDevice\");\n    pStats  Stats        = (pStats)GetProcAddress(dll, \"rgpu_d3d12_stats\");\n    if (!CreateDevice || !Stats) { std::printf(\"  FAIL: proxy missing exports\\n\"); return 1; }\n""",
        """    pCreate       CreateDevice = (pCreate)GetProcAddress(dll, \"D3D12CreateDevice\");\n    pGetInterface GetInterface = (pGetInterface)GetProcAddress(dll, \"D3D12GetInterface\");\n    pStats        Stats        = (pStats)GetProcAddress(dll, \"rgpu_d3d12_stats\");\n    pPathStats    PathStats    = (pPathStats)GetProcAddress(dll, \"rgpu_d3d12_path_stats\");\n    if (!CreateDevice || !GetInterface || !Stats || !PathStats) { std::printf(\"  FAIL: proxy missing exports\\n\"); return 1; }\n\n    /* Exercise the documented Agility discovery route. The deliberately invalid\n     * factory request can fail; the SDKConfiguration1 detour must still fire. */\n    ID3D12SDKConfiguration1 *sdkConfig = nullptr;\n    HRESULT sdkHr = GetInterface(CLSID_D3D12SDKConfiguration, __uuidof(ID3D12SDKConfiguration1), (void **)&sdkConfig);\n    if (SUCCEEDED(sdkHr) && sdkConfig) {\n        ID3D12DeviceFactory *probeFactory = nullptr;\n        HRESULT factoryHr = sdkConfig->CreateDeviceFactory(0, \"\", __uuidof(ID3D12DeviceFactory), (void **)&probeFactory);\n        std::printf(\"  SDKConfiguration1 path exercised: CreateDeviceFactory returned 0x%08lX\\n\", (unsigned long)factoryHr);\n        if (probeFactory) probeFactory->Release();\n        sdkConfig->Release();\n    } else {\n        std::printf(\"  FAIL: D3D12GetInterface SDKConfiguration1 0x%08lX\\n\", (unsigned long)sdkHr);\n        return 1;\n    }\n""",
        "harness SDKConfiguration1 path",
    )

    harness = replace_once(
        harness,
        """    long execs=0, lists_n=0, draws=0, dispatch=0, barriers=0, clears=0, copies=0; unsigned bytes=0;\n    Stats(&execs, &lists_n, &draws, &dispatch, &barriers, &clears, &copies, &bytes);\n    std::printf(\"  tee stats: execs=%ld lists=%ld draws=%ld barriers=%ld clears=%ld copies=%ld | batch=%u bytes\\n\",\n                execs, lists_n, draws, barriers, clears, copies, bytes);\n\n    int ok = execs >= 1 && barriers >= 1 && clears >= 1 && draws >= 1 && bytes > 0;\n""",
        """    long execs=0, lists_n=0, draws=0, dispatch=0, barriers=0, clears=0, copies=0; unsigned bytes=0;\n    Stats(&execs, &lists_n, &draws, &dispatch, &barriers, &clears, &copies, &bytes);\n    long sdkHooks=0, sdkFactoryCalls=0, factoryDeviceCalls=0;\n    PathStats(&sdkHooks, &sdkFactoryCalls, &factoryDeviceCalls);\n    std::printf(\"  tee stats: execs=%ld lists=%ld draws=%ld barriers=%ld clears=%ld copies=%ld | batch=%u bytes\\n\",\n                execs, lists_n, draws, barriers, clears, copies, bytes);\n    std::printf(\"  agility path: sdk-hooks=%ld create-factory-calls=%ld factory-device-calls=%ld\\n\",\n                sdkHooks, sdkFactoryCalls, factoryDeviceCalls);\n\n    int ok = execs >= 1 && barriers >= 1 && clears >= 1 && draws >= 1 && bytes > 0 &&\n             sdkHooks >= 1 && sdkFactoryCalls >= 1;\n""",
        "harness acceptance criteria",
    )
    write_crlf(HARNESS, harness)

print("Applied SDKConfiguration1 -> DeviceFactory interception fix.")
