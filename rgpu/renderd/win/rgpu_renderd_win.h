/* rgpu-renderd-win — host D3D11 reference renderer.
 *
 * Consumes an rgpu command batch and executes it on a REAL local D3D11 device.
 * This is the renderer side; it is SUPPOSED to use the physical GPU (the game
 * client is the side that must not). Phase-1/2 reference for the protocol; the
 * Colab production backend (rgpu-renderd-linux) implements the same protocol on
 * Vulkan. Present -> the target texture is read back as an RGBA frame. */
#ifndef RGPU_RENDERD_WIN_H
#define RGPU_RENDERD_WIN_H
#include <cstdint>
#include <vector>
#include <string>

struct RgpuFrame {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba; // width*height*4, row-major
};

/* Execute one COMMAND_BATCH payload. Returns true on success; on PRESENT the
 * presented texture is read back into `out`. Renderer owns a persistent D3D11
 * device across calls. */
bool rgpu_render_batch(const uint8_t *batch, uint32_t len, RgpuFrame &out, std::string &err);

/* Was a real local D3D11 device created by the RENDERER? (Expected on the host
 * reference renderer; distinct from the client's local-device invariant.) */
int rgpu_renderd_local_device_created();

#endif
