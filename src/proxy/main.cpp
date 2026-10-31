// Proxy d3d11.dll entry points: export forwarding and device creation.
#include <cstring>

#include "proxy.h"

HMODULE g_self;

extern "C" void* dx_table[];

namespace {

const char* const kExportNames[] = {
#include "exports.inc"
};

HMODULE g_real;

HMODULE RealD3D11() {
    if (g_real) return g_real;
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    wcscpy(path + n, L"\\d3d11.dll");
    HMODULE module = LoadLibraryW(path);
    if (module == g_self) {
        Log("the proxy was loaded as the system d3d11.dll; refusing to forward to itself");
        FreeLibrary(module);
        return nullptr;
    }
    if (!module) Log("cannot load %ls (error %lu)", path, GetLastError());
    g_real = module;
    return module;
}

HRESULT WINAPI MissingExport() { return E_NOTIMPL; }

bool IsSystemModule(void* address) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)address, &module))
        return false;
    wchar_t path[MAX_PATH * 2], windows[MAX_PATH];
    GetModuleFileNameW(module, path, MAX_PATH * 2);
    UINT n = GetWindowsDirectoryW(windows, MAX_PATH);
    return n && _wcsnicmp(path, windows, n) == 0;
}

std::string ModuleName(void* address) {
    HMODULE module = nullptr;
    char path[MAX_PATH * 2] = "?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)address, &module))
        GetModuleFileNameA(module, path, sizeof(path));
    const char* slash = strrchr(path, '\\');
    return slash ? slash + 1 : path;
}

std::string LevelList(const D3D_FEATURE_LEVEL* levels, UINT count) {
    std::string s;
    for (UINT i = 0; i < count; i++) s += (i ? " " : "") + LevelName(levels[i]);
    return s;
}

void LogAdapter(ID3D11Device* device) {
    IDXGIDevice* dxgi = nullptr;
    IDXGIAdapter* adapter = nullptr;
    if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter))) {
        DXGI_ADAPTER_DESC desc;
        if (SUCCEEDED(adapter->GetDesc(&desc)))
            Log("adapter: %ls (vendor 0x%04x, device 0x%04x, %u MB video memory)", desc.Description, desc.VendorId,
                desc.DeviceId, unsigned(desc.DedicatedVideoMemory >> 20));
    }
    if (adapter) adapter->Release();
    if (dxgi) dxgi->Release();
}

typedef HRESULT(WINAPI* CreateDeviceFn)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT,
                                        UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

HRESULT CreateDevice(void* caller, IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, HMODULE software, UINT flags,
                     const D3D_FEATURE_LEVEL* levels, UINT numLevels, UINT sdk, ID3D11Device** outDevice,
                     D3D_FEATURE_LEVEL* outLevel, ID3D11DeviceContext** outContext) {
    HMODULE real = RealD3D11();
    CreateDeviceFn create = real ? (CreateDeviceFn)GetProcAddress(real, "D3D11CreateDevice") : nullptr;
    if (!create) return E_FAIL;

    static const D3D_FEATURE_LEVEL kDefault[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
                                                 D3D_FEATURE_LEVEL_9_3,  D3D_FEATURE_LEVEL_9_2,  D3D_FEATURE_LEVEL_9_1};
    if (!levels || !numLevels) {
        levels = kDefault;
        numLevels = 6;
    }
    bool system = IsSystemModule(caller);
    Log("D3D11CreateDevice from %s: driver %d, flags 0x%x, levels [%s]%s", ModuleName(caller).c_str(), type, flags,
        LevelList(levels, numLevels).c_str(), outDevice ? "" : " (query only)");

    std::vector<D3D_FEATURE_LEVEL> request(levels, levels + numLevels);
    D3D_FEATURE_LEVEL fake = D3D_FEATURE_LEVEL(0);
    if (!g_config.native) {
        std::vector<D3D_FEATURE_LEVEL> supported;
        for (D3D_FEATURE_LEVEL l : request)
            if (l <= g_config.maxLevel) supported.push_back(l);
        bool wantFake = g_config.fake == Config::FakeAlways ||
                        (g_config.fake == Config::FakeAuto && supported.empty() && !system);
        if (wantFake) {
            fake = request[0];
            for (D3D_FEATURE_LEVEL l : request)
                if (l == D3D_FEATURE_LEVEL_11_0) fake = l;
            for (D3D_FEATURE_LEVEL l : request)
                if (fake != D3D_FEATURE_LEVEL_11_0 && l < fake && l >= D3D_FEATURE_LEVEL_11_0) fake = l;
            supported.clear();
            for (D3D_FEATURE_LEVEL l : {D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0})
                if (l <= g_config.maxLevel) supported.push_back(l);
        }
        if (supported.empty()) {
            Log("  no requested level is available below %s; failing like DX10 hardware would",
                LevelName(g_config.maxLevel).c_str());
            return DXGI_ERROR_UNSUPPORTED;
        }
        request = supported;
    }

    if (g_config.warp) {
        adapter = nullptr;
        type = D3D_DRIVER_TYPE_WARP;
    }
    UINT realFlags = flags | (g_config.debugLayer ? D3D11_CREATE_DEVICE_DEBUG : 0);
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL(0);
    HRESULT hr = create(adapter, type, software, realFlags, request.data(), UINT(request.size()), sdk,
                        outDevice ? &device : nullptr, &got, outContext ? &context : nullptr);
    if (FAILED(hr) && realFlags != flags) {
        Log("  debug layer unavailable (0x%08lx), creating without it", hr);
        hr = create(adapter, type, software, flags, request.data(), UINT(request.size()), sdk,
                    outDevice ? &device : nullptr, &got, outContext ? &context : nullptr);
    }
    D3D_FEATURE_LEVEL reported = fake ? fake : got;
    bool downlevel = got < D3D_FEATURE_LEVEL_11_0 || (g_config.emulate && SUCCEEDED(hr));
    if (g_config.emulate && SUCCEEDED(hr) && got >= D3D_FEATURE_LEVEL_11_0)
        Log("  Emulate=1: treating this %s device like a feature level 10 one", LevelName(got).c_str());
    if (FAILED(hr)) {
        Log("  real D3D11CreateDevice failed: 0x%08lx", hr);
    } else {
        Log("  created at feature level %s, game sees %s", LevelName(got).c_str(), LevelName(reported).c_str());
        if (device) {
            if (g_config.translate) {
                RegisterDevice(device, got, reported, downlevel);
                HookDevice(device);
                if (downlevel) {
                    ID3D11DeviceContext* immediate = nullptr;
                    device->GetImmediateContext(&immediate);  // hooked: registers it
                    immediate->Release();
                }
            }
            HookFactoryOf(device);
            LogAdapter(device);
        }
    }
    if (context && device && g_config.translate && downlevel) context = WrapContext(context);
    if (outDevice) *outDevice = device;
    if (outContext) *outContext = context;
    if (outLevel) *outLevel = SUCCEEDED(hr) ? reported : D3D_FEATURE_LEVEL(0);
    return hr;
}

}  // namespace

extern "C" HRESULT WINAPI Proxy_D3D11CreateDevice(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, HMODULE software,
                                                  UINT flags, const D3D_FEATURE_LEVEL* levels, UINT numLevels,
                                                  UINT sdk, ID3D11Device** device, D3D_FEATURE_LEVEL* level,
                                                  ID3D11DeviceContext** context) {
    EnsureInit();
    return CreateDevice(__builtin_return_address(0), adapter, type, software, flags, levels, numLevels, sdk, device,
                        level, context);
}

extern "C" HRESULT WINAPI Proxy_D3D11CreateDeviceAndSwapChain(
    IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, HMODULE software, UINT flags, const D3D_FEATURE_LEVEL* levels,
    UINT numLevels, UINT sdk, const DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** outSwapChain,
    ID3D11Device** outDevice, D3D_FEATURE_LEVEL* outLevel, ID3D11DeviceContext** outContext) {
    EnsureInit();
    if (outSwapChain && !desc) return E_INVALIDARG;
    if (outSwapChain) *outSwapChain = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    HRESULT hr = CreateDevice(__builtin_return_address(0), adapter, type, software, flags, levels, numLevels, sdk,
                              (outSwapChain || outDevice) ? &device : nullptr, outLevel, outContext ? &context : nullptr);
    if (SUCCEEDED(hr) && outSwapChain && device) {
        IDXGIDevice* dxgi = nullptr;
        IDXGIAdapter* realAdapter = nullptr;
        IDXGIFactory* factory = nullptr;
        hr = device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgi);
        if (SUCCEEDED(hr)) hr = dxgi->GetAdapter(&realAdapter);
        if (SUCCEEDED(hr)) hr = realAdapter->GetParent(__uuidof(IDXGIFactory), (void**)&factory);
        if (SUCCEEDED(hr)) {
            DXGI_SWAP_CHAIN_DESC copy = *desc;
            hr = factory->CreateSwapChain(device, &copy, outSwapChain);
        }
        if (factory) factory->Release();
        if (realAdapter) realAdapter->Release();
        if (dxgi) dxgi->Release();
    }
    if (FAILED(hr)) {
        if (context) context->Release();
        if (device) device->Release();
        if (outDevice) *outDevice = nullptr;
        if (outContext) *outContext = nullptr;
        return hr;
    }
    if (outDevice) *outDevice = device;
    else if (device) device->Release();
    if (outContext) *outContext = context;
    return hr;
}

// Called the first time an export is used: picks our implementation or the system one.
extern "C" void* dx_resolve_export(unsigned index) {
    EnsureInit();
    const char* name = kExportNames[index];
    void* function = nullptr;
    if (!strcmp(name, "D3D11CreateDevice")) {
        function = (void*)&Proxy_D3D11CreateDevice;
    } else if (!strcmp(name, "D3D11CreateDeviceAndSwapChain")) {
        function = (void*)&Proxy_D3D11CreateDeviceAndSwapChain;
    } else {
        HMODULE real = RealD3D11();
        function = real ? (void*)GetProcAddress(real, name) : nullptr;
        Log("forwarding %s to the system d3d11.dll%s", name, function ? "" : " - NOT FOUND");
        if (!function) function = (void*)&MissingExport;
    }
    InterlockedExchangePointer(&dx_table[index], function);
    return function;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = instance;
        DisableThreadLibraryCalls(instance);
    } else if (reason == DLL_PROCESS_DETACH) {
        Log("unloading: shaders converted %u, unchanged %u, placeholders %u; draws skipped %u, dispatches skipped %u",
            g_stats.shadersConverted.load(), g_stats.shadersPassed.load(), g_stats.shadersDummy.load(),
            g_stats.drawsDropped.load(), g_stats.dispatchesDropped.load());
        LogFlush();
    }
    return TRUE;
}
