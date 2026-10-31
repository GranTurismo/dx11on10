// Runs compute shaders on hardware without compute shaders.
//
// Generic path: the compute shader was rewritten (src/dxbc/compute.cpp) as one geometry
// shader per UAV it writes. A dispatch becomes one draw per UAV: one point per thread
// through our vertex shader, the generated geometry shader (which runs the thread and
// emits a point per store), and a pixel shader writing the value into a render target
// view of the UAV's texture. The compute stage's textures, constants and samplers are
// rebound for the geometry shader; the game's pipeline state is saved and restored.
//
// Fast path: when every thread writes just its own texel (plus a constant offset), the
// generated pixel shader runs once per texel over a full-screen triangle instead.
#include <cstring>
#include <memory>
#include <unordered_map>

#include "../dxbc/compute.h"
#include "emulation_shaders.h"
#include "proxy.h"

namespace {

// {9b0e8f3a-2c61-4d7e-8a15-5f3c2b7d9e40}: render target view kept with a placeholder UAV
const GUID kRtvGuid = {0x9b0e8f3a, 0x2c61, 0x4d7e, {0x8a, 0x15, 0x5f, 0x3c, 0x2b, 0x7d, 0x9e, 0x40}};
// {9b0e8f3b-2c61-4d7e-8a15-5f3c2b7d9e40}: snapshot view kept with a placeholder UAV
const GUID kSnapshotGuid = {0x9b0e8f3b, 0x2c61, 0x4d7e, {0x8a, 0x15, 0x5f, 0x3c, 0x2b, 0x7d, 0x9e, 0x40}};

struct Emulation {
    dxbc::ComputeProgram program;
    std::vector<ID3D11GeometryShader*> gs;
    std::vector<ID3D11PixelShader*> ps;  // fast path per pass, or null
    ~Emulation() {
        for (ID3D11GeometryShader* g : gs)
            if (g) g->Release();
        for (ID3D11PixelShader* s : ps)
            if (s) s->Release();
    }
};

SRWLOCK g_lock = SRWLOCK_INIT;
std::unordered_map<const void*, std::shared_ptr<Emulation>> g_emulations;

struct Resources {
    ID3D11VertexShader* fullscreen = nullptr;
    ID3D11VertexShader* thread = nullptr;
    ID3D11PixelShader* store[3] = {};
    ID3D11Buffer* constants = nullptr;
    ID3D11RasterizerState* raster = nullptr;
    ID3D11DepthStencilState* noDepth = nullptr;
    bool ready = false, failed = false;
};
std::unordered_map<ID3D11Device*, Resources> g_resources;

Resources* ResourcesFor(ID3D11Device* device) {
    AcquireSRWLockExclusive(&g_lock);
    Resources& r = g_resources[device];
    if (!r.ready && !r.failed) {
        HRESULT hr = device->CreateVertexShader(k_vs_fullscreen, sizeof(k_vs_fullscreen), nullptr, &r.fullscreen);
        if (SUCCEEDED(hr)) hr = device->CreateVertexShader(k_vs_thread, sizeof(k_vs_thread), nullptr, &r.thread);
        if (SUCCEEDED(hr)) hr = device->CreatePixelShader(k_ps_store_float, sizeof(k_ps_store_float), nullptr, &r.store[0]);
        if (SUCCEEDED(hr)) hr = device->CreatePixelShader(k_ps_store_uint, sizeof(k_ps_store_uint), nullptr, &r.store[1]);
        if (SUCCEEDED(hr)) hr = device->CreatePixelShader(k_ps_store_sint, sizeof(k_ps_store_sint), nullptr, &r.store[2]);
        D3D11_BUFFER_DESC bd = {dxbc::kDispatchConstantsSize, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER,
                                D3D11_CPU_ACCESS_WRITE, 0, 0};
        if (SUCCEEDED(hr)) hr = device->CreateBuffer(&bd, nullptr, &r.constants);
        D3D11_RASTERIZER_DESC rd = {};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        if (SUCCEEDED(hr)) hr = device->CreateRasterizerState(&rd, &r.raster);
        D3D11_DEPTH_STENCIL_DESC dd = {};
        if (SUCCEEDED(hr)) hr = device->CreateDepthStencilState(&dd, &r.noDepth);
        r.ready = SUCCEEDED(hr);
        r.failed = !r.ready;
        if (r.failed) Log("compute emulation: creating our shaders failed (0x%08lx)", hr);
    }
    Resources* result = r.ready ? &r : nullptr;
    ReleaseSRWLockExclusive(&g_lock);
    return result;
}

// Where a placeholder UAV's writes go: a render target view onto its resource.
struct Target {
    ID3D11RenderTargetView* rtv = nullptr;
    UINT width = 1, height = 1, slices = 1;
};

UINT MipSize(UINT size, UINT mip) { return size >> mip ? size >> mip : 1; }

Target TargetFor(ID3D11Device* device, ID3D11UnorderedAccessView* uav) {
    Target t;
    ID3D11Resource* resource = nullptr;
    uav->GetResource(&resource);
    if (!resource) return t;
    D3D11_RESOURCE_DIMENSION dim;
    resource->GetType(&dim);
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud;
    uav->GetDesc(&ud);
    bool described = ud.ViewDimension != D3D11_UAV_DIMENSION_UNKNOWN;
    D3D11_RENDER_TARGET_VIEW_DESC rd = {};
    rd.Format = ud.Format;
    UINT bind = 0;
    switch (dim) {
        case D3D11_RESOURCE_DIMENSION_TEXTURE2D: {
            D3D11_TEXTURE2D_DESC td;
            static_cast<ID3D11Texture2D*>(resource)->GetDesc(&td);
            bind = td.BindFlags;
            if (ud.ViewDimension == D3D11_UAV_DIMENSION_TEXTURE2DARRAY) {
                rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
                rd.Texture2DArray = {ud.Texture2DArray.MipSlice, ud.Texture2DArray.FirstArraySlice,
                                     ud.Texture2DArray.ArraySize};
                t.width = MipSize(td.Width, ud.Texture2DArray.MipSlice);
                t.height = MipSize(td.Height, ud.Texture2DArray.MipSlice);
                t.slices = ud.Texture2DArray.ArraySize;
            } else {
                UINT mip = described ? ud.Texture2D.MipSlice : 0;
                rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                rd.Texture2D.MipSlice = mip;
                t.width = MipSize(td.Width, mip);
                t.height = MipSize(td.Height, mip);
                if (!described && td.ArraySize > 1) {
                    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
                    rd.Texture2DArray = {0, 0, td.ArraySize};
                    t.slices = td.ArraySize;
                }
                if (!described) rd.Format = td.Format;
            }
            break;
        }
        case D3D11_RESOURCE_DIMENSION_TEXTURE3D: {
            D3D11_TEXTURE3D_DESC td;
            static_cast<ID3D11Texture3D*>(resource)->GetDesc(&td);
            bind = td.BindFlags;
            UINT mip = described ? ud.Texture3D.MipSlice : 0;
            rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE3D;
            rd.Texture3D = {mip, described ? ud.Texture3D.FirstWSlice : 0, described ? ud.Texture3D.WSize : UINT(-1)};
            t.width = MipSize(td.Width, mip);
            t.height = MipSize(td.Height, mip);
            t.slices = described && ud.Texture3D.WSize != UINT(-1) ? ud.Texture3D.WSize : MipSize(td.Depth, mip);
            if (!described) rd.Format = td.Format;
            break;
        }
        case D3D11_RESOURCE_DIMENSION_TEXTURE1D: {
            D3D11_TEXTURE1D_DESC td;
            static_cast<ID3D11Texture1D*>(resource)->GetDesc(&td);
            bind = td.BindFlags;
            if (ud.ViewDimension == D3D11_UAV_DIMENSION_TEXTURE1DARRAY) {
                rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE1DARRAY;
                rd.Texture1DArray = {ud.Texture1DArray.MipSlice, ud.Texture1DArray.FirstArraySlice,
                                     ud.Texture1DArray.ArraySize};
                t.width = MipSize(td.Width, ud.Texture1DArray.MipSlice);
                t.slices = ud.Texture1DArray.ArraySize;
            } else {
                UINT mip = described ? ud.Texture1D.MipSlice : 0;
                rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE1D;
                rd.Texture1D.MipSlice = mip;
                t.width = MipSize(td.Width, mip);
                if (!described) rd.Format = td.Format;
            }
            break;
        }
        case D3D11_RESOURCE_DIMENSION_BUFFER: {
            D3D11_BUFFER_DESC bd;
            static_cast<ID3D11Buffer*>(resource)->GetDesc(&bd);
            bind = bd.BindFlags;
            rd.ViewDimension = D3D11_RTV_DIMENSION_BUFFER;
            rd.Buffer.FirstElement = ud.Buffer.FirstElement;
            rd.Buffer.NumElements = ud.Buffer.NumElements;
            t.width = ud.Buffer.NumElements;
            break;
        }
        default: break;
    }
    UINT size = sizeof(t.rtv);
    if (SUCCEEDED(uav->GetPrivateData(kRtvGuid, &size, &t.rtv)) && t.rtv) {
        t.rtv->Release();  // the UAV keeps it alive
    } else {
        t.rtv = nullptr;
        HRESULT hr = device->CreateRenderTargetView(resource, &rd, &t.rtv);
        if (SUCCEEDED(hr)) {
            uav->SetPrivateDataInterface(kRtvGuid, t.rtv);
            t.rtv->Release();
        } else if (FirstTime("emulation-rtv " + std::to_string(dim) + " " + std::to_string(rd.Format))) {
            Log("compute emulation: no render target view for a UAV (resource dimension %d, format %d, bind 0x%x): "
                "0x%08lx",
                dim, rd.Format, bind, hr);
        }
    }
    resource->Release();
    return t;
}

// A shader-readable copy of what a placeholder UAV points at, refreshed per dispatch.
ID3D11ShaderResourceView* SnapshotFor(ID3D11Device* device, ID3D11DeviceContext* context,
                                      ID3D11UnorderedAccessView* uav) {
    ID3D11ShaderResourceView* view = nullptr;
    UINT size = sizeof(view);
    ID3D11Resource* resource = nullptr;
    uav->GetResource(&resource);
    if (!resource) return nullptr;
    if (SUCCEEDED(uav->GetPrivateData(kSnapshotGuid, &size, &view)) && view) {
        view->Release();
    } else {
        view = nullptr;
        D3D11_RESOURCE_DIMENSION dim;
        resource->GetType(&dim);
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud;
        uav->GetDesc(&ud);
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = ud.Format;
        ID3D11Resource* copy = nullptr;
        HRESULT hr = E_FAIL;
        if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
            D3D11_TEXTURE2D_DESC td;
            static_cast<ID3D11Texture2D*>(resource)->GetDesc(&td);
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            td.MiscFlags = 0;
            hr = device->CreateTexture2D(&td, nullptr, reinterpret_cast<ID3D11Texture2D**>(&copy));
            if (ud.ViewDimension == D3D11_UAV_DIMENSION_TEXTURE2DARRAY) {
                sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
                sd.Texture2DArray = {ud.Texture2DArray.MipSlice, 1, ud.Texture2DArray.FirstArraySlice,
                                     ud.Texture2DArray.ArraySize};
            } else {
                sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                sd.Texture2D = {ud.Texture2D.MipSlice, 1};
            }
        } else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE3D) {
            D3D11_TEXTURE3D_DESC td;
            static_cast<ID3D11Texture3D*>(resource)->GetDesc(&td);
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            td.MiscFlags = 0;
            hr = device->CreateTexture3D(&td, nullptr, reinterpret_cast<ID3D11Texture3D**>(&copy));
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
            sd.Texture3D = {ud.Texture3D.MipSlice, 1};
        } else if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
            D3D11_BUFFER_DESC bd;
            static_cast<ID3D11Buffer*>(resource)->GetDesc(&bd);
            bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            bd.MiscFlags = 0;
            bd.StructureByteStride = 0;
            hr = device->CreateBuffer(&bd, nullptr, reinterpret_cast<ID3D11Buffer**>(&copy));
            sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            sd.Buffer.FirstElement = ud.Buffer.FirstElement;
            sd.Buffer.NumElements = ud.Buffer.NumElements;
        }
        if (SUCCEEDED(hr)) hr = device->CreateShaderResourceView(copy, &sd, &view);
        if (copy) copy->Release();  // the view keeps it alive
        if (SUCCEEDED(hr)) {
            uav->SetPrivateDataInterface(kSnapshotGuid, view);
            view->Release();
        } else {
            view = nullptr;
            if (FirstTime("emulation-snapshot")) Log("compute emulation: no snapshot of a UAV for reading: 0x%08lx", hr);
        }
    }
    if (view) {
        ID3D11Resource* copy = nullptr;
        view->GetResource(&copy);
        context->CopyResource(copy, resource);
        copy->Release();
    }
    resource->Release();
    return view;
}

// Shader resources, constant buffers and samplers of one stage.
struct StageBindings {
    UINT srvCount = 0, samplerCount = 0;
    ID3D11ShaderResourceView* srvs[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
    ID3D11Buffer* cbs[D3D10_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT] = {};
    ID3D11SamplerState* samplers[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};

    template <class GetSrv, class GetCb, class GetSampler>
    void Save(UINT srvs_, UINT samplers_, GetSrv getSrv, GetCb getCb, GetSampler getSampler) {
        srvCount = srvs_, samplerCount = samplers_;
        if (srvCount) getSrv(0, srvCount, srvs);
        getCb(0, D3D10_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, cbs);
        if (samplerCount) getSampler(0, samplerCount, samplers);
    }
    template <class SetSrv, class SetCb, class SetSampler>
    void Restore(SetSrv setSrv, SetCb setCb, SetSampler setSampler) {
        if (srvCount) setSrv(0, srvCount, srvs);
        setCb(0, D3D10_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, cbs);
        if (samplerCount) setSampler(0, samplerCount, samplers);
        for (auto* p : srvs)
            if (p) p->Release();
        for (auto* p : cbs)
            if (p) p->Release();
        for (auto* p : samplers)
            if (p) p->Release();
    }
};

// The pipeline state an emulation draw touches, saved and restored around it.
struct SavedState {
    ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    UINT viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ID3D11RasterizerState* raster = nullptr;
    ID3D11BlendState* blend = nullptr;
    FLOAT blendFactor[4];
    UINT sampleMask;
    ID3D11DepthStencilState* depth = nullptr;
    UINT stencilRef;
    ID3D11InputLayout* layout = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY topology;
    ID3D11VertexShader* vs = nullptr;
    ID3D11GeometryShader* gs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    StageBindings gsBindings, psBindings;

    void Save(ID3D11DeviceContext* c, UINT srvs, UINT samplers) {
        c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
        c->RSGetViewports(&viewportCount, viewports);
        c->RSGetState(&raster);
        c->OMGetBlendState(&blend, blendFactor, &sampleMask);
        c->OMGetDepthStencilState(&depth, &stencilRef);
        c->IAGetInputLayout(&layout);
        c->IAGetPrimitiveTopology(&topology);
        c->VSGetShader(&vs, nullptr, nullptr);
        c->GSGetShader(&gs, nullptr, nullptr);
        c->PSGetShader(&ps, nullptr, nullptr);
        gsBindings.Save(
            srvs, samplers, [c](UINT s, UINT n, ID3D11ShaderResourceView** v) { c->GSGetShaderResources(s, n, v); },
            [c](UINT s, UINT n, ID3D11Buffer** v) { c->GSGetConstantBuffers(s, n, v); },
            [c](UINT s, UINT n, ID3D11SamplerState** v) { c->GSGetSamplers(s, n, v); });
        psBindings.Save(
            srvs, samplers, [c](UINT s, UINT n, ID3D11ShaderResourceView** v) { c->PSGetShaderResources(s, n, v); },
            [c](UINT s, UINT n, ID3D11Buffer** v) { c->PSGetConstantBuffers(s, n, v); },
            [c](UINT s, UINT n, ID3D11SamplerState** v) { c->PSGetSamplers(s, n, v); });
    }

    void Restore(ID3D11DeviceContext* c) {
        c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);
        c->RSSetViewports(viewportCount, viewports);
        c->RSSetState(raster);
        c->OMSetBlendState(blend, blendFactor, sampleMask);
        c->OMSetDepthStencilState(depth, stencilRef);
        c->IASetInputLayout(layout);
        c->IASetPrimitiveTopology(topology);
        c->VSSetShader(vs, nullptr, 0);
        c->GSSetShader(gs, nullptr, 0);
        c->PSSetShader(ps, nullptr, 0);
        gsBindings.Restore(
            [c](UINT s, UINT n, ID3D11ShaderResourceView** v) { c->GSSetShaderResources(s, n, v); },
            [c](UINT s, UINT n, ID3D11Buffer** v) { c->GSSetConstantBuffers(s, n, v); },
            [c](UINT s, UINT n, ID3D11SamplerState** v) { c->GSSetSamplers(s, n, v); });
        psBindings.Restore(
            [c](UINT s, UINT n, ID3D11ShaderResourceView** v) { c->PSSetShaderResources(s, n, v); },
            [c](UINT s, UINT n, ID3D11Buffer** v) { c->PSSetConstantBuffers(s, n, v); },
            [c](UINT s, UINT n, ID3D11SamplerState** v) { c->PSSetSamplers(s, n, v); });
        auto release = [](IUnknown* p) {
            if (p) p->Release();
        };
        for (auto* p : rtvs) release(p);
        IUnknown* rest[] = {dsv, raster, blend, depth, layout, vs, gs, ps};
        for (IUnknown* p : rest) release(p);
    }
};

void SetCommonState(ID3D11DeviceContext* c, Resources* r, ID3D11RenderTargetView* target, UINT width, UINT height) {
    D3D11_VIEWPORT viewport = {0, 0, float(width), float(height), 0, 1};
    c->OMSetRenderTargets(1, &target, nullptr);
    c->RSSetViewports(1, &viewport);
    c->RSSetState(r->raster);
    c->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    c->OMSetDepthStencilState(r->noDepth, 0);
    c->IASetInputLayout(nullptr);
}

bool Generic(ID3D11DeviceContext* context, ContextState* state, Resources* r, const Emulation& e, UINT x, UINT y,
             UINT z) {
    const dxbc::ComputeProgram& p = e.program;
    unsigned long long threads = 1ull * x * y * z * p.threads[0] * p.threads[1] * p.threads[2];
    if (!threads || threads > 0x7fffffff) return false;

    // UAVs the shader also reads are read from a copy taken now.
    std::vector<std::pair<UINT, ID3D11ShaderResourceView*>> snapshots;
    UINT srvCount = p.srvSlots;
    for (const auto& read : p.uavReads) {
        const void* uav = state->csUavs[read.first];
        if (!uav || !IsDummy(uav)) return false;
        ID3D11ShaderResourceView* view =
            SnapshotFor(state->device, context, static_cast<ID3D11UnorderedAccessView*>(const_cast<void*>(uav)));
        if (!view) return false;
        snapshots.push_back({read.second, view});
        srvCount = std::max(srvCount, read.second + 1);
    }

    ID3D11ShaderResourceView* srvs[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
    ID3D11Buffer* cbs[D3D10_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT] = {};
    ID3D11SamplerState* samplers[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
    if (p.srvSlots) context->CSGetShaderResources(0, p.srvSlots, srvs);
    context->CSGetConstantBuffers(0, D3D10_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, cbs);
    if (p.samplerSlots) context->CSGetSamplers(0, p.samplerSlots, samplers);
    ID3D11Buffer* gameCb = cbs[p.dispatchSlot];
    cbs[p.dispatchSlot] = r->constants;
    for (const auto& s : snapshots) srvs[s.first] = s.second;

    bool done = true;
    unsigned fastPasses = 0;
    for (size_t i = 0; i < p.passes.size(); i++) {
        const dxbc::ComputePass& pass = p.passes[i];
        const void* uav = state->csUavs[pass.uavSlot];
        if (!uav || !IsDummy(uav) || !e.gs[i]) {
            done = false;
            continue;
        }
        Target t = TargetFor(state->device, static_cast<ID3D11UnorderedAccessView*>(const_cast<void*>(uav)));
        if (!t.rtv) {
            done = false;
            continue;
        }
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (FAILED(context->Map(r->constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            done = false;
            continue;
        }
        uint32_t c[16] = {x, y, z, 0};
        float scale[4] = {2.0f / t.width, -2.0f / t.height, -1.0f, 1.0f};
        memcpy(&c[4], scale, sizeof(scale));
        c[8] = t.slices;
        c[12] = t.width, c[13] = t.height, c[14] = t.slices, c[15] = 1;
        memcpy(mapped.pData, c, sizeof(c));
        context->Unmap(r->constants, 0);

        SavedState saved;
        saved.Save(context, srvCount, p.samplerSlots);
        SetCommonState(context, r, t.rtv, t.width, t.height);
        if (e.ps[i] && z == 1) {
            // Fast path: one pixel per thread over the whole texture.
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->VSSetShader(r->fullscreen, nullptr, 0);
            context->GSSetShader(nullptr, nullptr, 0);
            context->PSSetShader(e.ps[i], nullptr, 0);
            if (srvCount) context->PSSetShaderResources(0, srvCount, srvs);
            context->PSSetConstantBuffers(0, D3D10_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, cbs);
            if (p.samplerSlots) context->PSSetSamplers(0, p.samplerSlots, samplers);
            context->Draw(3, 0);
            fastPasses++;
        } else {
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
            context->VSSetShader(r->thread, nullptr, 0);
            context->GSSetShader(e.gs[i], nullptr, 0);
            context->PSSetShader(r->store[int(pass.type)], nullptr, 0);
            if (srvCount) context->GSSetShaderResources(0, srvCount, srvs);
            context->GSSetConstantBuffers(0, D3D10_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, cbs);
            if (p.samplerSlots) context->GSSetSamplers(0, p.samplerSlots, samplers);
            context->Draw(UINT(threads), 0);
        }
        saved.Restore(context);
    }

    cbs[p.dispatchSlot] = gameCb;
    for (const auto& s : snapshots) srvs[s.first] = nullptr;  // not referenced by us
    for (auto* v : srvs)
        if (v) v->Release();
    for (auto* b : cbs)
        if (b) b->Release();
    for (auto* s : samplers)
        if (s) s->Release();
    if (done && FirstTime("generic-path " + std::to_string(state->csTag)))
        Log("compute shader #%u: %u of %u passes ran as full-screen pixel shaders", state->csTag, fastPasses,
            unsigned(p.passes.size()));
    return done;
}

std::shared_ptr<Emulation> Find(const void* shader) {
    AcquireSRWLockShared(&g_lock);
    auto it = g_emulations.find(shader);
    std::shared_ptr<Emulation> e = it == g_emulations.end() ? nullptr : it->second;
    ReleaseSRWLockShared(&g_lock);
    return e;
}

}  // namespace

void PrepareComputeEmulation(ID3D11Device* device, const void* shader, const void* code, size_t size, unsigned id,
                             int targetMinor) {
    auto e = std::make_shared<Emulation>();
    std::vector<uint8_t> signatures[3] = {
        {k_gs_signature_float, k_gs_signature_float + sizeof(k_gs_signature_float)},
        {k_gs_signature_uint, k_gs_signature_uint + sizeof(k_gs_signature_uint)},
        {k_gs_signature_sint, k_gs_signature_sint + sizeof(k_gs_signature_sint)},
    };
    std::vector<uint8_t> psSignatures[3] = {
        {k_ps_signature_float, k_ps_signature_float + sizeof(k_ps_signature_float)},
        {k_ps_signature_uint, k_ps_signature_uint + sizeof(k_ps_signature_uint)},
        {k_ps_signature_sint, k_ps_signature_sint + sizeof(k_ps_signature_sint)},
    };
    e->program = dxbc::ComputeToGS(code, size, targetMinor, signatures, psSignatures);
    if (!e->program.ok) {
        Log("compute shader #%u can't be replayed: %s", id, e->program.message.c_str());
        return;
    }
    for (const dxbc::ComputePass& pass : e->program.passes) {
        ID3D11GeometryShader* gs = nullptr;
        HRESULT hr = device->CreateGeometryShader(pass.gs.data(), pass.gs.size(), nullptr, &gs);
        if (FAILED(hr)) {
            Log("compute shader #%u: the runtime rejected its geometry shader for u%u (0x%08lx)", id, pass.uavSlot,
                hr);
            return;
        }
        e->gs.push_back(gs);
        ID3D11PixelShader* ps = nullptr;
        if (!pass.ps.empty() && FAILED(device->CreatePixelShader(pass.ps.data(), pass.ps.size(), nullptr, &ps))) {
            Log("compute shader #%u: the runtime rejected its pixel shader for u%u; using points", id, pass.uavSlot);
            ps = nullptr;
        }
        e->ps.push_back(ps);
    }
    Log("compute shader #%u will be replayed (%s)", id, e->program.message.c_str());
    AcquireSRWLockExclusive(&g_lock);
    g_emulations[shader] = e;
    ReleaseSRWLockExclusive(&g_lock);
}

void ForgetComputeEmulation(const void* shader) {
    AcquireSRWLockExclusive(&g_lock);
    g_emulations.erase(shader);
    ReleaseSRWLockExclusive(&g_lock);
}

bool EmulateDispatch(ID3D11DeviceContext* context, ContextState* state, UINT x, UINT y, UINT z) {
    if (!state->csShader) return false;
    std::shared_ptr<Emulation> e = Find(state->csShader);
    if (!e) return false;
    Resources* r = ResourcesFor(state->device);
    if (!r) return false;
    bool done = Generic(context, state, r, *e, x, y, z);
    if (done) {
        g_stats.dispatchesEmulated++;
        if (FirstTime("emulated " + std::to_string(state->csTag)))
            Log("running compute shader #%u (%u x %u x %u groups)", state->csTag, x, y, z);
    }
    return done;
}
