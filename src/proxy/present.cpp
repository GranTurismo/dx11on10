// DXGI hooks: notice swap chains, count frames, take screenshots, surface debug-layer messages.
#include <algorithm>

#include "../common/capture.h"
#include "proxy.h"

namespace {

std::atomic<unsigned> g_frame{0};
LARGE_INTEGER g_lastReport;
unsigned g_lastReportFrame = 0;

void DrainDebugMessages(IDXGISwapChain* swapChain) {
    if (!g_config.debugLayer) return;
    ID3D11Device* device = nullptr;
    if (FAILED(swapChain->GetDevice(__uuidof(ID3D11Device), (void**)&device))) return;
    ID3D11InfoQueue* queue = nullptr;
    if (SUCCEEDED(device->QueryInterface(__uuidof(ID3D11InfoQueue), (void**)&queue))) {
        UINT64 n = queue->GetNumStoredMessages();
        for (UINT64 i = 0; i < n; i++) {
            SIZE_T size = 0;
            queue->GetMessage(i, nullptr, &size);
            std::vector<char> buf(size);
            D3D11_MESSAGE* m = reinterpret_cast<D3D11_MESSAGE*>(buf.data());
            if (FAILED(queue->GetMessage(i, m, &size))) continue;
            std::string text(m->pDescription, m->DescriptionByteLength ? m->DescriptionByteLength - 1 : 0);
            if (m->Severity <= D3D11_MESSAGE_SEVERITY_WARNING && FirstTime("debug " + text))
                Log("debug layer: %s", text.c_str());
        }
        queue->ClearStoredMessages();
        queue->Release();
    }
    device->Release();
}

void BeforePresent(IDXGISwapChain* swapChain, UINT flags) {
    if (flags & DXGI_PRESENT_TEST) return;
    unsigned frame = ++g_frame;
    if (frame == 1) {
        Log("first frame presented");
        QueryPerformanceCounter(&g_lastReport);
    }
    if (std::find(g_config.screenshots.begin(), g_config.screenshots.end(), frame) != g_config.screenshots.end()) {
        std::wstring path = OutputPath(L"dx11on10_frame" + std::to_wstring(frame) + L".bmp");
        std::string error;
        if (CaptureBackBuffer(swapChain, path, error)) Log("screenshot of frame %u saved", frame);
        else Log("screenshot of frame %u failed: %s", frame, error.c_str());
    }
    DrainDebugMessages(swapChain);
    if (frame % 600 == 0) {
        LARGE_INTEGER now, freq;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&freq);
        double seconds = double(now.QuadPart - g_lastReport.QuadPart) / double(freq.QuadPart);
        Log("frame %u: %.1f fps; draws %u; shaders converted %u, unchanged %u, placeholders %u; draws skipped %u, "
            "dispatches skipped %u, emulated %u, indirect draws emulated %u",
            frame, (frame - g_lastReportFrame) / seconds, g_stats.draws.load(), g_stats.shadersConverted.load(),
            g_stats.shadersPassed.load(), g_stats.shadersDummy.load(), g_stats.drawsDropped.load(),
            g_stats.dispatchesDropped.load(), g_stats.dispatchesEmulated.load(), g_stats.indirectEmulated.load());
        g_lastReport = now;
        g_lastReportFrame = frame;
    }
}

void AfterPresent() {
    if (g_config.exitAfterFrame && g_frame >= g_config.exitAfterFrame) {
        Log("exiting after frame %u as configured; shaders converted %u, placeholders %u, draws skipped %u",
            g_frame.load(), g_stats.shadersConverted.load(), g_stats.shadersDummy.load(), g_stats.drawsDropped.load());
        LogFlush();
        TerminateProcess(GetCurrentProcess(), 0);
    }
}

HRESULT STDMETHODCALLTYPE Hk_Present(IDXGISwapChain* self, UINT sync, UINT flags) {
    auto orig = Orig<decltype(&Hk_Present)>(self, g_slots.swapPresent);
    BeforePresent(self, flags);
    HRESULT hr = orig(self, sync, flags);
    AfterPresent();
    return hr;
}

HRESULT STDMETHODCALLTYPE Hk_Present1(IDXGISwapChain1* self, UINT sync, UINT flags,
                                      const DXGI_PRESENT_PARAMETERS* params) {
    auto orig = Orig<decltype(&Hk_Present1)>(self, g_slots.swap1Present1);
    BeforePresent(self, flags);
    HRESULT hr = orig(self, sync, flags, params);
    AfterPresent();
    return hr;
}

void OnSwapChain(HRESULT hr, IDXGISwapChain* swapChain, const char* how, UINT width, UINT height, DXGI_FORMAT format,
                 UINT buffers, int effect) {
    if (FAILED(hr) || !swapChain) {
        Log("%s failed: 0x%08lx", how, hr);
        return;
    }
    Log("%s: %ux%u format %d, %u buffers, swap effect %d", how, width, height, format, buffers, effect);
    InstallSwapChainHooks(swapChain);
}

HRESULT STDMETHODCALLTYPE Hk_CreateSwapChain(IDXGIFactory* self, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc,
                                             IDXGISwapChain** out) {
    auto orig = Orig<decltype(&Hk_CreateSwapChain)>(self, g_slots.factoryCreateSwapChain);
    HRESULT hr = orig(self, device, desc, out);
    if (desc)
        OnSwapChain(hr, out ? *out : nullptr, "CreateSwapChain", desc->BufferDesc.Width, desc->BufferDesc.Height,
                    desc->BufferDesc.Format, desc->BufferCount, desc->SwapEffect);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hk_CreateSwapChainForHwnd(IDXGIFactory2* self, IUnknown* device, HWND window,
                                                    const DXGI_SWAP_CHAIN_DESC1* desc,
                                                    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen,
                                                    IDXGIOutput* output, IDXGISwapChain1** out) {
    auto orig = Orig<decltype(&Hk_CreateSwapChainForHwnd)>(self, g_slots.factory2CreateSwapChainForHwnd);
    HRESULT hr = orig(self, device, window, desc, fullscreen, output, out);
    if (desc)
        OnSwapChain(hr, out ? *out : nullptr, "CreateSwapChainForHwnd", desc->Width, desc->Height, desc->Format,
                    desc->BufferCount, desc->SwapEffect);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hk_CreateSwapChainForCoreWindow(IDXGIFactory2* self, IUnknown* device, IUnknown* window,
                                                          const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* output,
                                                          IDXGISwapChain1** out) {
    auto orig = Orig<decltype(&Hk_CreateSwapChainForCoreWindow)>(self, g_slots.factory2CreateSwapChainForCoreWindow);
    HRESULT hr = orig(self, device, window, desc, output, out);
    if (desc)
        OnSwapChain(hr, out ? *out : nullptr, "CreateSwapChainForCoreWindow", desc->Width, desc->Height, desc->Format,
                    desc->BufferCount, desc->SwapEffect);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hk_CreateSwapChainForComposition(IDXGIFactory2* self, IUnknown* device,
                                                           const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* output,
                                                           IDXGISwapChain1** out) {
    auto orig =
        Orig<decltype(&Hk_CreateSwapChainForComposition)>(self, g_slots.factory2CreateSwapChainForComposition);
    HRESULT hr = orig(self, device, desc, output, out);
    if (desc)
        OnSwapChain(hr, out ? *out : nullptr, "CreateSwapChainForComposition", desc->Width, desc->Height, desc->Format,
                    desc->BufferCount, desc->SwapEffect);
    return hr;
}

}  // namespace

unsigned CurrentFrame() { return g_frame.load(); }

bool Tracing() { return g_config.traceFrame && g_frame.load() + 1 == g_config.traceFrame; }

void InstallFactoryHooks(void* factory, bool factory2) {
    Hook(factory, g_slots.factoryCreateSwapChain, (void*)&Hk_CreateSwapChain);
    if (!factory2) return;
    Hook(factory, g_slots.factory2CreateSwapChainForHwnd, (void*)&Hk_CreateSwapChainForHwnd);
    Hook(factory, g_slots.factory2CreateSwapChainForCoreWindow, (void*)&Hk_CreateSwapChainForCoreWindow);
    Hook(factory, g_slots.factory2CreateSwapChainForComposition, (void*)&Hk_CreateSwapChainForComposition);
}

void InstallSwapChainHooks(IDXGISwapChain* swapChain) {
    Hook(swapChain, g_slots.swapPresent, (void*)&Hk_Present);
    IDXGISwapChain1* swapChain1 = nullptr;
    if (SUCCEEDED(swapChain->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&swapChain1))) {
        Hook(swapChain1, g_slots.swapPresent, (void*)&Hk_Present);
        Hook(swapChain1, g_slots.swap1Present1, (void*)&Hk_Present1);
        swapChain1->Release();
    }
}
