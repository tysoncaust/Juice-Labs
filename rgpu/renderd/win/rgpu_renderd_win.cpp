/* rgpu-renderd-win implementation. */
#include "rgpu_renderd_win.h"
#include "../../proto/rgpu_cmds.h"
#include <d3d11.h>
#include <dxgi.h>
#include <map>
#include <cstdio>

namespace {
ID3D11Device        *g_dev = nullptr;
ID3D11DeviceContext *g_ctx = nullptr;
int                  g_local_device_created = 0;
std::map<uint32_t, ID3D11Texture2D *> g_textures;

bool ensure_device(std::string &err) {
    if (g_dev) return true;
    D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                   want, 2, D3D11_SDK_VERSION, &g_dev, &got, &g_ctx);
    if (FAILED(hr)) { err = "renderer D3D11CreateDevice(HARDWARE) failed"; return false; }
    g_local_device_created = 1; // the renderer legitimately uses the real GPU
    return true;
}
} // namespace

int rgpu_renderd_local_device_created() { return g_local_device_created; }

bool rgpu_render_batch(const uint8_t *batch, uint32_t len, RgpuFrame &out, std::string &err) {
    if (len < sizeof(rgpu_frame_header)) { err = "batch too small"; return false; }
    const rgpu_frame_header *h = (const rgpu_frame_header *)batch;
    if (h->magic != RGPU_MAGIC || h->type != RGPU_MSG_COMMAND_BATCH) { err = "not a command batch"; return false; }
    if (!ensure_device(err)) return false;

    rgpu_batch_reader r;
    rgpu_br_init(&r, batch + sizeof(rgpu_frame_header), h->payload_len);
    const rgpu_cmd_record *rec; const unsigned char *args;
    while ((rec = rgpu_br_next(&r, &args)) != nullptr) {
        switch (rec->op) {
        case RGPU_CMD_CREATE_TEXTURE_2D: {
            auto *a = (const rgpu_args_create_texture2d *)args;
            D3D11_TEXTURE2D_DESC td{}; td.Width = a->width; td.Height = a->height;
            td.MipLevels = 1; td.ArraySize = 1; td.Format = (DXGI_FORMAT)a->format;
            td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            ID3D11Texture2D *tex = nullptr;
            if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &tex))) { err = "CreateTexture2D failed"; return false; }
            g_textures[rec->handle] = tex;
            break; }
        case RGPU_CMD_CLEAR_RTV: {
            auto it = g_textures.find(rec->handle);
            if (it == g_textures.end()) { err = "CLEAR_RTV: unknown texture handle"; return false; }
            auto *a = (const rgpu_args_clear_rtv *)args;
            ID3D11RenderTargetView *rtv = nullptr;
            if (FAILED(g_dev->CreateRenderTargetView(it->second, nullptr, &rtv))) { err = "CreateRTV failed"; return false; }
            g_ctx->ClearRenderTargetView(rtv, a->rgba);
            rtv->Release();
            break; }
        case RGPU_CMD_PRESENT: {
            auto it = g_textures.find(rec->handle);
            if (it == g_textures.end()) { err = "PRESENT: unknown texture handle"; return false; }
            D3D11_TEXTURE2D_DESC td; it->second->GetDesc(&td);
            D3D11_TEXTURE2D_DESC sd = td; sd.BindFlags = 0; sd.Usage = D3D11_USAGE_STAGING;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ID3D11Texture2D *stg = nullptr;
            if (FAILED(g_dev->CreateTexture2D(&sd, nullptr, &stg))) { err = "staging CreateTexture2D failed"; return false; }
            g_ctx->CopyResource(stg, it->second);
            D3D11_MAPPED_SUBRESOURCE m{};
            if (FAILED(g_ctx->Map(stg, 0, D3D11_MAP_READ, 0, &m))) { err = "Map staging failed"; stg->Release(); return false; }
            out.width = td.Width; out.height = td.Height; out.rgba.resize((size_t)td.Width * td.Height * 4);
            for (uint32_t y = 0; y < td.Height; ++y)
                memcpy(&out.rgba[(size_t)y * td.Width * 4], (const uint8_t *)m.pData + (size_t)y * m.RowPitch, (size_t)td.Width * 4);
            g_ctx->Unmap(stg, 0); stg->Release();
            break; }
        default:
            std::fprintf(stderr, "[rgpu-renderd] op %u not implemented in the reference renderer\n", rec->op);
            break;
        }
    }
    return true;
}
