// Read-only depth views for feature level 10.
//
// DX11 games bind a depth buffer read-only for depth testing while shaders read the
// same buffer as a texture (deferred lighting, soft particles, decals). Feature level 10
// has no read-only depth views; with a writable view bound, the runtime unbinds the
// texture and lighting reads zeros. For depth buffers the game wanted read-only views
// of, shaders get a copy instead, refreshed before a draw whenever the depth changed.
#include <cstdarg>
#include <cstdio>
#include <unordered_map>

#include "proxy.h"

namespace {

// {3f1c2a90-7b4e-4c1d-9e55-0d8a6b2f71c4}
const GUID kLinkGuid = {0x3f1c2a90, 0x7b4e, 0x4c1d, {0x9e, 0x55, 0x0d, 0x8a, 0x6b, 0x2f, 0x71, 0xc4}};

class RefCounted : public IUnknown {
public:
    virtual ~RefCounted() = default;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IUnknown)) {
            AddRef();
            *out = this;
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    // Not inlined: GCC otherwise mis-sizes "delete this" across the virtual destructor.
    __attribute__((noinline)) ULONG STDMETHODCALLTYPE Release() override {
        ULONG refs = InterlockedDecrement(&refs_);
        if (!refs) delete this;
        return refs;
    }

private:
    LONG refs_ = 1;
};

SRWLOCK g_lock = SRWLOCK_INIT;
std::atomic<unsigned> g_nextId{1};
unsigned g_drawsSinceTrace = 0;

// Logs one line of the depth trace, with the number of draws since the previous line.
void Trace(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Trace(const char* fmt, ...) {
    if (!Tracing()) return;
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    Log("trace: [%u draws] %s", g_drawsSinceTrace, buf);
    g_drawsSinceTrace = 0;
}

struct DepthTexture final : RefCounted {
    unsigned id = 0;
    ID3D11Texture2D* source = nullptr;  // cleared when the game's texture is destroyed
    D3D11_TEXTURE2D_DESC desc = {};
    ID3D11Texture2D* copy = nullptr;
    bool readOnlyUse = false;
    std::atomic<bool> dirty{true};
    ~DepthTexture() override {
        if (copy) copy->Release();
    }
};

struct ViewInfo final : RefCounted {
    void* view = nullptr;
    DepthTexture* texture = nullptr;
    bool srv = false;
    bool readOnlySubstitute = false;
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    ID3D11ShaderResourceView* twin = nullptr;
    ~ViewInfo() override;
};

std::unordered_map<void*, DepthTexture*> g_textures;
std::unordered_map<void*, ViewInfo*> g_views;

ViewInfo::~ViewInfo() {
    AcquireSRWLockExclusive(&g_lock);
    g_views.erase(view);
    ReleaseSRWLockExclusive(&g_lock);
    if (twin) twin->Release();
    texture->Release();
}

// Attached to the game's texture as private data: D3D releases it when the texture dies.
struct TextureLink final : RefCounted {
    DepthTexture* texture;
    explicit TextureLink(DepthTexture* t) : texture(t) {}
    ~TextureLink() override {
        AcquireSRWLockExclusive(&g_lock);
        g_textures.erase(texture->source);
        texture->source = nullptr;
        ReleaseSRWLockExclusive(&g_lock);
        texture->Release();
    }
};

DepthTexture* FindTexture(void* resource) {
    AcquireSRWLockShared(&g_lock);
    auto it = g_textures.find(resource);
    DepthTexture* t = it == g_textures.end() ? nullptr : it->second;
    ReleaseSRWLockShared(&g_lock);
    return t;
}

ViewInfo* FindView(void* view) {
    if (!view) return nullptr;
    AcquireSRWLockShared(&g_lock);
    auto it = g_views.find(view);
    ViewInfo* v = it == g_views.end() ? nullptr : it->second;
    ReleaseSRWLockShared(&g_lock);
    return v;
}

ViewInfo* TrackView(ID3D11DeviceChild* view, DepthTexture* texture) {
    ViewInfo* info = new ViewInfo;
    info->view = view;
    info->texture = texture;
    texture->AddRef();
    AcquireSRWLockExclusive(&g_lock);
    g_views[view] = info;
    ReleaseSRWLockExclusive(&g_lock);
    view->SetPrivateDataInterface(kLinkGuid, info);
    info->Release();  // the view's private data now owns it
    return info;
}

struct Binding {
    unsigned stage, slot;
    DepthTexture* texture;  // holds a reference
};

struct DepthState {
    std::vector<Binding> bound;
    DepthTexture* writable = nullptr;  // depth buffer bound for writing (holds a reference)
};

DepthState* State(ContextState* state) {
    if (!state || !state->downlevel) return nullptr;
    if (!state->depth) state->depth = new DepthState;
    return static_cast<DepthState*>(state->depth);
}

// Separate from g_lock: creating the copy calls back into our device hooks, which take g_lock.
SRWLOCK g_createLock = SRWLOCK_INIT;

ID3D11ShaderResourceView* TwinFor(ID3D11Device* device, ViewInfo* view) {
    DepthTexture* t = view->texture;
    if (!t->readOnlyUse || !t->source) return nullptr;
    if (view->twin) return view->twin;
    AcquireSRWLockExclusive(&g_createLock);
    if (!t->copy) {
        D3D11_TEXTURE2D_DESC d = t->desc;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        d.MiscFlags &= D3D11_RESOURCE_MISC_TEXTURECUBE;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.CPUAccessFlags = 0;
        HRESULT hr = device->CreateTexture2D(&d, nullptr, &t->copy);
        Log("depth copy %ux%u format %d for read-only depth use: 0x%08lx", d.Width, d.Height, d.Format, hr);
        t->dirty = true;
    }
    if (t->copy && !view->twin) device->CreateShaderResourceView(t->copy, &view->srvDesc, &view->twin);
    ReleaseSRWLockExclusive(&g_createLock);
    return view->twin;
}

void Unbind(DepthState* d, unsigned stage, UINT start, UINT count) {
    for (size_t i = 0; i < d->bound.size();) {
        Binding& b = d->bound[i];
        if (b.stage == stage && b.slot >= start && b.slot < start + count) {
            b.texture->Release();
            d->bound.erase(d->bound.begin() + i);
        } else {
            i++;
        }
    }
}

}  // namespace

void DepthOnTextureCreated(ID3D11Texture2D* texture, const D3D11_TEXTURE2D_DESC* desc) {
    if (!(desc->BindFlags & D3D11_BIND_DEPTH_STENCIL) || !(desc->BindFlags & D3D11_BIND_SHADER_RESOURCE)) return;
    DepthTexture* t = new DepthTexture;
    t->id = g_nextId++;
    t->source = texture;
    t->desc = *desc;
    Log("depth texture #%u: %ux%u format %d, %u samples, %u layers, bind 0x%x, misc 0x%x", t->id, desc->Width,
        desc->Height, desc->Format, desc->SampleDesc.Count, desc->ArraySize, desc->BindFlags, desc->MiscFlags);
    AcquireSRWLockExclusive(&g_lock);
    g_textures[texture] = t;
    ReleaseSRWLockExclusive(&g_lock);
    TextureLink* link = new TextureLink(t);  // takes over the creation reference of t
    texture->SetPrivateDataInterface(kLinkGuid, link);
    link->Release();
}

void DepthOnViewCreated(ID3D11Resource* resource, ID3D11DepthStencilView* dsv, bool readOnlySubstitute) {
    DepthTexture* t = FindTexture(resource);
    if (!t) return;
    ViewInfo* v = TrackView(dsv, t);
    v->readOnlySubstitute = readOnlySubstitute;
    if (readOnlySubstitute) {
        t->readOnlyUse = true;
        Log("depth texture #%u gets a writable view in place of a read-only one", t->id);
    }
}

void DepthOnViewCreated(ID3D11Resource* resource, ID3D11ShaderResourceView* srv) {
    DepthTexture* t = FindTexture(resource);
    if (!t) return;
    ViewInfo* v = TrackView(srv, t);
    v->srv = true;
    srv->GetDesc(&v->srvDesc);
}

void DepthOnSetShaderResources(ID3D11DeviceContext* context, unsigned stage, UINT start, UINT count,
                               ID3D11ShaderResourceView* const* views, ID3D11ShaderResourceView** out) {
    for (UINT i = 0; i < count; i++) out[i] = views ? views[i] : nullptr;
    ContextState* state = FindContext(context);
    DepthState* d = State(state);
    if (!d) return;
    if (Tracing() && FirstTime("trace-set")) Log("trace: textures set on context %p (state %p)", (void*)context, (void*)state);
    Unbind(d, stage, start, count);
    for (UINT i = 0; i < count; i++) {
        ViewInfo* v = FindView(out[i]);
        if (!v || !v->srv) continue;
        ID3D11ShaderResourceView* twin = TwinFor(state->device, v);
        if (!twin) continue;
        out[i] = twin;
        Trace("stage %u slot %u reads the copy of #%u%s", stage, start + i, v->texture->id,
              v->texture->dirty ? " (stale, refreshed at next draw)" : "");
        v->texture->AddRef();
        d->bound.push_back({stage, start + i, v->texture});
        if (FirstTime("depth-twin")) Log("shaders read a copy of depth buffers the game binds read-only");
    }
}

void DepthOnSetDepthView(ID3D11DeviceContext* context, ID3D11DepthStencilView* dsv) {
    DepthState* d = State(FindContext(context));
    if (!d) return;
    if (d->writable) d->writable->Release();
    d->writable = nullptr;
    ViewInfo* v = FindView(dsv);
    if (v && !v->srv && !v->readOnlySubstitute) {
        d->writable = v->texture;
        d->writable->AddRef();
        d->writable->dirty = true;
    }
    if (!dsv) Trace("depth unbound");
    else if (!v) Trace("depth bound: untracked view");
    else Trace("depth bound: #%u %s", v->texture->id, v->readOnlySubstitute ? "read-only (substitute)" : "writable");
}

void DepthOnWrite(ID3D11Resource* destination, const char* how) {
    DepthTexture* t = FindTexture(destination);
    if (!t) return;
    t->dirty = true;
    Trace("%s into #%u", how, t->id);
}

void DepthOnClearDepthView(ID3D11DepthStencilView* dsv) {
    ViewInfo* v = FindView(dsv);
    if (!v) return;
    v->texture->dirty = true;
    Trace("clear #%u", v->texture->id);
}

void DepthBeforeDraw(ID3D11DeviceContext* context, ContextState* state) {
    DepthState* d = state && state->depth ? static_cast<DepthState*>(state->depth) : nullptr;
    if (!d) return;
    for (Binding& b : d->bound) {
        DepthTexture* t = b.texture;
        if (t->dirty && t->copy && t->source) {
            context->CopyResource(t->copy, t->source);
            t->dirty = false;
            Trace("refreshed the copy of #%u", t->id);
        }
    }
    if (Tracing()) g_drawsSinceTrace++;
}

void DepthAfterDraw(ContextState* state) {
    DepthState* d = state && state->depth ? static_cast<DepthState*>(state->depth) : nullptr;
    if (d && d->writable) d->writable->dirty = true;
}

void DepthReset(ContextState* state) {
    DepthState* d = state && state->depth ? static_cast<DepthState*>(state->depth) : nullptr;
    if (!d) return;
    for (Binding& b : d->bound) b.texture->Release();
    d->bound.clear();
    if (d->writable) d->writable->Release();
    d->writable = nullptr;
}
