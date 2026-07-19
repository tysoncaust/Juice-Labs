/* rgpu Phase-2 loopback proof: record an rgpu command batch (create render
 * target, clear to a known color, present), execute it on the real host D3D11
 * renderer, and verify the returned frame's pixels — i.e. a frame genuinely
 * produced by going THROUGH the protocol, not by direct local calls. */
#include "rgpu_renderd_win.h"
#include "../../proto/rgpu_cmds.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>

int main() {
    std::printf("rgpu Phase-2 loopback: record -> renderer -> frame\n");
    std::printf("--------------------------------------------------\n");

    const uint32_t W = 64, H = 64, TEX = 1;
    const uint32_t FMT = 28; /* DXGI_FORMAT_R8G8B8A8_UNORM */
    const float color[4] = {0.2f, 0.4f, 0.8f, 1.0f};
    const uint8_t expect[4] = {51, 102, 204, 255};

    /* Build the COMMAND_BATCH: [frame_header][cmd records...] */
    unsigned char payload[512];
    rgpu_batch_writer w; rgpu_bw_init(&w, payload, sizeof(payload));
    rgpu_args_create_texture2d ct{W, H, FMT};
    rgpu_bw_cmd(&w, RGPU_CMD_CREATE_TEXTURE_2D, TEX, &ct, sizeof(ct));
    rgpu_args_clear_rtv cl; cl.rgba[0] = color[0]; cl.rgba[1] = color[1]; cl.rgba[2] = color[2]; cl.rgba[3] = color[3];
    rgpu_bw_cmd(&w, RGPU_CMD_CLEAR_RTV, TEX, &cl, sizeof(cl));
    rgpu_args_present pr{0};
    rgpu_bw_cmd(&w, RGPU_CMD_PRESENT, TEX, &pr, sizeof(pr));

    unsigned char batch[600];
    rgpu_frame_header h{}; h.magic = RGPU_MAGIC; h.version = RGPU_PROTOCOL_VERSION;
    h.type = RGPU_MSG_COMMAND_BATCH; h.session_id = 1; h.sequence = 1; h.payload_len = w.len;
    memcpy(batch, &h, sizeof(h));
    memcpy(batch + sizeof(h), payload, w.len);
    uint32_t total = (uint32_t)sizeof(h) + w.len;
    std::printf("  batch: %u cmds, %u bytes\n", w.count, total);

    RgpuFrame frame; std::string err;
    if (!rgpu_render_batch(batch, total, frame, err)) {
        std::printf("  FAIL: render error: %s\n", err.c_str());
        return 1;
    }
    std::printf("  frame: %ux%u, %zu bytes; renderer used local GPU: %d (expected 1 for the host renderer)\n",
                frame.width, frame.height, frame.rgba.size(), rgpu_renderd_local_device_created());

    int failures = 0;
    if (frame.width != W || frame.height != H) { std::printf("  FAIL: frame dimensions\n"); failures++; }
    /* check several pixels equal the cleared color (tolerance 1 LSB) */
    const size_t samples[] = {0, (size_t)(W / 2) * 4, (size_t)(H - 1) * W * 4 + (W - 1) * 4};
    for (size_t s : samples) {
        for (int c = 0; c < 4; ++c) {
            int got = frame.rgba[s + c], want = expect[c];
            if (std::abs(got - want) > 1) {
                std::printf("  FAIL: pixel@%zu ch%d got %d want %d\n", s / 4, c, got, want);
                failures++;
            }
        }
    }
    if (!failures)
        std::printf("  ok: presented frame pixels == cleared color (%d,%d,%d,%d) through the protocol\n",
                    expect[0], expect[1], expect[2], expect[3]);

    std::printf("--------------------------------------------------\n");
    std::printf(failures ? "RESULT: %d FAILURE(S)\n" : "RESULT: remote-style frame verified\n", failures);
    return failures ? 1 : 0;
}
