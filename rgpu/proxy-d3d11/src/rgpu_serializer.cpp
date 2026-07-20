/* rgpu serializing D3D11 front-end — implementation. */
#include "rgpu_serializer.h"
#include <cstring>

void RgpuDevice::record(uint32_t op, uint32_t handle, const void *args, uint32_t n) {
    rgpu_cmd_record rec; rec.op = op; rec.handle = handle; rec.arg_len = n;
    const unsigned char *p = (const unsigned char *)&rec;
    body_.insert(body_.end(), p, p + sizeof(rec));
    if (n) body_.insert(body_.end(), (const unsigned char *)args, (const unsigned char *)args + n);
    count_++;
}

uint32_t RgpuDevice::CreateTexture2D(uint32_t width, uint32_t height, uint32_t dxgi_format) {
    uint32_t h = next_handle_++;
    rgpu_args_create_texture2d a{width, height, dxgi_format};
    record(RGPU_CMD_CREATE_TEXTURE_2D, h, &a, sizeof(a));
    return h;
}

uint32_t RgpuDevice::CreateRenderTargetView(uint32_t texture_handle) {
    // reference render target view shares the resource handle
    record(RGPU_CMD_CREATE_RENDER_TARGET_VIEW, texture_handle, nullptr, 0);
    return texture_handle;
}

std::vector<unsigned char> RgpuDevice::BuildBatch(uint32_t session, uint32_t sequence) const {
    rgpu_frame_header h{}; h.magic = RGPU_MAGIC; h.version = RGPU_PROTOCOL_VERSION;
    h.type = RGPU_MSG_COMMAND_BATCH; h.session_id = session; h.sequence = sequence;
    h.payload_len = (uint32_t)body_.size();
    std::vector<unsigned char> out;
    const unsigned char *p = (const unsigned char *)&h;
    out.insert(out.end(), p, p + sizeof(h));
    out.insert(out.end(), body_.begin(), body_.end());
    return out;
}

void RgpuContext::ClearRenderTargetView(uint32_t rtv_handle, const float rgba[4]) {
    rgpu_args_clear_rtv a; std::memcpy(a.rgba, rgba, sizeof(a.rgba));
    dev_->record(RGPU_CMD_CLEAR_RTV, rtv_handle, &a, sizeof(a));
}

void RgpuContext::Present(uint32_t frame_handle) {
    rgpu_args_present a{0};
    dev_->record(RGPU_CMD_PRESENT, frame_handle, &a, sizeof(a));
}
