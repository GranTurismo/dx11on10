// ID3D11Device hooks: shader conversion, fake feature level, and fallbacks for
// resources a feature level 10 device refuses to create.
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>

#include "../dxbc/convert.h"
#include "proxy.h"

namespace {

struct ShaderEntry {
    dxbc::ConvertResult::Status status;
    std::vector<uint8_t> blob;
    unsigned id;  // number used in the log and dump file names
};

std::mutex g_cacheLock;
std::map<std::string, std::shared_ptr<ShaderEntry>> g_cache;
unsigned g_shaderCounter = 0;

const char* StageName(unsigned stage) {
    switch (stage) {
        case kVS: return "VS";
        case kPS: return "PS";
        case kGS: return "GS";
        case kHS: return "HS";
        case kDS: return "DS";
        default: return "CS";
    }
}

void DumpShader(unsigned id, unsigned stage, const char* suffix, const void* data, size_t size) {
    std::wstring dir = OutputPath(L"dx11on10_shaders");
    CreateDirectoryW(dir.c_str(), nullptr);
    wchar_t name[96];
    swprintf(name, 96, L"\\%04u_%hs%hs.dxbc", id, StageName(stage), suffix);
    FILE* f = _wfopen((dir + name).c_str(), L"wb");
    if (!f) return;
    fwrite(data, 1, size, f);
    fclose(f);
}

// The MD5 the DXBC header carries, as hex: identifies a shader exactly.
std::string HashOf(const void* code) {
    static const char digits[] = "0123456789abcdef";
    std::string s;
    const uint8_t* p = static_cast<const uint8_t*>(code) + 4;
    for (int i = 0; i < 16; i++) {
        s += digits[p[i] >> 4];
        s += digits[p[i] & 15];
    }
    return s;
}

struct Prepared {
    const void* code;
    SIZE_T size;
    bool converted = false;
    bool unsupported = false;
    unsigned id = 0;
    std::shared_ptr<ShaderEntry> keep;
};

// Returns the bytecode to hand to the real device for this shader.
Prepared PrepareShader(ID3D11Device* device, const void* code, SIZE_T size, unsigned stage) {
    Prepared p{code, size};
    const DeviceInfo* info = FindDevice(device);
    if (!info || !info->downlevel || !code || size < 32) return p;

    // The DXBC header carries an MD5 of the contents: a natural cache key.
    std::string key((const char*)code + 4, 16);
    key.append((const char*)&size, sizeof(size));
    key += char('0' + (info->real >= D3D_FEATURE_LEVEL_10_1));
    std::shared_ptr<ShaderEntry> entry;
    unsigned id;
    {
        std::lock_guard<std::mutex> lock(g_cacheLock);
        auto it = g_cache.find(key);
        if (it != g_cache.end()) entry = it->second;
        id = ++g_shaderCounter;
    }
    if (!entry) {
        dxbc::ConvertResult r = dxbc::ToSM4(code, size, info->real >= D3D_FEATURE_LEVEL_10_1 ? 1 : 0);
        entry = std::make_shared<ShaderEntry>(ShaderEntry{r.status, std::move(r.blob), id});
        switch (r.status) {
            case dxbc::ConvertResult::Converted:
                Log("shader #%u %s %d.%d (%u bytes): converted to 4.%d%s%s", id, StageName(stage), r.major, r.minor,
                    unsigned(size), info->real >= D3D_FEATURE_LEVEL_10_1, r.message.empty() ? "" : " - ",
                    r.message.c_str());
                g_stats.shadersConverted++;
                break;
            case dxbc::ConvertResult::AlreadySM4:
                g_stats.shadersPassed++;
                break;
            default:
                Log("shader #%u %s %d.%d (%u bytes, hash %s): NOT CONVERTIBLE - %s", id, StageName(stage), r.major,
                    r.minor, unsigned(size), HashOf(code).c_str(), r.message.c_str());
                g_stats.shadersDummy++;
                break;
        }
        if (g_config.dumpShaders == 1 || (g_config.dumpShaders == 2 && entry->blob.empty())) {
            DumpShader(id, stage, "", code, size);
            if (!entry->blob.empty()) DumpShader(id, stage, "_sm4", entry->blob.data(), entry->blob.size());
        }
        std::lock_guard<std::mutex> lock(g_cacheLock);
        g_cache.emplace(key, entry);
    }
    p.keep = entry;
    p.id = entry->id;
    if (entry->status == dxbc::ConvertResult::Converted) {
        p.code = entry->blob.data();
        p.size = entry->blob.size();
        p.converted = true;
    } else if (entry->status != dxbc::ConvertResult::AlreadySM4) {
        p.unsupported = true;
    }
    return p;
}

// Creates a shader through the real device, or a placeholder when it can't be converted.
template <class I, class F>
HRESULT CreateShader(F create, ID3D11Device* device, const void* code, SIZE_T size, ID3D11ClassLinkage* linkage,
                     I** out, unsigned stage) {
    if (stage == kCS && g_config.keepCompute) return create(device, code, size, linkage, out);
    Prepared p = PrepareShader(device, code, size, stage);
    for (unsigned keep : g_config.keepComputeIds)
        if (stage == kCS && p.id == keep) {
            Log("keeping compute shader #%u (KeepComputeIds)", keep);
            return create(device, code, size, linkage, out);
        }
    if (!p.unsupported) {
        HRESULT hr = create(device, p.code, p.size, p.converted ? nullptr : linkage, out);
        if (SUCCEEDED(hr) || !p.converted) return hr;
        Log("%s: the runtime rejected a converted shader (0x%08lx); using a placeholder", StageName(stage), hr);
    }
    if (!out) return S_FALSE;
    *out = static_cast<I*>(MakeDummyShader(device, stage, p.id));
    if (stage == kCS && size >= 32) {
        const DeviceInfo* info = FindDevice(device);
        PrepareComputeEmulation(device, *out, code, size, p.id, info && info->real >= D3D_FEATURE_LEVEL_10_1 ? 1 : 0);
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Hk_CreateVertexShader(ID3D11Device* self, const void* code, SIZE_T size,
                                                ID3D11ClassLinkage* linkage, ID3D11VertexShader** out) {
    auto orig = Orig<decltype(&Hk_CreateVertexShader)>(self, g_slots.devCreateVertexShader);
    return CreateShader(orig, self, code, size, linkage, out, kVS);
}

HRESULT STDMETHODCALLTYPE Hk_CreatePixelShader(ID3D11Device* self, const void* code, SIZE_T size,
                                               ID3D11ClassLinkage* linkage, ID3D11PixelShader** out) {
    auto orig = Orig<decltype(&Hk_CreatePixelShader)>(self, g_slots.devCreatePixelShader);
    return CreateShader(orig, self, code, size, linkage, out, kPS);
}

HRESULT STDMETHODCALLTYPE Hk_CreateGeometryShader(ID3D11Device* self, const void* code, SIZE_T size,
                                                  ID3D11ClassLinkage* linkage, ID3D11GeometryShader** out) {
    auto orig = Orig<decltype(&Hk_CreateGeometryShader)>(self, g_slots.devCreateGeometryShader);
    return CreateShader(orig, self, code, size, linkage, out, kGS);
}

HRESULT STDMETHODCALLTYPE Hk_CreateHullShader(ID3D11Device* self, const void* code, SIZE_T size,
                                              ID3D11ClassLinkage* linkage, ID3D11HullShader** out) {
    auto orig = Orig<decltype(&Hk_CreateHullShader)>(self, g_slots.devCreateHullShader);
    return CreateShader(orig, self, code, size, linkage, out, kHS);
}

HRESULT STDMETHODCALLTYPE Hk_CreateDomainShader(ID3D11Device* self, const void* code, SIZE_T size,
                                                ID3D11ClassLinkage* linkage, ID3D11DomainShader** out) {
    auto orig = Orig<decltype(&Hk_CreateDomainShader)>(self, g_slots.devCreateDomainShader);
    return CreateShader(orig, self, code, size, linkage, out, kDS);
}

HRESULT STDMETHODCALLTYPE Hk_CreateComputeShader(ID3D11Device* self, const void* code, SIZE_T size,
                                                 ID3D11ClassLinkage* linkage, ID3D11ComputeShader** out) {
    auto orig = Orig<decltype(&Hk_CreateComputeShader)>(self, g_slots.devCreateComputeShader);
    return CreateShader(orig, self, code, size, linkage, out, kCS);
}

HRESULT STDMETHODCALLTYPE Hk_CreateGeometryShaderWithStreamOutput(
    ID3D11Device* self, const void* code, SIZE_T size, const D3D11_SO_DECLARATION_ENTRY* entries, UINT numEntries,
    const UINT* strides, UINT numStrides, UINT rasterizedStream, ID3D11ClassLinkage* linkage,
    ID3D11GeometryShader** out) {
    auto orig = Orig<decltype(&Hk_CreateGeometryShaderWithStreamOutput)>(
        self, g_slots.devCreateGeometryShaderWithStreamOutput);
    Prepared p = PrepareShader(self, code, size, kGS);
    if (!p.unsupported) {
        HRESULT hr = orig(self, p.code, p.size, entries, numEntries, strides, numStrides, rasterizedStream,
                          p.converted ? nullptr : linkage, out);
        if (SUCCEEDED(hr) || !p.converted) {
            const DeviceInfo* info = FindDevice(self);
            if (FAILED(hr) && info && info->downlevel) {
                // Feature level 10 has one stream and always rasterizes it.
                std::vector<D3D11_SO_DECLARATION_ENTRY> stream0;
                for (UINT i = 0; i < numEntries; i++)
                    if (entries[i].Stream == 0) stream0.push_back(entries[i]);
                hr = orig(self, p.code, p.size, stream0.data(), UINT(stream0.size()), strides, numStrides, 0,
                          nullptr, out);
                Log("stream-output GS retried with stream 0 only: 0x%08lx", hr);
            }
            return hr;
        }
    }
    if (!out) return S_FALSE;
    *out = static_cast<ID3D11GeometryShader*>(MakeDummyShader(self, kGS, p.id));
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Hk_CreateInputLayout(ID3D11Device* self, const D3D11_INPUT_ELEMENT_DESC* elements,
                                               UINT count, const void* code, SIZE_T size, ID3D11InputLayout** out) {
    auto orig = Orig<decltype(&Hk_CreateInputLayout)>(self, g_slots.devCreateInputLayout);
    HRESULT hr = orig(self, elements, count, code, size, out);
    if (SUCCEEDED(hr)) return hr;
    Prepared p = PrepareShader(self, code, size, kVS);
    if (p.converted) {
        hr = orig(self, elements, count, p.code, p.size, out);
        if (FirstTime("layout-converted")) Log("input layouts need the converted VS bytecode (0x%08lx)", hr);
    }
    if (FAILED(hr)) Log("CreateInputLayout failed: 0x%08lx", hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hk_CreateBuffer(ID3D11Device* self, const D3D11_BUFFER_DESC* desc,
                                          const D3D11_SUBRESOURCE_DATA* data, ID3D11Buffer** out) {
    auto orig = Orig<decltype(&Hk_CreateBuffer)>(self, g_slots.devCreateBuffer);
    HRESULT hr = orig(self, desc, data, out);
    const DeviceInfo* info = FindDevice(self);
    if (SUCCEEDED(hr) || !desc || !info || !info->downlevel) return hr;
    D3D11_BUFFER_DESC d = *desc;
    bool typed = !(desc->MiscFlags & (D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS | D3D11_RESOURCE_MISC_BUFFER_STRUCTURED));
    d.BindFlags &= ~D3D11_BIND_UNORDERED_ACCESS;
    if (typed && (desc->BindFlags & D3D11_BIND_UNORDERED_ACCESS)) d.BindFlags |= D3D11_BIND_RENDER_TARGET;
    d.MiscFlags &= ~(D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS | D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS |
                     D3D11_RESOURCE_MISC_BUFFER_STRUCTURED);
    if (!(d.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED)) d.StructureByteStride = 0;
    HRESULT retry = orig(self, &d, data, out);
    if (FAILED(retry) && (d.BindFlags & D3D11_BIND_RENDER_TARGET) && !(desc->BindFlags & D3D11_BIND_RENDER_TARGET)) {
        d.BindFlags &= ~D3D11_BIND_RENDER_TARGET;
        retry = orig(self, &d, data, out);
    }
    char key[96];
    snprintf(key, sizeof(key), "buffer %x %x", desc->BindFlags, desc->MiscFlags);
    if (FirstTime(key))
        Log("CreateBuffer(bind 0x%x, misc 0x%x, %u bytes) failed (0x%08lx); without DX11-only flags: 0x%08lx",
            desc->BindFlags, desc->MiscFlags, desc->ByteWidth, hr, retry);
    return retry;
}

template <class Desc>
void LogTextureFailure(const char* kind, const Desc* desc, HRESULT hr, HRESULT retry) {
    char key[96];
    snprintf(key, sizeof(key), "%s %d %x %x", kind, desc->Format, desc->BindFlags, desc->MiscFlags);
    if (FirstTime(key))
        Log("%s(format %d, bind 0x%x, misc 0x%x) failed (0x%08lx); without UAV binding: 0x%08lx", kind, desc->Format,
            desc->BindFlags, desc->MiscFlags, hr, retry);
}

HRESULT STDMETHODCALLTYPE Hk_CreateTexture1D(ID3D11Device* self, const D3D11_TEXTURE1D_DESC* desc,
                                             const D3D11_SUBRESOURCE_DATA* data, ID3D11Texture1D** out) {
    auto orig = Orig<decltype(&Hk_CreateTexture1D)>(self, g_slots.devCreateTexture1D);
    HRESULT hr = orig(self, desc, data, out);
    const DeviceInfo* info = FindDevice(self);
    if (SUCCEEDED(hr) || !desc || !info || !info->downlevel) return hr;
    D3D11_TEXTURE1D_DESC d = *desc;
    d.BindFlags = (d.BindFlags & ~D3D11_BIND_UNORDERED_ACCESS) | D3D11_BIND_RENDER_TARGET;
    HRESULT retry = orig(self, &d, data, out);
    if (FAILED(retry)) {
        d.BindFlags = (desc->BindFlags & ~D3D11_BIND_UNORDERED_ACCESS);
        retry = orig(self, &d, data, out);
    }
    LogTextureFailure("CreateTexture1D", desc, hr, retry);
    return retry;
}

HRESULT STDMETHODCALLTYPE Hk_CreateTexture2D(ID3D11Device* self, const D3D11_TEXTURE2D_DESC* desc,
                                             const D3D11_SUBRESOURCE_DATA* data, ID3D11Texture2D** out) {
    auto orig = Orig<decltype(&Hk_CreateTexture2D)>(self, g_slots.devCreateTexture2D);
    HRESULT hr = orig(self, desc, data, out);
    const DeviceInfo* info = FindDevice(self);
    if (!desc || !info || !info->downlevel) return hr;
    if (FAILED(hr)) {
        // Without UAVs, compute emulations draw into the texture instead: ask for render target use.
        D3D11_TEXTURE2D_DESC d = *desc;
        d.BindFlags = (d.BindFlags & ~D3D11_BIND_UNORDERED_ACCESS) | D3D11_BIND_RENDER_TARGET;
        HRESULT retry = orig(self, &d, data, out);
        if (FAILED(retry)) {
            d.BindFlags &= ~D3D11_BIND_RENDER_TARGET;
            d.BindFlags |= desc->BindFlags & D3D11_BIND_RENDER_TARGET;
            retry = orig(self, &d, data, out);
        }
        LogTextureFailure("CreateTexture2D", desc, hr, retry);
        hr = retry;
    }
    if (SUCCEEDED(hr) && out && *out) DepthOnTextureCreated(*out, desc);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hk_CreateTexture3D(ID3D11Device* self, const D3D11_TEXTURE3D_DESC* desc,
                                             const D3D11_SUBRESOURCE_DATA* data, ID3D11Texture3D** out) {
    auto orig = Orig<decltype(&Hk_CreateTexture3D)>(self, g_slots.devCreateTexture3D);
    HRESULT hr = orig(self, desc, data, out);
    const DeviceInfo* info = FindDevice(self);
    if (SUCCEEDED(hr) || !desc || !info || !info->downlevel) return hr;
    D3D11_TEXTURE3D_DESC d = *desc;
    d.BindFlags = (d.BindFlags & ~D3D11_BIND_UNORDERED_ACCESS) | D3D11_BIND_RENDER_TARGET;
    HRESULT retry = orig(self, &d, data, out);
    if (FAILED(retry)) {
        d.BindFlags = (desc->BindFlags & ~D3D11_BIND_UNORDERED_ACCESS);
        retry = orig(self, &d, data, out);
    }
    LogTextureFailure("CreateTexture3D", desc, hr, retry);
    return retry;
}

HRESULT STDMETHODCALLTYPE Hk_CreateUnorderedAccessView(ID3D11Device* self, ID3D11Resource* resource,
                                                       const D3D11_UNORDERED_ACCESS_VIEW_DESC* desc,
                                                       ID3D11UnorderedAccessView** out) {
    auto orig = Orig<decltype(&Hk_CreateUnorderedAccessView)>(self, g_slots.devCreateUnorderedAccessView);
    HRESULT hr = orig(self, resource, desc, out);
    const DeviceInfo* info = FindDevice(self);
    if (SUCCEEDED(hr) || !info || !info->downlevel) return hr;
    if (FirstTime("uav")) Log("UAVs are not available (0x%08lx); handing out placeholders", hr);
    if (!out) return S_FALSE;
    *out = MakeDummyUAV(self, resource, desc);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Hk_CreateDepthStencilView(ID3D11Device* self, ID3D11Resource* resource,
                                                    const D3D11_DEPTH_STENCIL_VIEW_DESC* desc,
                                                    ID3D11DepthStencilView** out) {
    auto orig = Orig<decltype(&Hk_CreateDepthStencilView)>(self, g_slots.devCreateDepthStencilView);
    HRESULT hr = orig(self, resource, desc, out);
    const DeviceInfo* info = FindDevice(self);
    if (!info || !info->downlevel) return hr;
    bool substitute = false;
    if (FAILED(hr) && desc && desc->Flags) {
        // Read-only depth views are a DX11 feature: hand out a normal view, and let
        // depth.cpp give shaders a copy of the buffer while this view is bound.
        D3D11_DEPTH_STENCIL_VIEW_DESC d = *desc;
        d.Flags = 0;
        HRESULT retry = orig(self, resource, &d, out);
        if (FirstTime("readonly-dsv"))
            Log("read-only depth view failed (0x%08lx); writable view: 0x%08lx", hr, retry);
        hr = retry;
        substitute = true;
    }
    if (SUCCEEDED(hr) && out && *out) DepthOnViewCreated(resource, *out, substitute);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hk_CreateShaderResourceView(ID3D11Device* self, ID3D11Resource* resource,
                                                      const D3D11_SHADER_RESOURCE_VIEW_DESC* desc,
                                                      ID3D11ShaderResourceView** out) {
    auto orig = Orig<decltype(&Hk_CreateShaderResourceView)>(self, g_slots.devCreateShaderResourceView);
    HRESULT hr = orig(self, resource, desc, out);
    const DeviceInfo* info = FindDevice(self);
    if (SUCCEEDED(hr) && out && *out && info && info->downlevel) DepthOnViewCreated(resource, *out);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hk_CreateBlendState(ID3D11Device* self, const D3D11_BLEND_DESC* desc,
                                              ID3D11BlendState** out) {
    auto orig = Orig<decltype(&Hk_CreateBlendState)>(self, g_slots.devCreateBlendState);
    HRESULT hr = orig(self, desc, out);
    const DeviceInfo* info = FindDevice(self);
    if (SUCCEEDED(hr) || !desc || !desc->IndependentBlendEnable || !info || !info->downlevel) return hr;
    // Feature level 10_0 applies render target 0's blend settings to all targets.
    D3D11_BLEND_DESC d = *desc;
    d.IndependentBlendEnable = FALSE;
    HRESULT retry = orig(self, &d, out);
    if (FirstTime("independent-blend")) Log("independent blending failed (0x%08lx); shared blend: 0x%08lx", hr, retry);
    return retry;
}

HRESULT STDMETHODCALLTYPE Hk_CreateBlendState1(ID3D11Device1* self, const D3D11_BLEND_DESC1* desc,
                                               ID3D11BlendState1** out) {
    auto orig = Orig<decltype(&Hk_CreateBlendState1)>(self, g_slots.dev1CreateBlendState1);
    HRESULT hr = orig(self, desc, out);
    const DeviceInfo* info = FindDevice(self);
    if (SUCCEEDED(hr) || !desc || !desc->IndependentBlendEnable || !info || !info->downlevel) return hr;
    D3D11_BLEND_DESC1 d = *desc;
    d.IndependentBlendEnable = FALSE;
    HRESULT retry = orig(self, &d, out);
    if (FirstTime("independent-blend1")) Log("independent blending failed (0x%08lx); shared blend: 0x%08lx", hr, retry);
    return retry;
}

// Registers a context and swaps in our wrapper when the device is below feature level 11.
template <class C>
void HookNewContext(ID3D11Device* device, C** out) {
    const DeviceInfo* info = FindDevice(device);
    if (!info || !info->downlevel) return;
    HookContext(*out, device, true);
    *out = static_cast<C*>(static_cast<ID3D11DeviceContext*>(WrapContext(*out)));
}

void STDMETHODCALLTYPE Hk_GetImmediateContext(ID3D11Device* self, ID3D11DeviceContext** out) {
    Orig<decltype(&Hk_GetImmediateContext)>(self, g_slots.devGetImmediateContext)(self, out);
    if (out && *out) HookNewContext(self, out);
}

void STDMETHODCALLTYPE Hk_GetImmediateContext1(ID3D11Device1* self, ID3D11DeviceContext1** out) {
    Orig<decltype(&Hk_GetImmediateContext1)>(self, g_slots.dev1GetImmediateContext1)(self, out);
    if (out && *out) HookNewContext(self, out);
}

void STDMETHODCALLTYPE Hk_GetImmediateContext2(ID3D11Device2* self, ID3D11DeviceContext2** out) {
    Orig<decltype(&Hk_GetImmediateContext2)>(self, g_slots.dev2GetImmediateContext2)(self, out);
    if (out && *out) HookNewContext(self, out);
}

void STDMETHODCALLTYPE Hk_GetImmediateContext3(ID3D11Device3* self, ID3D11DeviceContext3** out) {
    Orig<decltype(&Hk_GetImmediateContext3)>(self, g_slots.dev3GetImmediateContext3)(self, out);
    if (out && *out) HookNewContext(self, out);
}

HRESULT STDMETHODCALLTYPE Hk_CreateDeferredContext(ID3D11Device* self, UINT flags, ID3D11DeviceContext** out) {
    auto orig = Orig<decltype(&Hk_CreateDeferredContext)>(self, g_slots.devCreateDeferredContext);
    HRESULT hr = orig(self, flags, out);
    if (SUCCEEDED(hr) && out && *out) HookNewContext(self, out);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hk_CreateDeferredContext1(ID3D11Device1* self, UINT flags, ID3D11DeviceContext1** out) {
    auto orig = Orig<decltype(&Hk_CreateDeferredContext1)>(self, g_slots.dev1CreateDeferredContext1);
    HRESULT hr = orig(self, flags, out);
    if (SUCCEEDED(hr) && out && *out) HookNewContext(self, out);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hk_CreateDeferredContext2(ID3D11Device2* self, UINT flags, ID3D11DeviceContext2** out) {
    auto orig = Orig<decltype(&Hk_CreateDeferredContext2)>(self, g_slots.dev2CreateDeferredContext2);
    HRESULT hr = orig(self, flags, out);
    if (SUCCEEDED(hr) && out && *out) HookNewContext(self, out);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hk_CreateDeferredContext3(ID3D11Device3* self, UINT flags, ID3D11DeviceContext3** out) {
    auto orig = Orig<decltype(&Hk_CreateDeferredContext3)>(self, g_slots.dev3CreateDeferredContext3);
    HRESULT hr = orig(self, flags, out);
    if (SUCCEEDED(hr) && out && *out) HookNewContext(self, out);
    return hr;
}

D3D_FEATURE_LEVEL STDMETHODCALLTYPE Hk_GetFeatureLevel(ID3D11Device* self) {
    const DeviceInfo* info = FindDevice(self);
    if (info) return info->reported;
    return Orig<decltype(&Hk_GetFeatureLevel)>(self, g_slots.devGetFeatureLevel)(self);
}

HRESULT STDMETHODCALLTYPE Hk_CheckFeatureSupport(ID3D11Device* self, D3D11_FEATURE feature, void* data, UINT size) {
    auto orig = Orig<decltype(&Hk_CheckFeatureSupport)>(self, g_slots.devCheckFeatureSupport);
    HRESULT hr = orig(self, feature, data, size);
    const DeviceInfo* info = FindDevice(self);
    if (SUCCEEDED(hr) && info && info->reported >= D3D_FEATURE_LEVEL_11_0 && info->downlevel &&
        feature == D3D11_FEATURE_D3D10_X_HARDWARE_OPTIONS &&
        size >= sizeof(D3D11_FEATURE_DATA_D3D10_X_HARDWARE_OPTIONS)) {
        // Implied by feature level 11_0.
        static_cast<D3D11_FEATURE_DATA_D3D10_X_HARDWARE_OPTIONS*>(data)
            ->ComputeShaders_Plus_RawAndStructuredBuffers_Via_Shader_4_x = TRUE;
    }
    return hr;
}

}  // namespace

void InstallDeviceHooks(void* device, int interfaceLevel) {
    const Slots& s = g_slots;
    Hook(device, s.devCreateBuffer, (void*)&Hk_CreateBuffer);
    Hook(device, s.devCreateTexture1D, (void*)&Hk_CreateTexture1D);
    Hook(device, s.devCreateTexture2D, (void*)&Hk_CreateTexture2D);
    Hook(device, s.devCreateTexture3D, (void*)&Hk_CreateTexture3D);
    Hook(device, s.devCreateUnorderedAccessView, (void*)&Hk_CreateUnorderedAccessView);
    Hook(device, s.devCreateDepthStencilView, (void*)&Hk_CreateDepthStencilView);
    Hook(device, s.devCreateShaderResourceView, (void*)&Hk_CreateShaderResourceView);
    Hook(device, s.devCreateInputLayout, (void*)&Hk_CreateInputLayout);
    Hook(device, s.devCreateVertexShader, (void*)&Hk_CreateVertexShader);
    Hook(device, s.devCreateGeometryShader, (void*)&Hk_CreateGeometryShader);
    Hook(device, s.devCreateGeometryShaderWithStreamOutput, (void*)&Hk_CreateGeometryShaderWithStreamOutput);
    Hook(device, s.devCreatePixelShader, (void*)&Hk_CreatePixelShader);
    Hook(device, s.devCreateHullShader, (void*)&Hk_CreateHullShader);
    Hook(device, s.devCreateDomainShader, (void*)&Hk_CreateDomainShader);
    Hook(device, s.devCreateComputeShader, (void*)&Hk_CreateComputeShader);
    Hook(device, s.devCreateBlendState, (void*)&Hk_CreateBlendState);
    Hook(device, s.devCreateDeferredContext, (void*)&Hk_CreateDeferredContext);
    Hook(device, s.devCheckFeatureSupport, (void*)&Hk_CheckFeatureSupport);
    Hook(device, s.devGetFeatureLevel, (void*)&Hk_GetFeatureLevel);
    Hook(device, s.devGetImmediateContext, (void*)&Hk_GetImmediateContext);
    if (interfaceLevel >= 1) {
        Hook(device, s.dev1CreateDeferredContext1, (void*)&Hk_CreateDeferredContext1);
        Hook(device, s.dev1CreateBlendState1, (void*)&Hk_CreateBlendState1);
        Hook(device, s.dev1GetImmediateContext1, (void*)&Hk_GetImmediateContext1);
    }
    if (interfaceLevel >= 2) {
        Hook(device, s.dev2CreateDeferredContext2, (void*)&Hk_CreateDeferredContext2);
        Hook(device, s.dev2GetImmediateContext2, (void*)&Hk_GetImmediateContext2);
    }
    if (interfaceLevel >= 3) {
        Hook(device, s.dev3CreateDeferredContext3, (void*)&Hk_CreateDeferredContext3);
        Hook(device, s.dev3GetImmediateContext3, (void*)&Hk_GetImmediateContext3);
    }
}
