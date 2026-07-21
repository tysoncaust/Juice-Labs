$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$proxyPath = Join-Path $here 'src\rgpu_d3d12_proxy.cpp'
$slotsPath = Join-Path $here 'src\rgpu_d3d12_slots.cpp'
$harnessPath = Join-Path $here 'test\rgpu_d3d12_harness.cpp'

function Replace-Exact([string]$Text, [string]$Old, [string]$New, [string]$Label) {
    if (-not $Text.Contains($Old)) {
        throw "Expected source fragment not found: $Label"
    }
    return $Text.Replace($Old, $New)
}

foreach ($path in @($proxyPath, $slotsPath, $harnessPath)) {
    Copy-Item $path "$path.bak-sdkconfig1" -Force
}

$proxy = [IO.File]::ReadAllText($proxyPath)
if ($proxy.Contains('RGPU_SLOT_SDKConfig1_CreateDeviceFactory')) {
    Write-Host 'SDKConfiguration1 fix already present in proxy; leaving source unchanged.'
} else {
    $proxy = Replace-Exact $proxy @'
    RGPU_SLOT_GCL_ExecuteIndirect, RGPU_SLOT_GCL6_DispatchMesh, RGPU_SLOT_GCL7_Barrier,
    RGPU_SLOT_Device_CheckFeatureSupport;
'@ @'
    RGPU_SLOT_GCL_ExecuteIndirect, RGPU_SLOT_GCL6_DispatchMesh, RGPU_SLOT_GCL7_Barrier,
    RGPU_SLOT_Device_CheckFeatureSupport, RGPU_SLOT_SDKConfig1_CreateDeviceFactory;
'@ 'extern slot declaration'

    $proxy = Replace-Exact $proxy @'
static fnFactoryCreateDevice o_FactoryCreateDevice;
static volatile LONG g_hooked_factory = 0;

static HRESULT STDMETHODCALLTYPE h_FactoryCreateDevice(ID3D12DeviceFactory *This, IUnknown *adapter, D3D_FEATURE_LEVEL fl, REFIID riid, void **pp) {
    HRESULT hr = o_FactoryCreateDevice(This, adapter, fl, riid, pp);
    bool warp = adapter_is_warp(adapter);
    rgpu_log("ID3D12DeviceFactory::CreateDevice fl=0x%X adapter=%s -> hr=0x%08lX device=%p",
             (unsigned)fl, warp ? "WARP" : "hardware", (unsigned long)hr, pp ? *pp : nullptr);
    if (SUCCEEDED(hr) && pp && *pp && !warp) arm_from_device((ID3D12Device *)*pp, "DeviceFactory");
    return hr;
}
'@ @'
static fnFactoryCreateDevice o_FactoryCreateDevice;
static void *g_hooked_factory_vtbl = nullptr;
static volatile LONG g_sdk_config_hooks = 0;
static volatile LONG g_sdk_factory_calls = 0;
static volatile LONG g_factory_device_calls = 0;

static bool hook_factory_object(void *obj) {
    if (!obj) return false;
    ID3D12DeviceFactory *factory = nullptr;
    if (FAILED(((IUnknown *)obj)->QueryInterface(__uuidof(ID3D12DeviceFactory), (void **)&factory)) || !factory)
        return false;
    void *vtbl = *(void **)factory;
    bool ok = true;
    if (vtbl != g_hooked_factory_vtbl) {
        ok = patch_slot(factory, RGPU_SLOT_Factory_CreateDevice, (void *)h_FactoryCreateDevice, (void **)&o_FactoryCreateDevice);
        if (ok) {
            g_hooked_factory_vtbl = vtbl;
            rgpu_log("hooked ID3D12DeviceFactory::CreateDevice factory=%p vtbl=%p", (void *)factory, vtbl);
        }
    }
    factory->Release();
    return ok;
}

static HRESULT STDMETHODCALLTYPE h_FactoryCreateDevice(ID3D12DeviceFactory *This, IUnknown *adapter, D3D_FEATURE_LEVEL fl, REFIID riid, void **pp) {
    InterlockedIncrement(&g_factory_device_calls);
    HRESULT hr = o_FactoryCreateDevice(This, adapter, fl, riid, pp);
    bool warp = adapter_is_warp(adapter);
    rgpu_log("ID3D12DeviceFactory::CreateDevice fl=0x%X adapter=%s -> hr=0x%08lX device=%p",
             (unsigned)fl, warp ? "WARP" : "hardware", (unsigned long)hr, pp ? *pp : nullptr);
    if (SUCCEEDED(hr) && pp && *pp && !warp) arm_from_device((ID3D12Device *)*pp, "DeviceFactory");
    return hr;
}

typedef HRESULT (STDMETHODCALLTYPE *fnSDKCreateDeviceFactory)(ID3D12SDKConfiguration1 *, UINT, LPCSTR, REFIID, void **);
static fnSDKCreateDeviceFactory o_SDKCreateDeviceFactory;
static void *g_hooked_sdkconfig_vtbl = nullptr;

static HRESULT STDMETHODCALLTYPE h_SDKCreateDeviceFactory(ID3D12SDKConfiguration1 *This, UINT sdkVersion,
                                                          LPCSTR sdkPath, REFIID riid, void **ppFactory) {
    InterlockedIncrement(&g_sdk_factory_calls);
    HRESULT hr = o_SDKCreateDeviceFactory(This, sdkVersion, sdkPath, riid, ppFactory);
    rgpu_log("ID3D12SDKConfiguration1::CreateDeviceFactory sdk=%u path=\"%s\" -> hr=0x%08lX factory=%p",
             sdkVersion, sdkPath ? sdkPath : "(null)", (unsigned long)hr,
             ppFactory ? *ppFactory : nullptr);
    if (SUCCEEDED(hr) && ppFactory && *ppFactory) hook_factory_object(*ppFactory);
    return hr;
}

static bool hook_sdk_configuration(void *obj) {
    if (!obj) return false;
    ID3D12SDKConfiguration1 *sdk = nullptr;
    if (FAILED(((IUnknown *)obj)->QueryInterface(__uuidof(ID3D12SDKConfiguration1), (void **)&sdk)) || !sdk)
        return false;
    void *vtbl = *(void **)sdk;
    bool ok = true;
    if (vtbl != g_hooked_sdkconfig_vtbl) {
        ok = patch_slot(sdk, RGPU_SLOT_SDKConfig1_CreateDeviceFactory,
                        (void *)h_SDKCreateDeviceFactory, (void **)&o_SDKCreateDeviceFactory);
        if (ok) {
            g_hooked_sdkconfig_vtbl = vtbl;
            InterlockedIncrement(&g_sdk_config_hooks);
            rgpu_log("hooked ID3D12SDKConfiguration1::CreateDeviceFactory sdkconfig=%p vtbl=%p", (void *)sdk, vtbl);
        }
    }
    sdk->Release();
    return ok;
}
'@ 'factory and SDKConfiguration1 hooks'

    $proxy = Replace-Exact $proxy @'
    HRESULT hr = fn(rclsid, riid, ppvDebug);
    if (SUCCEEDED(hr) && ppvDebug && *ppvDebug && riid == __uuidof(ID3D12DeviceFactory)) {
        if (InterlockedCompareExchange(&g_hooked_factory, 1, 0) == 0 &&
            patch_slot(*ppvDebug, RGPU_SLOT_Factory_CreateDevice, (void *)h_FactoryCreateDevice, (void **)&o_FactoryCreateDevice))
            rgpu_log("hooked ID3D12DeviceFactory::CreateDevice (Agility real-device path)");
    }
    return hr;
}
'@ @'
    HRESULT hr = fn(rclsid, riid, ppvDebug);
    if (SUCCEEDED(hr) && ppvDebug && *ppvDebug) {
        /* The normal Agility path is:
         * D3D12GetInterface(CLSID_D3D12SDKConfiguration, IID_ID3D12SDKConfiguration1)
         *   -> ID3D12SDKConfiguration1::CreateDeviceFactory
         *   -> ID3D12DeviceFactory::CreateDevice.
         * Hook the intermediate SDKConfiguration1 object; checking only for a factory
         * directly from D3D12GetInterface misses this documented path. */
        if (IsEqualCLSID(rclsid, CLSID_D3D12SDKConfiguration))
            hook_sdk_configuration(*ppvDebug);

        /* Retain support for runtimes/tools that request a factory directly. */
        hook_factory_object(*ppvDebug);
    }
    return hr;
}

__declspec(dllexport) void WINAPI rgpu_d3d12_path_stats(long *sdkConfigHooks, long *sdkFactoryCalls,
                                                        long *factoryDeviceCalls) {
    if (sdkConfigHooks) *sdkConfigHooks = g_sdk_config_hooks;
    if (sdkFactoryCalls) *sdkFactoryCalls = g_sdk_factory_calls;
    if (factoryDeviceCalls) *factoryDeviceCalls = g_factory_device_calls;
}
'@ 'D3D12GetInterface implementation'

    [IO.File]::WriteAllText($proxyPath, $proxy, [Text.UTF8Encoding]::new($false))
}

$slots = [IO.File]::ReadAllText($slotsPath)
if (-not $slots.Contains('RGPU_SLOT_SDKConfig1_CreateDeviceFactory')) {
    $slots = Replace-Exact $slots @'
unsigned RGPU_SLOT_Factory_CreateDevice      = SLOT(ID3D12DeviceFactoryVtbl, CreateDevice);
'@ @'
unsigned RGPU_SLOT_Factory_CreateDevice      = SLOT(ID3D12DeviceFactoryVtbl, CreateDevice);
unsigned RGPU_SLOT_SDKConfig1_CreateDeviceFactory = SLOT(ID3D12SDKConfiguration1Vtbl, CreateDeviceFactory);
'@ 'SDKConfiguration1 vtable slot'
    [IO.File]::WriteAllText($slotsPath, $slots, [Text.UTF8Encoding]::new($false))
}

$harness = [IO.File]::ReadAllText($harnessPath)
if (-not $harness.Contains('pPathStats')) {
    $harness = Replace-Exact $harness @'
typedef HRESULT (WINAPI *pCreate)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
typedef void    (WINAPI *pStats)(long *, long *, long *, long *, long *, long *, long *, unsigned *);
'@ @'
typedef HRESULT (WINAPI *pCreate)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
typedef HRESULT (WINAPI *pGetInterface)(REFCLSID, REFIID, void **);
typedef void    (WINAPI *pStats)(long *, long *, long *, long *, long *, long *, long *, unsigned *);
typedef void    (WINAPI *pPathStats)(long *, long *, long *);
'@ 'harness typedefs'

    $harness = Replace-Exact $harness @'
    pCreate CreateDevice = (pCreate)GetProcAddress(dll, "D3D12CreateDevice");
    pStats  Stats        = (pStats)GetProcAddress(dll, "rgpu_d3d12_stats");
    if (!CreateDevice || !Stats) { std::printf("  FAIL: proxy missing exports\n"); return 1; }
'@ @'
    pCreate       CreateDevice = (pCreate)GetProcAddress(dll, "D3D12CreateDevice");
    pGetInterface GetInterface = (pGetInterface)GetProcAddress(dll, "D3D12GetInterface");
    pStats        Stats        = (pStats)GetProcAddress(dll, "rgpu_d3d12_stats");
    pPathStats    PathStats    = (pPathStats)GetProcAddress(dll, "rgpu_d3d12_path_stats");
    if (!CreateDevice || !GetInterface || !Stats || !PathStats) { std::printf("  FAIL: proxy missing exports\n"); return 1; }

    /* Exercise the actual Agility discovery route. Even if this deliberately invalid
     * factory request fails, the SDKConfiguration1 vtable detour must fire. */
    ID3D12SDKConfiguration1 *sdkConfig = nullptr;
    HRESULT sdkHr = GetInterface(CLSID_D3D12SDKConfiguration, __uuidof(ID3D12SDKConfiguration1), (void **)&sdkConfig);
    if (SUCCEEDED(sdkHr) && sdkConfig) {
        ID3D12DeviceFactory *probeFactory = nullptr;
        HRESULT factoryHr = sdkConfig->CreateDeviceFactory(0, "", __uuidof(ID3D12DeviceFactory), (void **)&probeFactory);
        std::printf("  SDKConfiguration1 path exercised: CreateDeviceFactory returned 0x%08lX\n", (unsigned long)factoryHr);
        if (probeFactory) probeFactory->Release();
        sdkConfig->Release();
    } else {
        std::printf("  FAIL: D3D12GetInterface SDKConfiguration1 0x%08lX\n", (unsigned long)sdkHr);
        return 1;
    }
'@ 'harness SDKConfiguration1 path'

    $harness = Replace-Exact $harness @'
    long execs=0, lists_n=0, draws=0, dispatch=0, barriers=0, clears=0, copies=0; unsigned bytes=0;
    Stats(&execs, &lists_n, &draws, &dispatch, &barriers, &clears, &copies, &bytes);
    std::printf("  tee stats: execs=%ld lists=%ld draws=%ld barriers=%ld clears=%ld copies=%ld | batch=%u bytes\n",
                execs, lists_n, draws, barriers, clears, copies, bytes);

    int ok = execs >= 1 && barriers >= 1 && clears >= 1 && draws >= 1 && bytes > 0;
'@ @'
    long execs=0, lists_n=0, draws=0, dispatch=0, barriers=0, clears=0, copies=0; unsigned bytes=0;
    Stats(&execs, &lists_n, &draws, &dispatch, &barriers, &clears, &copies, &bytes);
    long sdkHooks=0, sdkFactoryCalls=0, factoryDeviceCalls=0;
    PathStats(&sdkHooks, &sdkFactoryCalls, &factoryDeviceCalls);
    std::printf("  tee stats: execs=%ld lists=%ld draws=%ld barriers=%ld clears=%ld copies=%ld | batch=%u bytes\n",
                execs, lists_n, draws, barriers, clears, copies, bytes);
    std::printf("  agility path: sdk-hooks=%ld create-factory-calls=%ld factory-device-calls=%ld\n",
                sdkHooks, sdkFactoryCalls, factoryDeviceCalls);

    int ok = execs >= 1 && barriers >= 1 && clears >= 1 && draws >= 1 && bytes > 0 &&
             sdkHooks >= 1 && sdkFactoryCalls >= 1;
'@ 'harness acceptance criteria'

    [IO.File]::WriteAllText($harnessPath, $harness, [Text.UTF8Encoding]::new($false))
}

Write-Host 'Applied SDKConfiguration1 -> DeviceFactory interception fix.'
