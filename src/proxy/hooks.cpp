// Vtable patching, device/context bookkeeping and placeholder objects.
//
// We patch the vtables of the real D3D11 objects instead of wrapping them: every
// object of a class shares one vtable, so a patch catches calls made through any
// pointer the game holds (GetDevice(), GetImmediateContext(), Steam overlay...),
// and object identity stays untouched. It also works for x64 games running under
// emulation on ARM64 Windows, where the runtime's functions are ARM64EC code.
#include <cstring>
#include <unordered_map>
#include <vector>

#include "proxy.h"

namespace {

constexpr int kMaxSlots = 192;
constexpr int kMaxVtables = 32;

struct VtableEntry {
    void** vtable;
    void* original[kMaxSlots];
};

VtableEntry g_vtables[kMaxVtables];
std::atomic<int> g_vtableCount{0};
SRWLOCK g_hookLock = SRWLOCK_INIT;

VtableEntry* FindVtable(void** vtable) {
    int n = g_vtableCount.load(std::memory_order_acquire);
    for (int i = 0; i < n; i++)
        if (g_vtables[i].vtable == vtable) return &g_vtables[i];
    return nullptr;
}

constexpr int kMaxDevices = 16;
DeviceInfo g_devices[kMaxDevices];
std::atomic<int> g_deviceCount{0};

// Every interface pointer of a context (ID3D11DeviceContext, ...1, ...2, ...) maps to one
// shared state: games may call through any of them, and they need not be the same pointer.
struct ContextAlias {
    void* pointer;
    ContextState* state;
};
constexpr int kMaxAliases = 256;
ContextAlias g_contexts[kMaxAliases];
std::atomic<int> g_contextCount{0};

void AddAlias(void* pointer, ContextState* state) {
    int n = g_contextCount.load();
    for (int i = 0; i < n; i++)
        if (g_contexts[i].pointer == pointer) {
            g_contexts[i].state = state;
            return;
        }
    if (n == kMaxAliases) return;
    g_contexts[n] = {pointer, state};
    g_contextCount.store(n + 1, std::memory_order_release);
}

SRWLOCK g_registryLock = SRWLOCK_INIT;

}  // namespace

void Hook(void* object, int slot, void* function) {
    if (!object || slot < 0 || slot >= kMaxSlots) return;
    void** vtable = *static_cast<void***>(object);
    AcquireSRWLockExclusive(&g_hookLock);
    VtableEntry* entry = FindVtable(vtable);
    if (!entry) {
        int n = g_vtableCount.load();
        if (n == kMaxVtables) {
            ReleaseSRWLockExclusive(&g_hookLock);
            Log("hook: too many vtables");
            return;
        }
        entry = &g_vtables[n];
        entry->vtable = vtable;
        memset(entry->original, 0, sizeof(entry->original));
        g_vtableCount.store(n + 1, std::memory_order_release);
    }
    if (!entry->original[slot] && vtable[slot] != function) {
        entry->original[slot] = vtable[slot];
        DWORD old;
        VirtualProtect(&vtable[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
        vtable[slot] = function;
        VirtualProtect(&vtable[slot], sizeof(void*), old, &old);
        FlushInstructionCache(GetCurrentProcess(), &vtable[slot], sizeof(void*));
    }
    ReleaseSRWLockExclusive(&g_hookLock);
}

bool IsHookedTable(void** table) { return FindVtable(table) != nullptr; }

void* Original(void* object, int slot) {
    void** table = *static_cast<void***>(object);
    VtableEntry* entry = FindVtable(table);
    if (entry && entry->original[slot]) return entry->original[slot];
    return table[slot];  // not patched (contexts are wrapped instead): the live entry
}

void RegisterDevice(ID3D11Device* device, D3D_FEATURE_LEVEL real, D3D_FEATURE_LEVEL reported, bool downlevel) {
    AcquireSRWLockExclusive(&g_registryLock);
    int n = g_deviceCount.load();
    DeviceInfo info{device, real, reported, downlevel};
    int slot = n;
    for (int i = 0; i < n; i++)
        if (g_devices[i].device == device) slot = i;
    if (slot == kMaxDevices) slot = kMaxDevices - 1;  // reuse the last entry
    g_devices[slot] = info;
    if (slot == n) g_deviceCount.store(n + 1, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_registryLock);
}

const DeviceInfo* FindDevice(ID3D11Device* device) {
    int n = g_deviceCount.load(std::memory_order_acquire);
    for (int i = 0; i < n; i++)
        if (g_devices[i].device == device) return &g_devices[i];
    return nullptr;
}

ContextState* FindContext(ID3D11DeviceContext* context) {
    int n = g_contextCount.load(std::memory_order_acquire);
    for (int i = 0; i < n; i++)
        if (g_contexts[i].pointer == context) return g_contexts[i].state;
    return nullptr;
}

// Registers a real context (and its other interface pointers) for the wrapper's bookkeeping.
void HookContext(ID3D11DeviceContext* context, ID3D11Device* device, bool downlevel) {
    const IID ids[] = {__uuidof(ID3D11DeviceContext), __uuidof(ID3D11DeviceContext1),
                       __uuidof(ID3D11DeviceContext2), __uuidof(ID3D11DeviceContext3),
                       __uuidof(ID3D11DeviceContext4)};
    void* pointers[5] = {};
    for (int i = 0; i < 5; i++) {
        IUnknown* p = nullptr;
        if (SUCCEEDED(context->QueryInterface(ids[i], (void**)&p))) {
            pointers[i] = p;
            p->Release();
        }
    }
    AcquireSRWLockExclusive(&g_registryLock);
    if (!FindContext(context)) {
        ContextState* state = new ContextState{};
        state->context = context;
        state->device = device;
        state->downlevel = downlevel;
        state->immediate = context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE;
        AddAlias(context, state);
        for (void* p : pointers)
            if (p) AddAlias(p, state);
    }
    ReleaseSRWLockExclusive(&g_registryLock);
}

void HookDevice(ID3D11Device* device) {
    InstallDeviceHooks(device, 0);
    const IID ids[] = {__uuidof(ID3D11Device1), __uuidof(ID3D11Device2), __uuidof(ID3D11Device3)};
    for (int level = 1; level <= 3; level++) {
        IUnknown* other = nullptr;
        if (FAILED(device->QueryInterface(ids[level - 1], (void**)&other))) break;
        InstallDeviceHooks(other, level);
        other->Release();
    }
}

void HookFactoryOf(ID3D11Device* device) {
    IDXGIDevice* dxgiDevice = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory* factory = nullptr;
    if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice)) &&
        SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) &&
        SUCCEEDED(adapter->GetParent(__uuidof(IDXGIFactory), (void**)&factory))) {
        InstallFactoryHooks(factory, false);
        IDXGIFactory2* factory2 = nullptr;
        if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory2), (void**)&factory2))) {
            InstallFactoryHooks(factory2, true);
            factory2->Release();
        }
    } else {
        Log("could not reach the DXGI factory; screenshots and frame counting are off");
    }
    if (factory) factory->Release();
    if (adapter) adapter->Release();
    if (dxgiDevice) dxgiDevice->Release();
}

// ---- placeholder objects ----

namespace {

std::atomic<void*> g_dummyVtables[8];

void RegisterDummyVtable(void* object) {
    void* vtable = *static_cast<void**>(object);
    for (auto& slot : g_dummyVtables) {
        void* current = slot.load();
        if (current == vtable) return;
        if (!current && slot.compare_exchange_strong(current, vtable)) return;
    }
}

struct PrivateData {
    GUID guid;
    std::vector<uint8_t> data;
    IUnknown* object;
};

template <class I>
class DummyChild : public I {
public:
    explicit DummyChild(ID3D11Device* device) : device_(device) { device_->AddRef(); }
    virtual ~DummyChild() {
        ForgetDummy(this);
        for (PrivateData& p : data_)
            if (p.object) p.object->Release();
        device_->Release();
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11DeviceChild) || riid == __uuidof(I)) {
            AddRef();
            *out = this;
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG refs = InterlockedDecrement(&refs_);
        if (!refs) delete this;
        return refs;
    }
    void STDMETHODCALLTYPE GetDevice(ID3D11Device** out) override {
        device_->AddRef();
        *out = device_;
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* size, void* data) override {
        if (!size) return E_INVALIDARG;
        for (PrivateData& p : data_) {
            if (p.guid != guid) continue;
            UINT needed = p.object ? sizeof(IUnknown*) : UINT(p.data.size());
            if (!data) {
                *size = needed;
                return S_OK;
            }
            if (*size < needed) return DXGI_ERROR_MORE_DATA;
            *size = needed;
            if (p.object) {
                p.object->AddRef();
                memcpy(data, &p.object, sizeof(IUnknown*));
            } else {
                memcpy(data, p.data.data(), needed);
            }
            return S_OK;
        }
        *size = 0;
        return DXGI_ERROR_NOT_FOUND;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT size, const void* data) override {
        Remove(guid);
        if (data) data_.push_back({guid, std::vector<uint8_t>((const uint8_t*)data, (const uint8_t*)data + size), nullptr});
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* object) override {
        Remove(guid);
        if (object) {
            const_cast<IUnknown*>(object)->AddRef();
            data_.push_back({guid, {}, const_cast<IUnknown*>(object)});
        }
        return S_OK;
    }

private:
    void Remove(REFGUID guid) {
        for (size_t i = 0; i < data_.size(); i++) {
            if (data_[i].guid != guid) continue;
            if (data_[i].object) data_[i].object->Release();
            data_.erase(data_.begin() + i);
            return;
        }
    }

    LONG refs_ = 1;
    ID3D11Device* device_;
    std::vector<PrivateData> data_;
};

template <class I>
class DummyShader final : public DummyChild<I> {
public:
    using DummyChild<I>::DummyChild;
};

class DummyUAV final : public DummyChild<ID3D11UnorderedAccessView> {
public:
    DummyUAV(ID3D11Device* device, ID3D11Resource* resource, const D3D11_UNORDERED_ACCESS_VIEW_DESC* desc)
        : DummyChild(device), resource_(resource) {
        if (resource_) resource_->AddRef();
        if (desc) desc_ = *desc;
    }
    ~DummyUAV() override {
        if (resource_) resource_->Release();
    }
    void STDMETHODCALLTYPE GetResource(ID3D11Resource** out) override {
        if (resource_) resource_->AddRef();
        *out = resource_;
    }
    void STDMETHODCALLTYPE GetDesc(D3D11_UNORDERED_ACCESS_VIEW_DESC* desc) override { *desc = desc_; }

private:
    ID3D11Resource* resource_;
    D3D11_UNORDERED_ACCESS_VIEW_DESC desc_ = {};
};

}  // namespace

bool IsDummy(const void* object) {
    if (!object) return false;
    void* vtable = *static_cast<void* const*>(object);
    for (auto& slot : g_dummyVtables) {
        void* current = slot.load(std::memory_order_relaxed);
        if (!current) return false;
        if (current == vtable) return true;
    }
    return false;
}

SRWLOCK g_tagLock = SRWLOCK_INIT;
std::unordered_map<const void*, unsigned> g_tags;

void ForgetDummy(const void* object) {
    AcquireSRWLockExclusive(&g_tagLock);
    g_tags.erase(object);
    ReleaseSRWLockExclusive(&g_tagLock);
    ForgetComputeEmulation(object);
}

unsigned DummyTag(const void* object) {
    AcquireSRWLockShared(&g_tagLock);
    auto it = g_tags.find(object);
    unsigned tag = it == g_tags.end() ? 0 : it->second;
    ReleaseSRWLockShared(&g_tagLock);
    return tag;
}

ID3D11DeviceChild* MakeDummyShader(ID3D11Device* device, unsigned stage, unsigned tag) {
    ID3D11DeviceChild* shader;
    switch (stage) {
        case kVS: shader = new DummyShader<ID3D11VertexShader>(device); break;
        case kPS: shader = new DummyShader<ID3D11PixelShader>(device); break;
        case kGS: shader = new DummyShader<ID3D11GeometryShader>(device); break;
        case kHS: shader = new DummyShader<ID3D11HullShader>(device); break;
        case kDS: shader = new DummyShader<ID3D11DomainShader>(device); break;
        default: shader = new DummyShader<ID3D11ComputeShader>(device); break;
    }
    RegisterDummyVtable(shader);
    AcquireSRWLockExclusive(&g_tagLock);
    g_tags[shader] = tag;  // shaders live as long as the game keeps them; a stale entry is harmless
    ReleaseSRWLockExclusive(&g_tagLock);
    return shader;
}

ID3D11UnorderedAccessView* MakeDummyUAV(ID3D11Device* device, ID3D11Resource* resource,
                                         const D3D11_UNORDERED_ACCESS_VIEW_DESC* desc) {
    ID3D11UnorderedAccessView* view = new DummyUAV(device, resource, desc);
    RegisterDummyVtable(view);
    return view;
}
