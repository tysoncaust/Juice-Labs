/* rgpu full ID3D11DeviceContext wrapper (pass-through + tee).
 *
 * The complete immediate-context vtable: IUnknown(3) + ID3D11DeviceChild(4) +
 * ID3D11DeviceContext(108) = 115 entries. Every method forwards to the wrapped
 * real context so the game renders on the local GPU; the per-frame draw/state/
 * clear calls are TEE'd into the rgpu protocol so the actual per-frame command
 * stream (not just device creation) is captured. GetDevice returns the wrapper
 * device to preserve COM identity. This is the D3D11 half of "per-frame calls
 * land in the protocol"; the remote renderer additionally needs the resource
 * object-graph translation (pointer->id), tracked separately. */
#ifndef RGPU_D3D11_CONTEXT_H
#define RGPU_D3D11_CONTEXT_H
#include <d3d11.h>
#include "rgpu_tee.h"

class RgpuD3D11Context : public ID3D11DeviceContext {
    ID3D11DeviceContext *real_;
    ID3D11Device *dev_wrapper_;   /* the RgpuD3D11Device, for GetDevice identity */
    long ref_ = 1;
public:
    RgpuD3D11Context(ID3D11DeviceContext *real, ID3D11Device *devWrapper)
        : real_(real), dev_wrapper_(devWrapper) { real_->AddRef(); }
    ~RgpuD3D11Context() { real_->Release(); }

    /* IUnknown */
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11DeviceChild) ||
            riid == __uuidof(ID3D11DeviceContext)) { *ppv = this; AddRef(); return S_OK; }
        return real_->QueryInterface(riid, ppv); /* newer context ifaces pass through */
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)++ref_; }
    ULONG STDMETHODCALLTYPE Release() override { long r = --ref_; if (r == 0) delete this; return (ULONG)r; }

    /* ID3D11DeviceChild */
    void STDMETHODCALLTYPE GetDevice(ID3D11Device **ppDevice) override {
        if (ppDevice) { *ppDevice = dev_wrapper_; dev_wrapper_->AddRef(); } /* keep wrapper identity */
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID g, UINT *s, void *d) override { return real_->GetPrivateData(g, s, d); }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID g, UINT s, const void *d) override { return real_->SetPrivateData(g, s, d); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID g, const IUnknown *d) override { return real_->SetPrivateDataInterface(g, d); }

    /* ---- ID3D11DeviceContext: set state (forward) ------------------------- */
    void STDMETHODCALLTYPE VSSetConstantBuffers(UINT s, UINT n, ID3D11Buffer *const *b) override { real_->VSSetConstantBuffers(s, n, b); }
    void STDMETHODCALLTYPE PSSetShaderResources(UINT s, UINT n, ID3D11ShaderResourceView *const *v) override { real_->PSSetShaderResources(s, n, v); }
    void STDMETHODCALLTYPE PSSetShader(ID3D11PixelShader *ps, ID3D11ClassInstance *const *ci, UINT n) override { real_->PSSetShader(ps, ci, n); }
    void STDMETHODCALLTYPE PSSetSamplers(UINT s, UINT n, ID3D11SamplerState *const *ss) override { real_->PSSetSamplers(s, n, ss); }
    void STDMETHODCALLTYPE VSSetShader(ID3D11VertexShader *vs, ID3D11ClassInstance *const *ci, UINT n) override { real_->VSSetShader(vs, ci, n); }

    /* ---- draws: forward + TEE --------------------------------------------- */
    void STDMETHODCALLTYPE DrawIndexed(UINT ic, UINT sil, INT bvl) override {
        real_->DrawIndexed(ic, sil, bvl);
        rgpu_args_draw_indexed a{ic, sil, bvl, 1, 0}; rgpu_tee().record(RGPU_CMD_DRAW_INDEXED, 0, &a, sizeof(a)); g_rgpu_tee_draws++;
    }
    void STDMETHODCALLTYPE Draw(UINT vc, UINT svl) override {
        real_->Draw(vc, svl);
        rgpu_args_draw a{vc, svl, 1, 0}; rgpu_tee().record(RGPU_CMD_DRAW, 0, &a, sizeof(a)); g_rgpu_tee_draws++;
    }
    HRESULT STDMETHODCALLTYPE Map(ID3D11Resource *r, UINT sub, D3D11_MAP mt, UINT mf, D3D11_MAPPED_SUBRESOURCE *m) override { return real_->Map(r, sub, mt, mf, m); }
    void STDMETHODCALLTYPE Unmap(ID3D11Resource *r, UINT sub) override { real_->Unmap(r, sub); }
    void STDMETHODCALLTYPE PSSetConstantBuffers(UINT s, UINT n, ID3D11Buffer *const *b) override { real_->PSSetConstantBuffers(s, n, b); }
    void STDMETHODCALLTYPE IASetInputLayout(ID3D11InputLayout *il) override { real_->IASetInputLayout(il); }
    void STDMETHODCALLTYPE IASetVertexBuffers(UINT s, UINT n, ID3D11Buffer *const *b, const UINT *st, const UINT *o) override { real_->IASetVertexBuffers(s, n, b, st, o); }
    void STDMETHODCALLTYPE IASetIndexBuffer(ID3D11Buffer *b, DXGI_FORMAT f, UINT o) override { real_->IASetIndexBuffer(b, f, o); }
    void STDMETHODCALLTYPE DrawIndexedInstanced(UINT icpi, UINT ic, UINT sil, INT bvl, UINT sil2) override {
        real_->DrawIndexedInstanced(icpi, ic, sil, bvl, sil2);
        rgpu_args_draw_indexed a{icpi, sil, bvl, ic, sil2}; rgpu_tee().record(RGPU_CMD_DRAW_INDEXED, 0, &a, sizeof(a)); g_rgpu_tee_draws++;
    }
    void STDMETHODCALLTYPE DrawInstanced(UINT vcpi, UINT ic, UINT svl, UINT sil) override {
        real_->DrawInstanced(vcpi, ic, svl, sil);
        rgpu_args_draw a{vcpi, svl, ic, sil}; rgpu_tee().record(RGPU_CMD_DRAW, 0, &a, sizeof(a)); g_rgpu_tee_draws++;
    }
    void STDMETHODCALLTYPE GSSetConstantBuffers(UINT s, UINT n, ID3D11Buffer *const *b) override { real_->GSSetConstantBuffers(s, n, b); }
    void STDMETHODCALLTYPE GSSetShader(ID3D11GeometryShader *gs, ID3D11ClassInstance *const *ci, UINT n) override { real_->GSSetShader(gs, ci, n); }
    void STDMETHODCALLTYPE IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY t) override { real_->IASetPrimitiveTopology(t); }
    void STDMETHODCALLTYPE VSSetShaderResources(UINT s, UINT n, ID3D11ShaderResourceView *const *v) override { real_->VSSetShaderResources(s, n, v); }
    void STDMETHODCALLTYPE VSSetSamplers(UINT s, UINT n, ID3D11SamplerState *const *ss) override { real_->VSSetSamplers(s, n, ss); }
    void STDMETHODCALLTYPE Begin(ID3D11Asynchronous *a) override { real_->Begin(a); }
    void STDMETHODCALLTYPE End(ID3D11Asynchronous *a) override { real_->End(a); }
    HRESULT STDMETHODCALLTYPE GetData(ID3D11Asynchronous *a, void *d, UINT ds, UINT f) override { return real_->GetData(a, d, ds, f); }
    void STDMETHODCALLTYPE SetPredication(ID3D11Predicate *p, WINBOOL v) override { real_->SetPredication(p, v); }
    void STDMETHODCALLTYPE GSSetShaderResources(UINT s, UINT n, ID3D11ShaderResourceView *const *v) override { real_->GSSetShaderResources(s, n, v); }
    void STDMETHODCALLTYPE GSSetSamplers(UINT s, UINT n, ID3D11SamplerState *const *ss) override { real_->GSSetSamplers(s, n, ss); }
    void STDMETHODCALLTYPE OMSetRenderTargets(UINT n, ID3D11RenderTargetView *const *rtv, ID3D11DepthStencilView *dsv) override {
        real_->OMSetRenderTargets(n, rtv, dsv);
        rgpu_args_set_render_targets a{n, (n && rtv) ? rgpu_obj_id(rtv[0]) : 0, rgpu_obj_id(dsv)};
        rgpu_tee().record(RGPU_CMD_SET_RENDER_TARGETS, 0, &a, sizeof(a)); g_rgpu_tee_state++;
    }
    void STDMETHODCALLTYPE OMSetRenderTargetsAndUnorderedAccessViews(UINT nr, ID3D11RenderTargetView *const *rtv, ID3D11DepthStencilView *dsv, UINT us, UINT nu, ID3D11UnorderedAccessView *const *uav, const UINT *ic) override { real_->OMSetRenderTargetsAndUnorderedAccessViews(nr, rtv, dsv, us, nu, uav, ic); }
    void STDMETHODCALLTYPE OMSetBlendState(ID3D11BlendState *b, const FLOAT bf[4], UINT sm) override { real_->OMSetBlendState(b, bf, sm); }
    void STDMETHODCALLTYPE OMSetDepthStencilState(ID3D11DepthStencilState *d, UINT sr) override { real_->OMSetDepthStencilState(d, sr); }
    void STDMETHODCALLTYPE SOSetTargets(UINT n, ID3D11Buffer *const *b, const UINT *o) override { real_->SOSetTargets(n, b, o); }
    void STDMETHODCALLTYPE DrawAuto() override { real_->DrawAuto(); g_rgpu_tee_draws++; }
    void STDMETHODCALLTYPE DrawIndexedInstancedIndirect(ID3D11Buffer *b, UINT o) override { real_->DrawIndexedInstancedIndirect(b, o); g_rgpu_tee_draws++; }
    void STDMETHODCALLTYPE DrawInstancedIndirect(ID3D11Buffer *b, UINT o) override { real_->DrawInstancedIndirect(b, o); g_rgpu_tee_draws++; }
    void STDMETHODCALLTYPE Dispatch(UINT x, UINT y, UINT z) override {
        real_->Dispatch(x, y, z);
        rgpu_args_dispatch a{x, y, z}; rgpu_tee().record(RGPU_CMD_DISPATCH, 0, &a, sizeof(a)); g_rgpu_tee_draws++;
    }
    void STDMETHODCALLTYPE DispatchIndirect(ID3D11Buffer *b, UINT o) override { real_->DispatchIndirect(b, o); g_rgpu_tee_draws++; }
    void STDMETHODCALLTYPE RSSetState(ID3D11RasterizerState *r) override { real_->RSSetState(r); }
    void STDMETHODCALLTYPE RSSetViewports(UINT n, const D3D11_VIEWPORT *v) override { real_->RSSetViewports(n, v); }
    void STDMETHODCALLTYPE RSSetScissorRects(UINT n, const D3D11_RECT *r) override { real_->RSSetScissorRects(n, r); }
    void STDMETHODCALLTYPE CopySubresourceRegion(ID3D11Resource *dr, UINT ds, UINT x, UINT y, UINT z, ID3D11Resource *sr, UINT ss, const D3D11_BOX *b) override { real_->CopySubresourceRegion(dr, ds, x, y, z, sr, ss, b); }
    void STDMETHODCALLTYPE CopyResource(ID3D11Resource *dr, ID3D11Resource *sr) override {
        real_->CopyResource(dr, sr);
        rgpu_tee().record(RGPU_CMD_COPY_RESOURCE, rgpu_obj_id(dr), nullptr, 0); g_rgpu_tee_state++;
    }
    void STDMETHODCALLTYPE UpdateSubresource(ID3D11Resource *dr, UINT ds, const D3D11_BOX *b, const void *d, UINT rp, UINT dp) override { real_->UpdateSubresource(dr, ds, b, d, rp, dp); }
    void STDMETHODCALLTYPE CopyStructureCount(ID3D11Buffer *db, UINT o, ID3D11UnorderedAccessView *sv) override { real_->CopyStructureCount(db, o, sv); }
    void STDMETHODCALLTYPE ClearRenderTargetView(ID3D11RenderTargetView *rtv, const FLOAT c[4]) override {
        real_->ClearRenderTargetView(rtv, c);
        rgpu_args_clear_rtv a; memcpy(a.rgba, c, sizeof(a.rgba));
        rgpu_tee().record(RGPU_CMD_CLEAR_RTV, rgpu_obj_id(rtv), &a, sizeof(a)); g_rgpu_tee_clears++;
    }
    void STDMETHODCALLTYPE ClearUnorderedAccessViewUint(ID3D11UnorderedAccessView *u, const UINT v[4]) override { real_->ClearUnorderedAccessViewUint(u, v); }
    void STDMETHODCALLTYPE ClearUnorderedAccessViewFloat(ID3D11UnorderedAccessView *u, const FLOAT v[4]) override { real_->ClearUnorderedAccessViewFloat(u, v); }
    void STDMETHODCALLTYPE ClearDepthStencilView(ID3D11DepthStencilView *dsv, UINT f, FLOAT depth, UINT8 stencil) override {
        real_->ClearDepthStencilView(dsv, f, depth, stencil);
        rgpu_args_clear_dsv a{f, depth, stencil}; rgpu_tee().record(RGPU_CMD_CLEAR_DSV, rgpu_obj_id(dsv), &a, sizeof(a)); g_rgpu_tee_clears++;
    }
    void STDMETHODCALLTYPE GenerateMips(ID3D11ShaderResourceView *v) override { real_->GenerateMips(v); }
    void STDMETHODCALLTYPE SetResourceMinLOD(ID3D11Resource *r, FLOAT l) override { real_->SetResourceMinLOD(r, l); }
    FLOAT STDMETHODCALLTYPE GetResourceMinLOD(ID3D11Resource *r) override { return real_->GetResourceMinLOD(r); }
    void STDMETHODCALLTYPE ResolveSubresource(ID3D11Resource *dr, UINT ds, ID3D11Resource *sr, UINT ss, DXGI_FORMAT f) override { real_->ResolveSubresource(dr, ds, sr, ss, f); }
    void STDMETHODCALLTYPE ExecuteCommandList(ID3D11CommandList *c, WINBOOL r) override { real_->ExecuteCommandList(c, r); }
    void STDMETHODCALLTYPE HSSetShaderResources(UINT s, UINT n, ID3D11ShaderResourceView *const *v) override { real_->HSSetShaderResources(s, n, v); }
    void STDMETHODCALLTYPE HSSetShader(ID3D11HullShader *h, ID3D11ClassInstance *const *ci, UINT n) override { real_->HSSetShader(h, ci, n); }
    void STDMETHODCALLTYPE HSSetSamplers(UINT s, UINT n, ID3D11SamplerState *const *ss) override { real_->HSSetSamplers(s, n, ss); }
    void STDMETHODCALLTYPE HSSetConstantBuffers(UINT s, UINT n, ID3D11Buffer *const *b) override { real_->HSSetConstantBuffers(s, n, b); }
    void STDMETHODCALLTYPE DSSetShaderResources(UINT s, UINT n, ID3D11ShaderResourceView *const *v) override { real_->DSSetShaderResources(s, n, v); }
    void STDMETHODCALLTYPE DSSetShader(ID3D11DomainShader *d, ID3D11ClassInstance *const *ci, UINT n) override { real_->DSSetShader(d, ci, n); }
    void STDMETHODCALLTYPE DSSetSamplers(UINT s, UINT n, ID3D11SamplerState *const *ss) override { real_->DSSetSamplers(s, n, ss); }
    void STDMETHODCALLTYPE DSSetConstantBuffers(UINT s, UINT n, ID3D11Buffer *const *b) override { real_->DSSetConstantBuffers(s, n, b); }
    void STDMETHODCALLTYPE CSSetShaderResources(UINT s, UINT n, ID3D11ShaderResourceView *const *v) override { real_->CSSetShaderResources(s, n, v); }
    void STDMETHODCALLTYPE CSSetUnorderedAccessViews(UINT s, UINT n, ID3D11UnorderedAccessView *const *u, const UINT *ic) override { real_->CSSetUnorderedAccessViews(s, n, u, ic); }
    void STDMETHODCALLTYPE CSSetShader(ID3D11ComputeShader *c, ID3D11ClassInstance *const *ci, UINT n) override { real_->CSSetShader(c, ci, n); }
    void STDMETHODCALLTYPE CSSetSamplers(UINT s, UINT n, ID3D11SamplerState *const *ss) override { real_->CSSetSamplers(s, n, ss); }
    void STDMETHODCALLTYPE CSSetConstantBuffers(UINT s, UINT n, ID3D11Buffer *const *b) override { real_->CSSetConstantBuffers(s, n, b); }

    /* ---- get state (forward) ---------------------------------------------- */
    void STDMETHODCALLTYPE VSGetConstantBuffers(UINT s, UINT n, ID3D11Buffer **b) override { real_->VSGetConstantBuffers(s, n, b); }
    void STDMETHODCALLTYPE PSGetShaderResources(UINT s, UINT n, ID3D11ShaderResourceView **v) override { real_->PSGetShaderResources(s, n, v); }
    void STDMETHODCALLTYPE PSGetShader(ID3D11PixelShader **ps, ID3D11ClassInstance **ci, UINT *n) override { real_->PSGetShader(ps, ci, n); }
    void STDMETHODCALLTYPE PSGetSamplers(UINT s, UINT n, ID3D11SamplerState **ss) override { real_->PSGetSamplers(s, n, ss); }
    void STDMETHODCALLTYPE VSGetShader(ID3D11VertexShader **vs, ID3D11ClassInstance **ci, UINT *n) override { real_->VSGetShader(vs, ci, n); }
    void STDMETHODCALLTYPE PSGetConstantBuffers(UINT s, UINT n, ID3D11Buffer **b) override { real_->PSGetConstantBuffers(s, n, b); }
    void STDMETHODCALLTYPE IAGetInputLayout(ID3D11InputLayout **il) override { real_->IAGetInputLayout(il); }
    void STDMETHODCALLTYPE IAGetVertexBuffers(UINT s, UINT n, ID3D11Buffer **b, UINT *st, UINT *o) override { real_->IAGetVertexBuffers(s, n, b, st, o); }
    void STDMETHODCALLTYPE IAGetIndexBuffer(ID3D11Buffer **b, DXGI_FORMAT *f, UINT *o) override { real_->IAGetIndexBuffer(b, f, o); }
    void STDMETHODCALLTYPE GSGetConstantBuffers(UINT s, UINT n, ID3D11Buffer **b) override { real_->GSGetConstantBuffers(s, n, b); }
    void STDMETHODCALLTYPE GSGetShader(ID3D11GeometryShader **gs, ID3D11ClassInstance **ci, UINT *n) override { real_->GSGetShader(gs, ci, n); }
    void STDMETHODCALLTYPE IAGetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY *t) override { real_->IAGetPrimitiveTopology(t); }
    void STDMETHODCALLTYPE VSGetShaderResources(UINT s, UINT n, ID3D11ShaderResourceView **v) override { real_->VSGetShaderResources(s, n, v); }
    void STDMETHODCALLTYPE VSGetSamplers(UINT s, UINT n, ID3D11SamplerState **ss) override { real_->VSGetSamplers(s, n, ss); }
    void STDMETHODCALLTYPE GetPredication(ID3D11Predicate **p, WINBOOL *v) override { real_->GetPredication(p, v); }
    void STDMETHODCALLTYPE GSGetShaderResources(UINT s, UINT n, ID3D11ShaderResourceView **v) override { real_->GSGetShaderResources(s, n, v); }
    void STDMETHODCALLTYPE GSGetSamplers(UINT s, UINT n, ID3D11SamplerState **ss) override { real_->GSGetSamplers(s, n, ss); }
    void STDMETHODCALLTYPE OMGetRenderTargets(UINT n, ID3D11RenderTargetView **rtv, ID3D11DepthStencilView **dsv) override { real_->OMGetRenderTargets(n, rtv, dsv); }
    void STDMETHODCALLTYPE OMGetRenderTargetsAndUnorderedAccessViews(UINT nr, ID3D11RenderTargetView **rtv, ID3D11DepthStencilView **dsv, UINT us, UINT nu, ID3D11UnorderedAccessView **uav) override { real_->OMGetRenderTargetsAndUnorderedAccessViews(nr, rtv, dsv, us, nu, uav); }
    void STDMETHODCALLTYPE OMGetBlendState(ID3D11BlendState **b, FLOAT bf[4], UINT *sm) override { real_->OMGetBlendState(b, bf, sm); }
    void STDMETHODCALLTYPE OMGetDepthStencilState(ID3D11DepthStencilState **d, UINT *sr) override { real_->OMGetDepthStencilState(d, sr); }
    void STDMETHODCALLTYPE SOGetTargets(UINT n, ID3D11Buffer **b) override { real_->SOGetTargets(n, b); }
    void STDMETHODCALLTYPE RSGetState(ID3D11RasterizerState **r) override { real_->RSGetState(r); }
    void STDMETHODCALLTYPE RSGetViewports(UINT *n, D3D11_VIEWPORT *v) override { real_->RSGetViewports(n, v); }
    void STDMETHODCALLTYPE RSGetScissorRects(UINT *n, D3D11_RECT *r) override { real_->RSGetScissorRects(n, r); }
    void STDMETHODCALLTYPE HSGetShaderResources(UINT s, UINT n, ID3D11ShaderResourceView **v) override { real_->HSGetShaderResources(s, n, v); }
    void STDMETHODCALLTYPE HSGetShader(ID3D11HullShader **h, ID3D11ClassInstance **ci, UINT *n) override { real_->HSGetShader(h, ci, n); }
    void STDMETHODCALLTYPE HSGetSamplers(UINT s, UINT n, ID3D11SamplerState **ss) override { real_->HSGetSamplers(s, n, ss); }
    void STDMETHODCALLTYPE HSGetConstantBuffers(UINT s, UINT n, ID3D11Buffer **b) override { real_->HSGetConstantBuffers(s, n, b); }
    void STDMETHODCALLTYPE DSGetShaderResources(UINT s, UINT n, ID3D11ShaderResourceView **v) override { real_->DSGetShaderResources(s, n, v); }
    void STDMETHODCALLTYPE DSGetShader(ID3D11DomainShader **d, ID3D11ClassInstance **ci, UINT *n) override { real_->DSGetShader(d, ci, n); }
    void STDMETHODCALLTYPE DSGetSamplers(UINT s, UINT n, ID3D11SamplerState **ss) override { real_->DSGetSamplers(s, n, ss); }
    void STDMETHODCALLTYPE DSGetConstantBuffers(UINT s, UINT n, ID3D11Buffer **b) override { real_->DSGetConstantBuffers(s, n, b); }
    void STDMETHODCALLTYPE CSGetShaderResources(UINT s, UINT n, ID3D11ShaderResourceView **v) override { real_->CSGetShaderResources(s, n, v); }
    void STDMETHODCALLTYPE CSGetUnorderedAccessViews(UINT s, UINT n, ID3D11UnorderedAccessView **u) override { real_->CSGetUnorderedAccessViews(s, n, u); }
    void STDMETHODCALLTYPE CSGetShader(ID3D11ComputeShader **c, ID3D11ClassInstance **ci, UINT *n) override { real_->CSGetShader(c, ci, n); }
    void STDMETHODCALLTYPE CSGetSamplers(UINT s, UINT n, ID3D11SamplerState **ss) override { real_->CSGetSamplers(s, n, ss); }
    void STDMETHODCALLTYPE CSGetConstantBuffers(UINT s, UINT n, ID3D11Buffer **b) override { real_->CSGetConstantBuffers(s, n, b); }
    void STDMETHODCALLTYPE ClearState() override { real_->ClearState(); }
    void STDMETHODCALLTYPE Flush() override { real_->Flush(); }
    D3D11_DEVICE_CONTEXT_TYPE STDMETHODCALLTYPE GetType() override { return real_->GetType(); }
    UINT STDMETHODCALLTYPE GetContextFlags() override { return real_->GetContextFlags(); }
    HRESULT STDMETHODCALLTYPE FinishCommandList(WINBOOL r, ID3D11CommandList **c) override { return real_->FinishCommandList(r, c); }
};
#endif
