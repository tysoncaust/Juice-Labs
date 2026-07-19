/* rgpu synthetic DXGI adapter + fail-closed policy (Phase-1 interception proof).
 *
 * Presents ONE synthetic IDXGIAdapter1 ("Remote GPU - <name>") describing the
 * negotiated remote GPU, and a synthetic IDXGIFactory1 whose EnumAdapters* return
 * ONLY that adapter. The real game's pre-launch checks (vendor, VRAM, feature
 * level, adapter enumeration) then see the remote GPU, never local hardware.
 *
 * Fail-closed policy is enforced here: the proxy never creates a local hardware
 * device, never falls back to WARP, and (debug) aborts if an unproxied hardware
 * device is created. Counters prove local=0 / remote=1.
 */
#ifndef RGPU_SYNTHETIC_H
#define RGPU_SYNTHETIC_H
#include <dxgi.h>
#include <dxgi1_2.h>
#include <d3d11.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Negotiated caps that the synthetic adapter advertises (set from CAPS_OFFER). */
typedef struct {
    wchar_t  description[128];
    unsigned vendor_id;
    unsigned device_id;
    uint64_t dedicated_vram;   /* bytes */
    unsigned feature_level;    /* D3D_FEATURE_LEVEL_11_0 / _11_1 */
    unsigned location_remote;  /* always 1 */
} rgpu_caps;

/* Fail-closed policy (all enforced by the proxy). */
typedef struct {
    int remote_required;          /* 1 */
    int local_hardware_fallback;  /* 0 */
    int warp_game_fallback;       /* 0 */
    int debug_abort_on_local_hw;  /* 1 in debug */
} rgpu_policy;

extern rgpu_policy   g_rgpu_policy;
extern volatile long g_local_hw_devices_created;   /* invariant: MUST stay 0 */
extern volatile long g_local_hw_attempts_refused;  /* caught + refused local/WARP attempts */
extern volatile long g_remote_devices_created;
extern int           g_rgpu_remote_connected;      /* set when renderer session is live */

void      rgpu_set_caps(const rgpu_caps *caps);
rgpu_caps rgpu_default_caps(void);   /* "Remote GPU - NVIDIA T4", 15360MB, 11_1 */

/* Called on any attempt to create a LOCAL hardware device. Fail closed. */
void rgpu_guard_local_hardware(const char *where);

/* Proxy entrypoints (what the game's dxgi/d3d11 calls are routed to). Real
 * builds export these as CreateDXGIFactory / D3D11CreateDevice replacements. */
HRESULT rgpu_CreateDXGIFactory1(REFIID riid, void **ppFactory);
HRESULT rgpu_CreateDXGIFactory(REFIID riid, void **ppFactory);
HRESULT rgpu_D3D11CreateDevice(
    IDXGIAdapter *pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software,
    UINT Flags, const D3D_FEATURE_LEVEL *pFeatureLevels, UINT FeatureLevels,
    UINT SDKVersion, ID3D11Device **ppDevice, D3D_FEATURE_LEVEL *pFeatureLevel,
    ID3D11DeviceContext **ppImmediateContext);

#ifdef __cplusplus
}
#endif
#endif /* RGPU_SYNTHETIC_H */
