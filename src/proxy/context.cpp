// ID3D11DeviceContext handling: keep placeholder objects away from the real runtime
// and skip work the hardware can't do (tessellation, compute, UAVs).
//
// Contexts are wrapped rather than vtable-patched: the D3D11 runtime keeps a private,
// per-context function table and rewrites its draw entries while running (switching
// between internal fast and slow paths), which silently drops patches on them.
#include <cstdio>
#include <string>
#include <unordered_map>

#include "proxy.h"

namespace {

void STDMETHODCALLTYPE Hk_OMSetRenderTargets(ID3D11DeviceContext* self, UINT count,
                                             ID3D11RenderTargetView* const* views, ID3D11DepthStencilView* depth);

void SetStage(ContextState* state, unsigned stage, const void*& shader) {
    if (!state || !state->downlevel) return;
    if (IsDummy(shader)) {
        state->dummyStages |= stage;
        if (stage == kCS) {
            state->csTag = DummyTag(shader);
            state->csShader = shader;
        }
        shader = nullptr;
    } else {
        state->dummyStages &= ~stage;
    }
}

// True when the current draw can't be executed on this device.
bool SkipDraw(ContextState* state) {
    if (!state || !state->downlevel) return false;
    if (!state->patchTopology && !(state->dummyStages & (kVS | kPS | kGS | kHS | kDS))) return false;
    g_stats.drawsDropped++;
    if (state->patchTopology || (state->dummyStages & (kHS | kDS))) {
        if (FirstTime("skip-tess")) Log("skipping tessellated draws (no tessellator on this hardware)");
    } else if (FirstTime("skip-shader")) {
        Log("skipping draws that use a shader we couldn't convert");
    }
    return true;
}

#define DEFINE_SET_SHADER(Stage, Iface, Bit)                                                                   \
    void STDMETHODCALLTYPE Hk_##Stage##SetShader(ID3D11DeviceContext* self, Iface* shader,                     \
                                                 ID3D11ClassInstance* const* instances, UINT count) {          \
        auto orig = Orig<decltype(&Hk_##Stage##SetShader)>(self, g_slots.ctx##Stage##SetShader);               \
        ContextState* state = FindContext(self);                                                               \
        const void* s = shader;                                                                                \
        SetStage(state, Bit, s);                                                                               \
        if (state && state->downlevel) {                                                                       \
            instances = nullptr;                                                                               \
            count = 0;                                                                                         \
        }                                                                                                      \
        orig(self, (Iface*)s, instances, count);                                                               \
    }

DEFINE_SET_SHADER(VS, ID3D11VertexShader, kVS)
DEFINE_SET_SHADER(PS, ID3D11PixelShader, kPS)
DEFINE_SET_SHADER(GS, ID3D11GeometryShader, kGS)
DEFINE_SET_SHADER(HS, ID3D11HullShader, kHS)
DEFINE_SET_SHADER(DS, ID3D11DomainShader, kDS)
DEFINE_SET_SHADER(CS, ID3D11ComputeShader, kCS)

void STDMETHODCALLTYPE Hk_IASetPrimitiveTopology(ID3D11DeviceContext* self, D3D11_PRIMITIVE_TOPOLOGY topology) {
    auto orig = Orig<decltype(&Hk_IASetPrimitiveTopology)>(self, g_slots.ctxIASetPrimitiveTopology);
    ContextState* state = FindContext(self);
    if (state && state->downlevel) {
        state->patchTopology = topology >= D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST;
        if (state->patchTopology) return;
    }
    orig(self, topology);
}

// Runs a draw unless it can't execute here, keeping depth copies current around it.
template <class F>
void Draw(ID3D11DeviceContext* context, F draw) {
    ContextState* state = FindContext(context);
    g_stats.draws++;
    if (Tracing() && FirstTime("trace-draw"))
        Log("trace: draw on context %p (state %p, depth state %p)", (void*)context, (void*)state,
            state ? state->depth : nullptr);
    if (SkipDraw(state)) return;
    DepthBeforeDraw(context, state);
    draw();
    DepthAfterDraw(state);
}

void STDMETHODCALLTYPE Hk_Draw(ID3D11DeviceContext* self, UINT count, UINT start) {
    Draw(self, [&] { Orig<decltype(&Hk_Draw)>(self, g_slots.ctxDraw)(self, count, start); });
}

void STDMETHODCALLTYPE Hk_DrawIndexed(ID3D11DeviceContext* self, UINT count, UINT start, INT base) {
    Draw(self, [&] { Orig<decltype(&Hk_DrawIndexed)>(self, g_slots.ctxDrawIndexed)(self, count, start, base); });
}

void STDMETHODCALLTYPE Hk_DrawInstanced(ID3D11DeviceContext* self, UINT count, UINT instances, UINT start,
                                        UINT startInstance) {
    Draw(self, [&] {
        Orig<decltype(&Hk_DrawInstanced)>(self, g_slots.ctxDrawInstanced)(self, count, instances, start,
                                                                          startInstance);
    });
}

void STDMETHODCALLTYPE Hk_DrawIndexedInstanced(ID3D11DeviceContext* self, UINT count, UINT instances, UINT start,
                                               INT base, UINT startInstance) {
    Draw(self, [&] {
        Orig<decltype(&Hk_DrawIndexedInstanced)>(self, g_slots.ctxDrawIndexedInstanced)(self, count, instances, start,
                                                                                       base, startInstance);
    });
}

void STDMETHODCALLTYPE Hk_DrawAuto(ID3D11DeviceContext* self) {
    Draw(self, [&] { Orig<decltype(&Hk_DrawAuto)>(self, g_slots.ctxDrawAuto)(self); });
}

// Indirect draws need feature level 11: read the arguments back and draw directly.
bool ReadArgs(ID3D11DeviceContext* self, ContextState* state, ID3D11Buffer* args, UINT offset, UINT* out, UINT n) {
    if (!state->immediate) {
        if (FirstTime("indirect-deferred")) Log("indirect draw on a deferred context: skipped");
        return false;
    }
    if (!state->argsStaging) {
        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth = 32;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(state->device->CreateBuffer(&desc, nullptr, &state->argsStaging))) return false;
    }
    D3D11_BOX box = {offset, 0, 0, offset + n * 4, 1, 1};
    self->CopySubresourceRegion(state->argsStaging, 0, 0, 0, 0, args, 0, &box);
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(self->Map(state->argsStaging, 0, D3D11_MAP_READ, 0, &mapped))) return false;
    memcpy(out, mapped.pData, n * 4);
    self->Unmap(state->argsStaging, 0);
    g_stats.indirectEmulated++;
    if (FirstTime("indirect")) Log("emulating indirect draws with a CPU read-back");
    return true;
}

void STDMETHODCALLTYPE Hk_DrawInstancedIndirect(ID3D11DeviceContext* self, ID3D11Buffer* args, UINT offset) {
    ContextState* state = FindContext(self);
    if (!state || !state->downlevel)
        return Orig<decltype(&Hk_DrawInstancedIndirect)>(self, g_slots.ctxDrawInstancedIndirect)(self, args, offset);
    UINT a[4];
    if (SkipDraw(state) || !ReadArgs(self, state, args, offset, a, 4)) return;
    DepthBeforeDraw(self, state);
    Orig<decltype(&Hk_DrawInstanced)>(self, g_slots.ctxDrawInstanced)(self, a[0], a[1], a[2], a[3]);
    DepthAfterDraw(state);
}

void STDMETHODCALLTYPE Hk_DrawIndexedInstancedIndirect(ID3D11DeviceContext* self, ID3D11Buffer* args, UINT offset) {
    ContextState* state = FindContext(self);
    if (!state || !state->downlevel)
        return Orig<decltype(&Hk_DrawIndexedInstancedIndirect)>(self, g_slots.ctxDrawIndexedInstancedIndirect)(
            self, args, offset);
    UINT a[5];
    if (SkipDraw(state) || !ReadArgs(self, state, args, offset, a, 5)) return;
    DepthBeforeDraw(self, state);
    Orig<decltype(&Hk_DrawIndexedInstanced)>(self, g_slots.ctxDrawIndexedInstanced)(self, a[0], a[1], a[2], INT(a[3]),
                                                                                   a[4]);
    DepthAfterDraw(state);
}

// Describes what a placeholder UAV points at, for the log.
std::string DescribeUav(const void* view) {
    if (!view || !IsDummy(view)) return "";
    ID3D11Resource* resource = nullptr;
    static_cast<ID3D11UnorderedAccessView*>(const_cast<void*>(view))->GetResource(&resource);
    if (!resource) return "?";
    D3D11_RESOURCE_DIMENSION dim;
    resource->GetType(&dim);
    char buf[128] = "?";
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC d;
        static_cast<ID3D11Texture2D*>(resource)->GetDesc(&d);
        snprintf(buf, sizeof(buf), "2D %ux%u format %d mips %u", d.Width, d.Height, d.Format, d.MipLevels);
    } else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE3D) {
        D3D11_TEXTURE3D_DESC d;
        static_cast<ID3D11Texture3D*>(resource)->GetDesc(&d);
        snprintf(buf, sizeof(buf), "3D %ux%ux%u format %d", d.Width, d.Height, d.Depth, d.Format);
    } else if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
        D3D11_BUFFER_DESC d;
        static_cast<ID3D11Buffer*>(resource)->GetDesc(&d);
        snprintf(buf, sizeof(buf), "buffer %u bytes, stride %u", d.ByteWidth, d.StructureByteStride);
    }
    resource->Release();
    return buf;
}

bool SkipDispatch(ID3D11DeviceContext* context, UINT x, UINT y, UINT z) {
    ContextState* state = FindContext(context);
    if (!state || !state->downlevel || !(state->dummyStages & kCS)) return false;
    g_stats.dispatchesDropped++;
    if (FirstTime("skip-cs " + std::to_string(state->csTag))) {
        std::string writes;
        for (int i = 0; i < 8; i++) {
            std::string d = DescribeUav(state->csUavs[i]);
            if (!d.empty()) writes += " u" + std::to_string(i) + "=[" + d + "]";
        }
        Log("skipping compute shader #%u (%u x %u x %u groups) at frame %u; writes:%s", state->csTag, x, y, z,
            CurrentFrame(), writes.empty() ? " nothing known" : writes.c_str());
    }
    return true;
}

void STDMETHODCALLTYPE Hk_Dispatch(ID3D11DeviceContext* self, UINT x, UINT y, UINT z) {
    ContextState* state = FindContext(self);
    if (state && state->downlevel && (state->dummyStages & kCS) && EmulateDispatch(self, state, x, y, z)) return;
    if (SkipDispatch(self, x, y, z)) return;
    Orig<decltype(&Hk_Dispatch)>(self, g_slots.ctxDispatch)(self, x, y, z);
}

void STDMETHODCALLTYPE Hk_DispatchIndirect(ID3D11DeviceContext* self, ID3D11Buffer* args, UINT offset) {
    ContextState* state = FindContext(self);
    if (state && state->downlevel) {
        UINT a[3];
        if ((state->dummyStages & kCS) && ReadArgs(self, state, args, offset, a, 3) &&
            EmulateDispatch(self, state, a[0], a[1], a[2]))
            return;
        g_stats.dispatchesDropped++;
        if (FirstTime("skip-cs-indirect")) Log("skipping an indirect compute dispatch");
        return;
    }
    Orig<decltype(&Hk_DispatchIndirect)>(self, g_slots.ctxDispatchIndirect)(self, args, offset);
}

void STDMETHODCALLTYPE Hk_CSSetUnorderedAccessViews(ID3D11DeviceContext* self, UINT start, UINT count,
                                                    ID3D11UnorderedAccessView* const* views, const UINT* counts) {
    auto orig = Orig<decltype(&Hk_CSSetUnorderedAccessViews)>(self, g_slots.ctxCSSetUnorderedAccessViews);
    ContextState* state = FindContext(self);
    if (state && state->downlevel && !g_config.keepCompute) {
        // Below feature level 11 only slot 0 exists (compute shader 4.x); placeholders are dropped.
        for (UINT i = 0; i < count && start + i < 8; i++) state->csUavs[start + i] = views ? views[i] : nullptr;
        if (start > 0 || !count) return;
        ID3D11UnorderedAccessView* first = views && !IsDummy(views[0]) ? views[0] : nullptr;
        return orig(self, 0, 1, &first, counts);
    }
    orig(self, start, count, views, counts);
}

void STDMETHODCALLTYPE Hk_OMSetRenderTargetsAndUnorderedAccessViews(
    ID3D11DeviceContext* self, UINT numViews, ID3D11RenderTargetView* const* views, ID3D11DepthStencilView* depth,
    UINT uavStart, UINT numUavs, ID3D11UnorderedAccessView* const* uavs, const UINT* counts) {
    ContextState* state = FindContext(self);
    if (state && state->downlevel) {
        // No pixel shader UAVs below feature level 11: bind just the render targets.
        if (numUavs && numUavs != D3D11_KEEP_UNORDERED_ACCESS_VIEWS && FirstTime("ps-uav"))
            Log("pixel shader UAVs are not available; binding render targets only");
        if (numViews != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) Hk_OMSetRenderTargets(self, numViews, views, depth);
        return;
    }
    Orig<decltype(&Hk_OMSetRenderTargetsAndUnorderedAccessViews)>(
        self, g_slots.ctxOMSetRenderTargetsAndUnorderedAccessViews)(self, numViews, views, depth, uavStart, numUavs,
                                                                    uavs, counts);
}

void STDMETHODCALLTYPE Hk_ClearUnorderedAccessViewUint(ID3D11DeviceContext* self, ID3D11UnorderedAccessView* view,
                                                       const UINT values[4]) {
    if (IsDummy(view)) return;
    Orig<decltype(&Hk_ClearUnorderedAccessViewUint)>(self, g_slots.ctxClearUnorderedAccessViewUint)(self, view, values);
}

void STDMETHODCALLTYPE Hk_ClearUnorderedAccessViewFloat(ID3D11DeviceContext* self, ID3D11UnorderedAccessView* view,
                                                        const FLOAT values[4]) {
    if (IsDummy(view)) return;
    Orig<decltype(&Hk_ClearUnorderedAccessViewFloat)>(self, g_slots.ctxClearUnorderedAccessViewFloat)(self, view,
                                                                                                     values);
}

void STDMETHODCALLTYPE Hk_CopyStructureCount(ID3D11DeviceContext* self, ID3D11Buffer* dst, UINT offset,
                                             ID3D11UnorderedAccessView* src) {
    if (IsDummy(src)) return;
    Orig<decltype(&Hk_CopyStructureCount)>(self, g_slots.ctxCopyStructureCount)(self, dst, offset, src);
}

void ResetState(ID3D11DeviceContext* context) {
    if (ContextState* state = FindContext(context)) {
        state->dummyStages = 0;
        state->patchTopology = false;
        DepthReset(state);
    }
}

void STDMETHODCALLTYPE Hk_OMSetRenderTargets(ID3D11DeviceContext* self, UINT count,
                                             ID3D11RenderTargetView* const* views, ID3D11DepthStencilView* depth) {
    Orig<decltype(&Hk_OMSetRenderTargets)>(self, g_slots.ctxOMSetRenderTargets)(self, count, views, depth);
    DepthOnSetDepthView(self, depth);
}

void STDMETHODCALLTYPE Hk_ClearDepthStencilView(ID3D11DeviceContext* self, ID3D11DepthStencilView* view, UINT flags,
                                                FLOAT depth, UINT8 stencil) {
    DepthOnClearDepthView(view);
    Orig<decltype(&Hk_ClearDepthStencilView)>(self, g_slots.ctxClearDepthStencilView)(self, view, flags, depth,
                                                                                     stencil);
}

void STDMETHODCALLTYPE Hk_CopyResource(ID3D11DeviceContext* self, ID3D11Resource* dst, ID3D11Resource* src) {
    DepthOnWrite(dst, "CopyResource");
    Orig<decltype(&Hk_CopyResource)>(self, g_slots.ctxCopyResource)(self, dst, src);
}

void STDMETHODCALLTYPE Hk_CopySubresourceRegion(ID3D11DeviceContext* self, ID3D11Resource* dst, UINT dstSub, UINT x,
                                                UINT y, UINT z, ID3D11Resource* src, UINT srcSub,
                                                const D3D11_BOX* box) {
    DepthOnWrite(dst, "CopySubresourceRegion");
    Orig<decltype(&Hk_CopySubresourceRegion)>(self, g_slots.ctxCopySubresourceRegion)(self, dst, dstSub, x, y, z, src,
                                                                                     srcSub, box);
}

#define DEFINE_SET_RESOURCES(Stage, Bit)                                                                       \
    void STDMETHODCALLTYPE Hk_##Stage##SetShaderResources(ID3D11DeviceContext* self, UINT start, UINT count,   \
                                                          ID3D11ShaderResourceView* const* views) {            \
        auto orig = Orig<decltype(&Hk_##Stage##SetShaderResources)>(self, g_slots.ctx##Stage##SetShaderResources); \
        ID3D11ShaderResourceView* actual[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT];                        \
        if (count > D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) return orig(self, start, count, views);      \
        DepthOnSetShaderResources(self, Bit, start, count, views, actual);                                     \
        orig(self, start, count, actual);                                                                      \
    }

DEFINE_SET_RESOURCES(VS, kVS)
DEFINE_SET_RESOURCES(PS, kPS)
DEFINE_SET_RESOURCES(GS, kGS)
DEFINE_SET_RESOURCES(CS, kCS)

void STDMETHODCALLTYPE Hk_ClearState(ID3D11DeviceContext* self) {
    Orig<decltype(&Hk_ClearState)>(self, g_slots.ctxClearState)(self);
    ResetState(self);
}

HRESULT STDMETHODCALLTYPE Hk_FinishCommandList(ID3D11DeviceContext* self, BOOL restore, ID3D11CommandList** out) {
    HRESULT hr = Orig<decltype(&Hk_FinishCommandList)>(self, g_slots.ctxFinishCommandList)(self, restore, out);
    if (!restore) ResetState(self);
    return hr;
}

void STDMETHODCALLTYPE Hk_ExecuteCommandList(ID3D11DeviceContext* self, ID3D11CommandList* list, BOOL restore) {
    Orig<decltype(&Hk_ExecuteCommandList)>(self, g_slots.ctxExecuteCommandList)(self, list, restore);
    if (!restore) ResetState(self);
}

// What the game gets instead of a real context. Methods we care about go to the Hk_
// functions above; everything else is forwarded unchanged.
class ContextWrapper final : public ID3D11DeviceContext4 {
public:
    explicit ContextWrapper(ID3D11DeviceContext* real) : real_(real) {
        // Same object, so these share its reference count: release right away, keep the pointers.
        if (SUCCEEDED(real->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&real1_))) real1_->Release();
        if (SUCCEEDED(real->QueryInterface(__uuidof(ID3D11DeviceContext2), (void**)&real2_))) real2_->Release();
        if (SUCCEEDED(real->QueryInterface(__uuidof(ID3D11DeviceContext3), (void**)&real3_))) real3_->Release();
        if (SUCCEEDED(real->QueryInterface(__uuidof(ID3D11DeviceContext4), (void**)&real4_))) real4_->Release();
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11DeviceChild) ||
            riid == __uuidof(ID3D11DeviceContext) || (riid == __uuidof(ID3D11DeviceContext1) && real1_) ||
            (riid == __uuidof(ID3D11DeviceContext2) && real2_) || (riid == __uuidof(ID3D11DeviceContext3) && real3_) ||
            (riid == __uuidof(ID3D11DeviceContext4) && real4_)) {
            AddRef();
            *out = this;
            return S_OK;
        }
        return real_->QueryInterface(riid, out);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return real_->AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override;

#include "context_wrapper.inc"

    ID3D11DeviceContext* real_;

private:
    ID3D11DeviceContext1* real1_ = nullptr;
    ID3D11DeviceContext2* real2_ = nullptr;
    ID3D11DeviceContext3* real3_ = nullptr;
    ID3D11DeviceContext4* real4_ = nullptr;
};

SRWLOCK g_wrapLock = SRWLOCK_INIT;
std::unordered_map<void*, ContextWrapper*> g_wrappers;

ULONG STDMETHODCALLTYPE ContextWrapper::Release() {
    void* real = real_;
    ULONG refs = real_->Release();
    if (!refs) {
        AcquireSRWLockExclusive(&g_wrapLock);
        g_wrappers.erase(real);
        ReleaseSRWLockExclusive(&g_wrapLock);
        delete this;
    }
    return refs;
}

}  // namespace

ID3D11DeviceContext* WrapContext(ID3D11DeviceContext* real) {
    if (!real) return nullptr;
    AcquireSRWLockExclusive(&g_wrapLock);
    ContextWrapper*& wrapper = g_wrappers[real];
    if (!wrapper) wrapper = new ContextWrapper(real);
    ID3D11DeviceContext* result = wrapper;
    ReleaseSRWLockExclusive(&g_wrapLock);
    return result;
}
