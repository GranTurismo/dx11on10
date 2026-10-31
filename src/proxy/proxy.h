// Shared declarations of the proxy d3d11.dll.
#pragma once
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>

#include <atomic>
#include <string>
#include <vector>

#include "slots.h"

// ---- config.cpp / log.cpp ----

struct Config {
    D3D_FEATURE_LEVEL maxLevel = D3D_FEATURE_LEVEL_10_1;  // highest level the real device may have
    bool native = false;                                  // no cap at all (reference runs)
    enum Fake { FakeAuto, FakeAlways, FakeOff } fake = FakeAuto;
    bool translate = true;  // false: only cap the level, like plain DX10 hardware
    bool warp = false;
    bool debugLayer = false;
    int dumpShaders = 0;  // 1 = every shader, 2 = only those that could not be converted
    std::vector<unsigned> screenshots;
    unsigned exitAfterFrame = 0;
    unsigned traceFrame = 0;  // log depth bookkeeping during this frame
    bool emulate = false;      // debugging: treat a feature level 11 device like a 10 one
    bool keepCompute = false;  // debugging, with emulate: leave compute shaders running
    std::vector<unsigned> keepComputeIds;  // debugging, with emulate: leave just these running
};

extern Config g_config;

void EnsureInit();
void InstallCrashLogger();
std::wstring OutputPath(const std::wstring& name);
void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
bool FirstTime(const std::string& key);  // true only the first time a key is seen
void LogFlush();
std::string LevelName(D3D_FEATURE_LEVEL level);
unsigned CurrentFrame();  // frames presented so far
bool Tracing();           // true during the configured trace frame

struct Stats {
    std::atomic<unsigned> shadersConverted{0}, shadersPassed{0}, shadersDummy{0};
    std::atomic<unsigned> drawsDropped{0}, dispatchesDropped{0}, indirectEmulated{0};
    std::atomic<unsigned> draws{0}, dispatchesEmulated{0};
};
extern Stats g_stats;

// ---- hooks.cpp ----

void Hook(void* object, int slot, void* function);
void* Original(void* object, int slot);
bool IsHookedTable(void** table);  // true when Hook() has patched this vtable
template <class F>
F Orig(void* object, int slot) {
    return reinterpret_cast<F>(Original(object, slot));
}

struct DeviceInfo {
    ID3D11Device* device;
    D3D_FEATURE_LEVEL real, reported;
    bool downlevel;  // real level is below 11_0: convert shaders and filter what it can't do
};

void RegisterDevice(ID3D11Device* device, D3D_FEATURE_LEVEL real, D3D_FEATURE_LEVEL reported, bool downlevel);
const DeviceInfo* FindDevice(ID3D11Device* device);

enum StageBit : unsigned { kVS = 1, kPS = 2, kGS = 4, kHS = 8, kDS = 16, kCS = 32 };

struct ContextState {
    ID3D11DeviceContext* context;
    ID3D11Device* device;
    bool downlevel;
    bool immediate;
    unsigned dummyStages;  // stages that have a placeholder shader bound
    bool patchTopology;
    ID3D11Buffer* argsStaging;
    void* depth;  // depth.cpp bookkeeping
    unsigned csTag;          // shader number of the bound placeholder compute shader
    const void* csShader;    // the bound placeholder compute shader
    const void* csUavs[8];   // placeholder UAVs bound for compute
};

ContextState* FindContext(ID3D11DeviceContext* context);
void HookDevice(ID3D11Device* device);
void HookContext(ID3D11DeviceContext* context, ID3D11Device* device, bool downlevel);
void HookFactoryOf(ID3D11Device* device);

// Placeholder objects for things the hardware can't create.
bool IsDummy(const void* object);
ID3D11DeviceChild* MakeDummyShader(ID3D11Device* device, unsigned stage, unsigned tag);
unsigned DummyTag(const void* object);  // the shader number a placeholder stands for
void ForgetDummy(const void* object);   // called when a placeholder is destroyed
ID3D11UnorderedAccessView* MakeDummyUAV(ID3D11Device* device, ID3D11Resource* resource,
                                         const D3D11_UNORDERED_ACCESS_VIEW_DESC* desc);

// ---- depth.cpp: read-only depth views via copies ----

void DepthOnTextureCreated(ID3D11Texture2D* texture, const D3D11_TEXTURE2D_DESC* desc);
void DepthOnViewCreated(ID3D11Resource* resource, ID3D11DepthStencilView* dsv, bool readOnlySubstitute);
void DepthOnViewCreated(ID3D11Resource* resource, ID3D11ShaderResourceView* srv);
void DepthOnSetShaderResources(ID3D11DeviceContext* context, unsigned stage, UINT start, UINT count,
                               ID3D11ShaderResourceView* const* views, ID3D11ShaderResourceView** out);
void DepthOnSetDepthView(ID3D11DeviceContext* context, ID3D11DepthStencilView* dsv);
void DepthOnClearDepthView(ID3D11DepthStencilView* dsv);
void DepthOnWrite(ID3D11Resource* destination, const char* how);
void DepthBeforeDraw(ID3D11DeviceContext* context, ContextState* state);
void DepthAfterDraw(ContextState* state);
void DepthReset(ContextState* state);

// ---- compute.cpp: pixel-shader versions of known compute shaders ----

// Called for each compute shader replaced by a placeholder: prepares running it anyway.
void PrepareComputeEmulation(ID3D11Device* device, const void* shader, const void* code, size_t size, unsigned id,
                             int targetMinor);
void ForgetComputeEmulation(const void* shader);
bool EmulateDispatch(ID3D11DeviceContext* context, ContextState* state, UINT x, UINT y, UINT z);

// ---- device.cpp / context.cpp / present.cpp ----

void InstallDeviceHooks(void* device, int interfaceLevel);
ID3D11DeviceContext* WrapContext(ID3D11DeviceContext* real);  // game-facing wrapper; the reference transfers
void InstallFactoryHooks(void* factory, bool factory2);
void InstallSwapChainHooks(IDXGISwapChain* swapChain);
