/* rgpu client-serialization proof: drive the serializing D3D11 front-end the way
 * a game does (create RT texture -> RTV -> clear -> present), take the emitted
 * rgpu protocol batch, run it on the real reference renderer, and verify the
 * frame. Proves: game-shaped D3D11 calls serialize into the protocol and render
 * to a real frame (no local device used by the client itself). */
#include "../src/rgpu_serializer.h"
#include "../../renderd/win/rgpu_renderd_win.h"
#include <cstdio>
#include <cstdlib>
#include <string>

int main() {
    std::printf("rgpu client-serialization proof\n-------------------------------\n");

    RgpuDevice dev;                                  // "ID3D11Device"
    uint32_t tex = dev.CreateTexture2D(64, 64, 28);  // DXGI_FORMAT_R8G8B8A8_UNORM
    uint32_t rtv = dev.CreateRenderTargetView(tex);
    RgpuContext *ctx = dev.GetImmediateContext();    // "ID3D11DeviceContext"
    const float color[4] = {0.2f, 0.4f, 0.8f, 1.0f};
    ctx->ClearRenderTargetView(rtv, color);
    ctx->Present(tex);

    std::vector<unsigned char> batch = dev.BuildBatch();
    std::printf("  serialized %u D3D11 calls -> %zu-byte protocol batch\n",
                dev.commands_recorded(), batch.size());

    RgpuFrame frame; std::string err;
    if (!rgpu_render_batch(batch.data(), (uint32_t)batch.size(), frame, err)) {
        std::printf("  FAIL: renderer: %s\n", err.c_str()); return 1;
    }
    const uint8_t expect[4] = {51, 102, 204, 255};
    int ok = frame.width == 64 && frame.height == 64;
    for (int c = 0; c < 4 && ok; ++c) ok = std::abs((int)frame.rgba[c] - expect[c]) <= 1;
    std::printf("  frame %ux%u, top-left pixel (%d,%d,%d,%d)\n", frame.width, frame.height,
                frame.rgba[0], frame.rgba[1], frame.rgba[2], frame.rgba[3]);

    std::printf("-------------------------------\n");
    std::printf(ok ? "RESULT: game-shaped D3D11 calls serialized + rendered correctly\n"
                   : "RESULT: FAIL\n");
    return ok ? 0 : 1;
}
