/* rgpu serializing D3D11 front-end (device/context shape).
 *
 * The methods here mirror the D3D11 calls a game makes and, instead of touching
 * a local GPU, SERIALIZE each into the rgpu protocol command stream that flows
 * over the transport to the remote renderer. This is the serialization core the
 * game-facing ID3D11Device/ID3D11DeviceContext COM object forwards to.
 *
 * State: the covered subset (create render-target texture, clear, present) is
 * implemented + tested end-to-end (serialize -> renderer -> verified frame).
 * Wrapping this in the full ID3D11Device/ID3D11DeviceContext COM vtable (~150
 * methods) so a QueryInterface'd real game lands here — plus a d3d11.dll proxy
 * to inject it — is the same pattern as rgpu_synthetic's IDXGIFactory1 and is
 * the mechanical remaining step. */
#ifndef RGPU_SERIALIZER_H
#define RGPU_SERIALIZER_H
#include "../../proto/rgpu_cmds.h"
#include <cstdint>
#include <vector>

class RgpuDevice;

class RgpuContext {
public:
    explicit RgpuContext(RgpuDevice *dev) : dev_(dev) {}
    void ClearRenderTargetView(uint32_t rtv_handle, const float rgba[4]); // ID3D11DeviceContext
    void Present(uint32_t frame_handle);                                  // swapchain present
private:
    RgpuDevice *dev_;
};

class RgpuDevice {
public:
    RgpuDevice() : ctx_(this) {}
    uint32_t CreateTexture2D(uint32_t width, uint32_t height, uint32_t dxgi_format); // ID3D11Device
    uint32_t CreateRenderTargetView(uint32_t texture_handle);                        // ID3D11Device
    RgpuContext *GetImmediateContext() { return &ctx_; }
    std::vector<unsigned char> BuildBatch(uint32_t session = 1, uint32_t sequence = 1) const;
    uint32_t commands_recorded() const { return count_; }
    void record(uint32_t op, uint32_t handle, const void *args, uint32_t n); // serialization sink
private:
    std::vector<unsigned char> body_;
    RgpuContext ctx_;
    uint32_t next_handle_ = 1;
    uint32_t count_ = 0;
};
#endif
