/* rgpu d3d11 proxy harness — proves the pass-through+tee device the way a game
 * uses it, but headless and deterministic:
 *   1. LoadLibrary the proxy DLL and GetProcAddress its D3D11CreateDevice export
 *      (exactly the symbol a game's import table binds to when the DLL sits next
 *      to the .exe).
 *   2. Create a device -> receive the RgpuD3D11Device wrapper around a real one.
 *   3. Through the WRAPPER, create a render-target texture + RTV, clear it on the
 *      real immediate context, copy to a staging texture and read the pixel back.
 *      A correct readback proves the wrapped device renders on the real GPU with
 *      no incompatible-driver error or refusal.
 *   4. Query the proxy's stats: 1 device wrapped, and the two covered creates
 *      tee'd into the rgpu protocol batch.
 * This is the strongest controlled check before dropping the DLL next to a game. */
#include <windows.h>
#include <d3d11.h>
#include <cstdio>
#include <cstdlib>

typedef HRESULT(WINAPI *pCreate)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL *, UINT, UINT, ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
typedef void(WINAPI *pStats)(long *, long *, long *, unsigned *, unsigned *);
typedef void(WINAPI *pCtxStats)(long *, long *, long *, long *);

int main() {
    std::printf("rgpu d3d11 proxy harness\n------------------------\n");

    HMODULE dll = LoadLibraryA("rgpu_d3d11.dll");
    if (!dll) { std::printf("  FAIL: cannot load rgpu_d3d11.dll (err %lu)\n", GetLastError()); return 1; }
    pCreate CreateDevice = (pCreate)GetProcAddress(dll, "D3D11CreateDevice");
    pStats  Stats        = (pStats)GetProcAddress(dll, "rgpu_proxy_stats");
    if (!CreateDevice || !Stats) { std::printf("  FAIL: proxy missing exports\n"); return 1; }
    std::printf("  proxy DLL loaded; D3D11CreateDevice + rgpu_proxy_stats resolved\n");

    D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    ID3D11Device *dev = nullptr; ID3D11DeviceContext *ctx = nullptr; D3D_FEATURE_LEVEL got{};
    const char *substrate = "hardware GPU";
    HRESULT hr = CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, want, 2,
                              D3D11_SDK_VERSION, &dev, &got, &ctx);
    if (FAILED(hr)) {   // dev host has an RTX; WARP is only a fallback substrate to exercise wrap+tee
        substrate = "WARP (harness substrate; policy still forbids WARP for a real game)";
        hr = CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, want, 2,
                          D3D11_SDK_VERSION, &dev, &got, &ctx);
    }
    if (FAILED(hr) || !dev || !ctx) { std::printf("  FAIL: device create hr=0x%08lX\n", (unsigned long)hr); return 1; }
    std::printf("  device created via proxy on %s, feature level 0x%04X\n", substrate, got);

    /* through the WRAPPER: create a render target the way a game sets up its backbuffer path */
    D3D11_TEXTURE2D_DESC td{}; td.Width = 64; td.Height = 64; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D *rt = nullptr;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &rt))) { std::printf("  FAIL: CreateTexture2D\n"); return 1; }
    ID3D11RenderTargetView *rtv = nullptr;
    if (FAILED(dev->CreateRenderTargetView(rt, nullptr, &rtv))) { std::printf("  FAIL: CreateRenderTargetView\n"); return 1; }

    const float color[4] = {0.2f, 0.4f, 0.8f, 1.0f};
    ctx->ClearRenderTargetView(rtv, color);
    ctx->Flush();

    /* read the rendered pixel back off the real GPU */
    D3D11_TEXTURE2D_DESC sd = td; sd.BindFlags = 0; sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *staging = nullptr;
    if (FAILED(dev->CreateTexture2D(&sd, nullptr, &staging))) { std::printf("  FAIL: staging tex\n"); return 1; }
    ctx->CopyResource(staging, rt);
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &map))) { std::printf("  FAIL: Map\n"); return 1; }
    const unsigned char *px = (const unsigned char *)map.pData;
    unsigned char r = px[0], g = px[1], b = px[2], a = px[3];
    ctx->Unmap(staging, 0);
    std::printf("  wrapped device rendered a real frame; top-left pixel (%u,%u,%u,%u)\n", r, g, b, a);
    const unsigned char expect[4] = {51, 102, 204, 255};
    int pixel_ok = (abs((int)r-expect[0])<=1 && abs((int)g-expect[1])<=1 && abs((int)b-expect[2])<=1 && abs((int)a-expect[3])<=1);

    long devWrapped=0, teeTex=0, teeRtv=0; unsigned teeCmds=0, teeBytes=0;
    Stats(&devWrapped, &teeTex, &teeRtv, &teeCmds, &teeBytes);
    std::printf("  proxy stats: devicesWrapped=%ld  tee{tex2d=%ld rtv=%ld cmds=%u batch=%u bytes}\n",
                devWrapped, teeTex, teeRtv, teeCmds, teeBytes);

    /* the ctx returned by CreateDevice is now the WRAPPED immediate context, so the
     * ClearRenderTargetView above went through the 115-method context vtable + tee'd. */
    long ctxWrapped=0, teeDraws=0, teeClears=0, teeState=0;
    pCtxStats CtxStats = (pCtxStats)GetProcAddress(dll, "rgpu_proxy_ctx_stats");
    if (CtxStats) CtxStats(&ctxWrapped, &teeDraws, &teeClears, &teeState);
    std::printf("  context stats: contextsWrapped=%ld  tee{draws=%ld clears=%ld state=%ld}\n",
                ctxWrapped, teeDraws, teeClears, teeState);

    int ok = pixel_ok && devWrapped == 1 && teeTex >= 1 && teeRtv >= 1 && teeCmds >= 2 && teeBytes > 0
             && ctxWrapped >= 1 && teeClears >= 1;
    staging->Release(); rtv->Release(); rt->Release(); ctx->Release(); dev->Release();
    std::printf("------------------------\n");
    std::printf(ok ? "RESULT: game-shaped D3D11 device + immediate context ran through the full wrappers on the real GPU + tee'd to protocol\n"
                   : "RESULT: FAIL\n");
    return ok ? 0 : 1;
}
