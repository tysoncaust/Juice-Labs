/* rgpu full ID3D11Device wrapper (pass-through + tee).
 *
 * Wraps a real ID3D11Device: every one of the 40 ID3D11Device methods forwards
 * to the wrapped device, and the covered subset (render-target CreateTexture2D
 * + CreateRenderTargetView) is TEE'd into the rgpu protocol serializer. This is
 * the Phase-1 reference on the RTX host: a real game boots + runs on the local
 * GPU THROUGH our interception layer while we capture its device-level calls.
 * The production/fail-closed build swaps the pass-through for the remote renderer
 * over the transport (no local device). GetImmediateContext returns the real
 * context (context wrapper is the next layer). */
#ifndef RGPU_D3D11_DEVICE_H
#define RGPU_D3D11_DEVICE_H
#include <d3d11.h>
#include "rgpu_serializer.h"

extern volatile long g_rgpu_dev_wrapped;        // real devices wrapped
extern volatile long g_rgpu_tee_texture2d;      // RT textures tee'd to protocol
extern volatile long g_rgpu_tee_rtv;            // RTVs tee'd to protocol
RgpuDevice &rgpu_tee();                          // the serializer capturing the tee

class RgpuD3D11Device : public ID3D11Device {
    ID3D11Device *real_;
    long ref_ = 1;
public:
    explicit RgpuD3D11Device(ID3D11Device *real) : real_(real) { real_->AddRef(); }
    ~RgpuD3D11Device() { real_->Release(); }

    /* IUnknown */
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11Device)) { *ppv = this; AddRef(); return S_OK; }
        return real_->QueryInterface(riid, ppv); /* newer device ifaces pass through */
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)++ref_; }
    ULONG STDMETHODCALLTYPE Release() override { long r = --ref_; if (r == 0) delete this; return (ULONG)r; }

    /* ID3D11Device — all 40 methods forward to the real device */
    HRESULT STDMETHODCALLTYPE CreateBuffer(const D3D11_BUFFER_DESC *d, const D3D11_SUBRESOURCE_DATA *i, ID3D11Buffer **o) override { return real_->CreateBuffer(d, i, o); }
    HRESULT STDMETHODCALLTYPE CreateTexture1D(const D3D11_TEXTURE1D_DESC *d, const D3D11_SUBRESOURCE_DATA *i, ID3D11Texture1D **o) override { return real_->CreateTexture1D(d, i, o); }
    HRESULT STDMETHODCALLTYPE CreateTexture2D(const D3D11_TEXTURE2D_DESC *d, const D3D11_SUBRESOURCE_DATA *i, ID3D11Texture2D **o) override {
        HRESULT hr = real_->CreateTexture2D(d, i, o);
        if (SUCCEEDED(hr) && d && (d->BindFlags & D3D11_BIND_RENDER_TARGET)) {
            rgpu_tee().CreateTexture2D(d->Width, d->Height, (uint32_t)d->Format);
            g_rgpu_tee_texture2d++;
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE CreateTexture3D(const D3D11_TEXTURE3D_DESC *d, const D3D11_SUBRESOURCE_DATA *i, ID3D11Texture3D **o) override { return real_->CreateTexture3D(d, i, o); }
    HRESULT STDMETHODCALLTYPE CreateShaderResourceView(ID3D11Resource *r, const D3D11_SHADER_RESOURCE_VIEW_DESC *d, ID3D11ShaderResourceView **o) override { return real_->CreateShaderResourceView(r, d, o); }
    HRESULT STDMETHODCALLTYPE CreateUnorderedAccessView(ID3D11Resource *r, const D3D11_UNORDERED_ACCESS_VIEW_DESC *d, ID3D11UnorderedAccessView **o) override { return real_->CreateUnorderedAccessView(r, d, o); }
    HRESULT STDMETHODCALLTYPE CreateRenderTargetView(ID3D11Resource *r, const D3D11_RENDER_TARGET_VIEW_DESC *d, ID3D11RenderTargetView **o) override {
        HRESULT hr = real_->CreateRenderTargetView(r, d, o);
        if (SUCCEEDED(hr)) { rgpu_tee().CreateRenderTargetView(0); g_rgpu_tee_rtv++; }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE CreateDepthStencilView(ID3D11Resource *r, const D3D11_DEPTH_STENCIL_VIEW_DESC *d, ID3D11DepthStencilView **o) override { return real_->CreateDepthStencilView(r, d, o); }
    HRESULT STDMETHODCALLTYPE CreateInputLayout(const D3D11_INPUT_ELEMENT_DESC *e, UINT n, const void *b, SIZE_T bl, ID3D11InputLayout **o) override { return real_->CreateInputLayout(e, n, b, bl, o); }
    HRESULT STDMETHODCALLTYPE CreateVertexShader(const void *b, SIZE_T bl, ID3D11ClassLinkage *c, ID3D11VertexShader **o) override { return real_->CreateVertexShader(b, bl, c, o); }
    HRESULT STDMETHODCALLTYPE CreateGeometryShader(const void *b, SIZE_T bl, ID3D11ClassLinkage *c, ID3D11GeometryShader **o) override { return real_->CreateGeometryShader(b, bl, c, o); }
    HRESULT STDMETHODCALLTYPE CreateGeometryShaderWithStreamOutput(const void *b, SIZE_T bl, const D3D11_SO_DECLARATION_ENTRY *so, UINT ne, const UINT *bs, UINT ns, UINT rs, ID3D11ClassLinkage *c, ID3D11GeometryShader **o) override { return real_->CreateGeometryShaderWithStreamOutput(b, bl, so, ne, bs, ns, rs, c, o); }
    HRESULT STDMETHODCALLTYPE CreatePixelShader(const void *b, SIZE_T bl, ID3D11ClassLinkage *c, ID3D11PixelShader **o) override { return real_->CreatePixelShader(b, bl, c, o); }
    HRESULT STDMETHODCALLTYPE CreateHullShader(const void *b, SIZE_T bl, ID3D11ClassLinkage *c, ID3D11HullShader **o) override { return real_->CreateHullShader(b, bl, c, o); }
    HRESULT STDMETHODCALLTYPE CreateDomainShader(const void *b, SIZE_T bl, ID3D11ClassLinkage *c, ID3D11DomainShader **o) override { return real_->CreateDomainShader(b, bl, c, o); }
    HRESULT STDMETHODCALLTYPE CreateComputeShader(const void *b, SIZE_T bl, ID3D11ClassLinkage *c, ID3D11ComputeShader **o) override { return real_->CreateComputeShader(b, bl, c, o); }
    HRESULT STDMETHODCALLTYPE CreateClassLinkage(ID3D11ClassLinkage **o) override { return real_->CreateClassLinkage(o); }
    HRESULT STDMETHODCALLTYPE CreateBlendState(const D3D11_BLEND_DESC *d, ID3D11BlendState **o) override { return real_->CreateBlendState(d, o); }
    HRESULT STDMETHODCALLTYPE CreateDepthStencilState(const D3D11_DEPTH_STENCIL_DESC *d, ID3D11DepthStencilState **o) override { return real_->CreateDepthStencilState(d, o); }
    HRESULT STDMETHODCALLTYPE CreateRasterizerState(const D3D11_RASTERIZER_DESC *d, ID3D11RasterizerState **o) override { return real_->CreateRasterizerState(d, o); }
    HRESULT STDMETHODCALLTYPE CreateSamplerState(const D3D11_SAMPLER_DESC *d, ID3D11SamplerState **o) override { return real_->CreateSamplerState(d, o); }
    HRESULT STDMETHODCALLTYPE CreateQuery(const D3D11_QUERY_DESC *d, ID3D11Query **o) override { return real_->CreateQuery(d, o); }
    HRESULT STDMETHODCALLTYPE CreatePredicate(const D3D11_QUERY_DESC *d, ID3D11Predicate **o) override { return real_->CreatePredicate(d, o); }
    HRESULT STDMETHODCALLTYPE CreateCounter(const D3D11_COUNTER_DESC *d, ID3D11Counter **o) override { return real_->CreateCounter(d, o); }
    HRESULT STDMETHODCALLTYPE CreateDeferredContext(UINT f, ID3D11DeviceContext **o) override { return real_->CreateDeferredContext(f, o); }
    HRESULT STDMETHODCALLTYPE OpenSharedResource(HANDLE h, REFIID r, void **o) override { return real_->OpenSharedResource(h, r, o); }
    HRESULT STDMETHODCALLTYPE CheckFormatSupport(DXGI_FORMAT f, UINT *o) override { return real_->CheckFormatSupport(f, o); }
    HRESULT STDMETHODCALLTYPE CheckMultisampleQualityLevels(DXGI_FORMAT f, UINT s, UINT *o) override { return real_->CheckMultisampleQualityLevels(f, s, o); }
    void    STDMETHODCALLTYPE CheckCounterInfo(D3D11_COUNTER_INFO *o) override { real_->CheckCounterInfo(o); }
    HRESULT STDMETHODCALLTYPE CheckCounter(const D3D11_COUNTER_DESC *d, D3D11_COUNTER_TYPE *t, UINT *ac, LPSTR n, UINT *nl, LPSTR u, UINT *ul, LPSTR de, UINT *dl) override { return real_->CheckCounter(d, t, ac, n, nl, u, ul, de, dl); }
    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(D3D11_FEATURE f, void *d, UINT s) override { return real_->CheckFeatureSupport(f, d, s); }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID g, UINT *s, void *d) override { return real_->GetPrivateData(g, s, d); }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID g, UINT s, const void *d) override { return real_->SetPrivateData(g, s, d); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID g, const IUnknown *d) override { return real_->SetPrivateDataInterface(g, d); }
    D3D_FEATURE_LEVEL STDMETHODCALLTYPE GetFeatureLevel() override { return real_->GetFeatureLevel(); }
    UINT    STDMETHODCALLTYPE GetCreationFlags() override { return real_->GetCreationFlags(); }
    HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason() override { return real_->GetDeviceRemovedReason(); }
    void    STDMETHODCALLTYPE GetImmediateContext(ID3D11DeviceContext **o) override { real_->GetImmediateContext(o); }
    HRESULT STDMETHODCALLTYPE SetExceptionMode(UINT f) override { return real_->SetExceptionMode(f); }
    UINT    STDMETHODCALLTYPE GetExceptionMode() override { return real_->GetExceptionMode(); }
};
#endif
