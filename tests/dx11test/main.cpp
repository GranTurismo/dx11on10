// dx11test: a tiny "DX11-only game". It refuses to run below feature level 11_0 and
// uses Shader Model 5 everywhere, like the games the proxy is for. Rendering is a
// function of the frame number, so screenshots of the same frame are comparable.
//
//   dx11test [--frames N] [--shot file.bmp] [--tess] [--compute] [--swapchain blt|flip|both]
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <windows.h>

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../../src/common/capture.h"

namespace {

FILE* g_log;

void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(g_log, fmt, args);
    va_end(args);
    fputc('\n', g_log);
    fflush(g_log);
}

struct Options {
    unsigned frames = 120;
    std::wstring shot;
    bool tess = false, compute = false, depthRead = false;
    std::string swapchain = "blt";
};

struct Mat {
    float m[4][4];
};

Mat Identity() {
    Mat r = {};
    for (int i = 0; i < 4; i++) r.m[i][i] = 1;
    return r;
}

Mat Mul(const Mat& a, const Mat& b) {
    Mat r = {};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            for (int k = 0; k < 4; k++) r.m[i][j] += a.m[i][k] * b.m[k][j];
    return r;
}

Mat Transpose(const Mat& a) {
    Mat r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) r.m[i][j] = a.m[j][i];
    return r;
}

Mat RotateY(float a) {
    Mat r = Identity();
    r.m[0][0] = cosf(a), r.m[0][2] = -sinf(a), r.m[2][0] = sinf(a), r.m[2][2] = cosf(a);
    return r;
}

Mat RotateX(float a) {
    Mat r = Identity();
    r.m[1][1] = cosf(a), r.m[1][2] = sinf(a), r.m[2][1] = -sinf(a), r.m[2][2] = cosf(a);
    return r;
}

Mat Translate(float x, float y, float z) {
    Mat r = Identity();
    r.m[3][0] = x, r.m[3][1] = y, r.m[3][2] = z;
    return r;
}

Mat Scale(float s) {
    Mat r = Identity();
    r.m[0][0] = r.m[1][1] = r.m[2][2] = s;
    return r;
}

// Row-vector convention (v * M), left-handed, depth 0..1.
Mat Perspective(float fovY, float aspect, float zn, float zf) {
    Mat r = {};
    float y = 1 / tanf(fovY / 2);
    r.m[0][0] = y / aspect;
    r.m[1][1] = y;
    r.m[2][2] = zf / (zf - zn);
    r.m[2][3] = 1;
    r.m[3][2] = -zn * zf / (zf - zn);
    return r;
}

struct FrameConstants {
    Mat worldViewProj;
    Mat world;
    float lightDir[4];
    float tint[4];
    float params[4];
};

struct Vertex {
    float pos[3], normal[3], uv[2];
};

struct Particle {
    float pos[3], color[4];
};

typedef HRESULT(WINAPI* CompileFn)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT,
                                   UINT, ID3DBlob**, ID3DBlob**);

class App {
public:
    explicit App(const Options& o) : opt_(o) {}
    int Run();

private:
    bool CreateWindowAndDevice();
    bool CreateSwapChain();
    bool LoadShaders();
    ID3DBlob* Compile(const char* entry, const char* target);
    bool CreateResources();
    void Render(unsigned frame);
    void DrawMesh(const Mat& world, ID3D11PixelShader* ps, const float tint[3]);
    void SetConstants(const Mat& world, const float tint[3]);

    Options opt_;
    HWND window_ = nullptr;
    UINT width_ = 1280, height_ = 720;
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    IDXGISwapChain* swapChain_ = nullptr;
    ID3D11RenderTargetView* rtv_ = nullptr;
    ID3D11DepthStencilView* dsv_ = nullptr;
    std::string source_;
    CompileFn compile_ = nullptr;

    ID3D11VertexShader* vsMesh_ = nullptr;
    ID3D11PixelShader* psMesh_ = nullptr;
    ID3D11PixelShader* psDepth_ = nullptr;
    ID3D11VertexShader* vsParticle_ = nullptr;
    ID3D11GeometryShader* gsParticle_ = nullptr;
    ID3D11PixelShader* psParticle_ = nullptr;
    ID3D11VertexShader* vsPatch_ = nullptr;
    ID3D11HullShader* hs_ = nullptr;
    ID3D11DomainShader* ds_ = nullptr;
    ID3D11ComputeShader* cs_ = nullptr;
    ID3D11InputLayout* meshLayout_ = nullptr;
    ID3D11InputLayout* particleLayout_ = nullptr;

    ID3D11Buffer* cubeVB_ = nullptr;
    ID3D11Buffer* cubeIB_ = nullptr;
    ID3D11Buffer* particleVB_ = nullptr;
    ID3D11Buffer* constants_ = nullptr;
    ID3D11ShaderResourceView* checker_ = nullptr;
    ID3D11Texture2D* csTexture_ = nullptr;
    ID3D11ShaderResourceView* csView_ = nullptr;
    ID3D11UnorderedAccessView* csUav_ = nullptr;
    ID3D11SamplerState* sampler_ = nullptr;
    ID3D11BlendState* additive_ = nullptr;
    ID3D11DepthStencilState* depthOn_ = nullptr;
    ID3D11DepthStencilState* depthRead_ = nullptr;
    ID3D11RasterizerState* raster_ = nullptr;
    ID3D11DepthStencilView* dsvReadOnly_ = nullptr;
    ID3D11ShaderResourceView* depthSrv_ = nullptr;
    ID3D11VertexShader* vsFullscreen_ = nullptr;
    ID3D11PixelShader* psFog_ = nullptr;
    ID3D11BlendState* alphaBlend_ = nullptr;
    ID3D11DepthStencilState* depthTestOnly_ = nullptr;
    Mat viewProj_;
};

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_DESTROY) PostQuitMessage(0);
    return DefWindowProcW(h, msg, w, l);
}

bool App::CreateWindowAndDevice() {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"dx11test";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    RECT r = {0, 0, LONG(width_), LONG(height_)};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    window_ = CreateWindowW(L"dx11test", L"dx11test - DirectX 11 only", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 40,
                            r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);

    // Like many DX11-only games: ask for 11_0 and nothing else.
    D3D_FEATURE_LEVEL wanted = D3D_FEATURE_LEVEL_11_0, got;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   &wanted, 1, D3D11_SDK_VERSION, &device_, &got, &context_);
    if (FAILED(hr)) {
        Log("FAIL: this program requires a DirectX 11 graphics card (D3D11CreateDevice: 0x%08lx)", hr);
        return false;
    }
    if (device_->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0) {
        Log("FAIL: feature level %x is below 11_0", device_->GetFeatureLevel());
        return false;
    }
    Log("device created, feature level reported: %x", got);
    return true;
}

bool App::CreateSwapChain() {
    IDXGIDevice* dxgi;
    IDXGIAdapter* adapter;
    IDXGIFactory* factory;
    device_->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgi);
    dxgi->GetAdapter(&adapter);
    adapter->GetParent(__uuidof(IDXGIFactory), (void**)&factory);
    HRESULT hr;
    if (opt_.swapchain == "flip") {
        IDXGIFactory2* factory2 = nullptr;
        factory->QueryInterface(__uuidof(IDXGIFactory2), (void**)&factory2);
        DXGI_SWAP_CHAIN_DESC1 d = {};
        d.Width = width_, d.Height = height_, d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc = {1, 0};
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 2;
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        IDXGISwapChain1* sc1 = nullptr;
        hr = factory2->CreateSwapChainForHwnd(device_, window_, &d, nullptr, nullptr, &sc1);
        swapChain_ = sc1;
        factory2->Release();
    } else {
        DXGI_SWAP_CHAIN_DESC d = {};
        d.BufferDesc.Width = width_, d.BufferDesc.Height = height_;
        d.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc = {1, 0};
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 1;
        d.OutputWindow = window_;
        d.Windowed = TRUE;
        d.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        hr = factory->CreateSwapChain(device_, &d, &swapChain_);
    }
    factory->Release();
    adapter->Release();
    dxgi->Release();
    if (FAILED(hr)) {
        Log("FAIL: swap chain creation 0x%08lx", hr);
        return false;
    }
    ID3D11Texture2D* back;
    swapChain_->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back);
    device_->CreateRenderTargetView(back, nullptr, &rtv_);
    back->Release();

    D3D11_TEXTURE2D_DESC dd = {};
    dd.Width = width_, dd.Height = height_, dd.MipLevels = 1, dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dd.SampleDesc = {1, 0};
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (opt_.depthRead) {
        // Readable depth with a read-only view, like deferred renderers use.
        dd.Format = DXGI_FORMAT_R24G8_TYPELESS;
        dd.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
    }
    ID3D11Texture2D* depth;
    device_->CreateTexture2D(&dd, nullptr, &depth);
    D3D11_DEPTH_STENCIL_VIEW_DESC dv = {};
    dv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    device_->CreateDepthStencilView(depth, &dv, &dsv_);
    if (opt_.depthRead) {
        dv.Flags = D3D11_DSV_READ_ONLY_DEPTH | D3D11_DSV_READ_ONLY_STENCIL;
        HRESULT hr = device_->CreateDepthStencilView(depth, &dv, &dsvReadOnly_);
        Log("  CreateDepthStencilView(read-only): 0x%08lx", hr);
        D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = 1;
        hr = device_->CreateShaderResourceView(depth, &sv, &depthSrv_);
        Log("  CreateShaderResourceView(depth): 0x%08lx", hr);
    }
    depth->Release();
    Log("swap chain created (%s)", opt_.swapchain.c_str());
    return true;
}

ID3DBlob* App::Compile(const char* entry, const char* target) {
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = compile_(source_.data(), source_.size(), "scene.hlsl", nullptr, nullptr, entry, target,
                          D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (errors) {
        Log("%s", (const char*)errors->GetBufferPointer());
        errors->Release();
    }
    if (FAILED(hr)) Log("FAIL: compiling %s (%s): 0x%08lx", entry, target, hr);
    return code;
}

#define CREATE(kind, blob, out)                                                                                    \
    do {                                                                                                           \
        HRESULT h = device_->Create##kind((blob)->GetBufferPointer(), (blob)->GetBufferSize(), nullptr, out);     \
        Log("  Create" #kind ": 0x%08lx", h);                                                                      \
        if (FAILED(h)) return false;                                                                               \
    } while (0)

bool App::LoadShaders() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    wcscpy(wcsrchr(path, L'\\') + 1, L"scene.hlsl");
    FILE* f = _wfopen(path, L"rb");
    if (!f) {
        Log("FAIL: scene.hlsl not found next to the exe");
        return false;
    }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) source_.append(buf, n);
    fclose(f);
    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    compile_ = compiler ? (CompileFn)GetProcAddress(compiler, "D3DCompile") : nullptr;
    if (!compile_) {
        Log("FAIL: d3dcompiler_47.dll not available");
        return false;
    }

    ID3DBlob* vs = Compile("vs_mesh", "vs_5_0");
    ID3DBlob* ps = Compile("ps_mesh", "ps_5_0");
    ID3DBlob* psDepth = Compile("ps_depth_ge", "ps_5_0");
    ID3DBlob* vsP = Compile("vs_particle", "vs_5_0");
    ID3DBlob* gsP = Compile("gs_particle", "gs_5_0");
    ID3DBlob* psP = Compile("ps_particle", "ps_5_0");
    if (!vs || !ps || !psDepth || !vsP || !gsP || !psP) return false;
    CREATE(VertexShader, vs, &vsMesh_);
    CREATE(PixelShader, ps, &psMesh_);
    CREATE(PixelShader, psDepth, &psDepth_);
    CREATE(VertexShader, vsP, &vsParticle_);
    CREATE(GeometryShader, gsP, &gsParticle_);
    CREATE(PixelShader, psP, &psParticle_);

    D3D11_INPUT_ELEMENT_DESC mesh[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    HRESULT hr = device_->CreateInputLayout(mesh, 3, vs->GetBufferPointer(), vs->GetBufferSize(), &meshLayout_);
    Log("  CreateInputLayout(mesh): 0x%08lx", hr);
    if (FAILED(hr)) return false;
    D3D11_INPUT_ELEMENT_DESC particle[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    hr = device_->CreateInputLayout(particle, 2, vsP->GetBufferPointer(), vsP->GetBufferSize(), &particleLayout_);
    Log("  CreateInputLayout(particle): 0x%08lx", hr);
    if (FAILED(hr)) return false;

    if (opt_.tess) {
        ID3DBlob* vsT = Compile("vs_patch", "vs_5_0");
        ID3DBlob* hs = Compile("hs_main", "hs_5_0");
        ID3DBlob* ds = Compile("ds_main", "ds_5_0");
        if (!vsT || !hs || !ds) return false;
        CREATE(VertexShader, vsT, &vsPatch_);
        CREATE(HullShader, hs, &hs_);
        CREATE(DomainShader, ds, &ds_);
    }
    if (opt_.compute) {
        ID3DBlob* cs = Compile("cs_main", "cs_5_0");
        if (!cs) return false;
        CREATE(ComputeShader, cs, &cs_);
    }
    if (opt_.depthRead) {
        ID3DBlob* vsF = Compile("vs_fullscreen", "vs_5_0");
        ID3DBlob* fog = Compile("ps_fog", "ps_5_0");
        if (!vsF || !fog) return false;
        CREATE(VertexShader, vsF, &vsFullscreen_);
        CREATE(PixelShader, fog, &psFog_);
    }
    return true;
}

bool App::CreateResources() {
    // Cube: 6 faces x 4 vertices.
    std::vector<Vertex> v;
    std::vector<uint16_t> idx;
    const float n[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (int f = 0; f < 6; f++) {
        float nx = n[f][0], ny = n[f][1], nz = n[f][2];
        float u[3] = {ny + nz != 0 ? 1.0f : 0.0f, nx != 0 ? 1.0f : 0.0f, 0};
        if (nz != 0) u[0] = 1, u[1] = 0, u[2] = 0;
        float w[3] = {ny * u[2] - nz * u[1], nz * u[0] - nx * u[2], nx * u[1] - ny * u[0]};
        uint16_t base = uint16_t(v.size());
        for (int k = 0; k < 4; k++) {
            float a = (k & 1) ? 1.0f : -1.0f, b = (k & 2) ? 1.0f : -1.0f;
            Vertex vert = {{nx + a * u[0] + b * w[0], ny + a * u[1] + b * w[1], nz + a * u[2] + b * w[2]},
                           {nx, ny, nz},
                           {(a + 1) / 2, (b + 1) / 2}};
            v.push_back(vert);
        }
        uint16_t quad[6] = {0, 1, 2, 2, 1, 3};
        for (uint16_t q : quad) idx.push_back(base + q);
    }
    D3D11_BUFFER_DESC bd = {};
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.ByteWidth = UINT(v.size() * sizeof(Vertex));
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA init = {v.data()};
    if (FAILED(device_->CreateBuffer(&bd, &init, &cubeVB_))) return false;
    bd.ByteWidth = UINT(idx.size() * 2);
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    init.pSysMem = idx.data();
    if (FAILED(device_->CreateBuffer(&bd, &init, &cubeIB_))) return false;

    std::vector<Particle> particles;
    for (int i = 0; i < 48; i++) {
        float a = i * 6.2831853f / 48;
        particles.push_back({{cosf(a) * 3.2f, sinf(a * 3) * 0.4f, sinf(a) * 3.2f},
                             {0.5f + 0.5f * cosf(a), 0.5f + 0.5f * sinf(a * 2), 0.9f, 1}});
    }
    bd.ByteWidth = UINT(particles.size() * sizeof(Particle));
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    init.pSysMem = particles.data();
    if (FAILED(device_->CreateBuffer(&bd, &init, &particleVB_))) return false;

    bd = {};
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.ByteWidth = sizeof(FrameConstants);
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device_->CreateBuffer(&bd, nullptr, &constants_))) return false;

    // Checkerboard with a full mip chain made by GenerateMips.
    const UINT size = 256;
    std::vector<uint32_t> pixels(size * size);
    for (UINT y = 0; y < size; y++)
        for (UINT x = 0; x < size; x++) {
            bool c = ((x / 32) ^ (y / 32)) & 1;
            pixels[y * size + x] = c ? 0xff2080f0 : 0xfff0f0f0;
        }
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = td.Height = size;
    td.MipLevels = 0;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc = {1, 0};
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    ID3D11Texture2D* tex;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, &tex))) return false;
    context_->UpdateSubresource(tex, 0, nullptr, pixels.data(), size * 4, 0);
    device_->CreateShaderResourceView(tex, nullptr, &checker_);
    context_->GenerateMips(checker_);
    tex->Release();

    if (opt_.compute) {
        td.MipLevels = 1;
        td.MiscFlags = 0;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        std::vector<uint32_t> gray(size * size, 0xff404040);
        init = {gray.data(), size * 4, 0};
        HRESULT hr = device_->CreateTexture2D(&td, &init, &csTexture_);
        Log("  CreateTexture2D(UAV): 0x%08lx", hr);
        if (FAILED(hr)) return false;
        device_->CreateShaderResourceView(csTexture_, nullptr, &csView_);
        hr = device_->CreateUnorderedAccessView(csTexture_, nullptr, &csUav_);
        Log("  CreateUnorderedAccessView: 0x%08lx", hr);
        if (FAILED(hr)) return false;
    }

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_ANISOTROPIC;
    sd.MaxAnisotropy = 8;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    device_->CreateSamplerState(&sd, &sampler_);

    D3D11_BLEND_DESC blend = {};
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device_->CreateBlendState(&blend, &additive_);

    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    device_->CreateBlendState(&blend, &alphaBlend_);

    D3D11_DEPTH_STENCIL_DESC ds = {};
    ds.DepthEnable = TRUE;
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    device_->CreateDepthStencilState(&ds, &depthOn_);
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    device_->CreateDepthStencilState(&ds, &depthRead_);
    ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
    device_->CreateDepthStencilState(&ds, &depthTestOnly_);

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    device_->CreateRasterizerState(&rd, &raster_);
    return true;
}

void App::SetConstants(const Mat& world, const float tint[3]) {
    D3D11_MAPPED_SUBRESOURCE m;
    context_->Map(constants_, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
    FrameConstants* c = static_cast<FrameConstants*>(m.pData);
    c->worldViewProj = Transpose(Mul(world, viewProj_));
    c->world = Transpose(world);
    float l[4] = {-0.4f, -0.8f, 0.45f, 0};
    memcpy(c->lightDir, l, sizeof(l));
    c->tint[0] = tint[0], c->tint[1] = tint[1], c->tint[2] = tint[2], c->tint[3] = 1;
    c->params[0] = 0, c->params[1] = 0.12f;
    context_->Unmap(constants_, 0);
}

void App::DrawMesh(const Mat& world, ID3D11PixelShader* ps, const float tint[3]) {
    SetConstants(world, tint);
    UINT stride = sizeof(Vertex), offset = 0;
    context_->IASetInputLayout(meshLayout_);
    context_->IASetVertexBuffers(0, 1, &cubeVB_, &stride, &offset);
    context_->IASetIndexBuffer(cubeIB_, DXGI_FORMAT_R16_UINT, 0);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vsMesh_, nullptr, 0);
    context_->PSSetShader(ps, nullptr, 0);
    context_->DrawIndexed(36, 0, 0);
}

void App::Render(unsigned frame) {
    float t = frame * 0.02f;
    viewProj_ = Mul(Translate(0, 0, 7), Perspective(0.9f, float(width_) / height_, 0.1f, 50));
    float clear[4] = {0.07f, 0.09f, 0.14f, 1};
    context_->ClearRenderTargetView(rtv_, clear);
    context_->ClearDepthStencilView(dsv_, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1, 0);
    context_->OMSetRenderTargets(1, &rtv_, dsv_);
    D3D11_VIEWPORT vp = {0, 0, float(width_), float(height_), 0, 1};
    context_->RSSetViewports(1, &vp);
    context_->RSSetState(raster_);
    context_->OMSetDepthStencilState(depthOn_, 0);
    context_->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    context_->VSSetConstantBuffers(0, 1, &constants_);
    context_->PSSetConstantBuffers(0, 1, &constants_);
    context_->GSSetConstantBuffers(0, 1, &constants_);
    context_->DSSetConstantBuffers(0, 1, &constants_);
    context_->PSSetShaderResources(0, 1, &checker_);
    context_->PSSetSamplers(0, 1, &sampler_);

    const float white[3] = {1, 1, 1}, green[3] = {0.6f, 1, 0.7f}, orange[3] = {1, 0.7f, 0.4f};
    DrawMesh(Mul(RotateX(t * 0.7f), RotateY(t)), psMesh_, white);
    DrawMesh(Mul(Mul(Scale(0.6f), RotateY(-t * 1.3f)), Translate(-2.6f, -0.4f, 0.5f)), psDepth_, green);

    if (opt_.compute) {
        // Compute fills a texture that the right-hand cube shows.
        context_->CSSetShader(cs_, nullptr, 0);
        context_->CSSetConstantBuffers(0, 1, &constants_);
        context_->CSSetUnorderedAccessViews(0, 1, &csUav_, nullptr);
        context_->Dispatch(256 / 8, 256 / 8, 1);
        ID3D11UnorderedAccessView* none = nullptr;
        context_->CSSetUnorderedAccessViews(0, 1, &none, nullptr);
        context_->PSSetShaderResources(0, 1, &csView_);
        DrawMesh(Mul(Mul(Scale(0.6f), RotateY(t * 0.9f)), Translate(2.6f, -0.4f, 0.5f)), psMesh_, orange);
        context_->PSSetShaderResources(0, 1, &checker_);
    }

    if (opt_.tess) {
        // A tessellated cube rounded into a sphere, above the main cube.
        SetConstants(Mul(Mul(Scale(1.0f), RotateY(t)), Translate(0, 2.1f, 1.5f)), orange);
        UINT stride = sizeof(Vertex), offset = 0;
        context_->IASetInputLayout(meshLayout_);
        context_->IASetVertexBuffers(0, 1, &cubeVB_, &stride, &offset);
        context_->IASetIndexBuffer(cubeIB_, DXGI_FORMAT_R16_UINT, 0);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
        context_->VSSetShader(vsPatch_, nullptr, 0);
        context_->HSSetShader(hs_, nullptr, 0);
        context_->DSSetShader(ds_, nullptr, 0);
        context_->PSSetShader(psMesh_, nullptr, 0);
        context_->DrawIndexed(36, 0, 0);
        context_->HSSetShader(nullptr, nullptr, 0);
        context_->DSSetShader(nullptr, nullptr, 0);
    }

    if (opt_.depthRead) {
        // Fog read from the depth buffer while it stays bound read-only for testing.
        context_->OMSetRenderTargets(1, &rtv_, dsvReadOnly_);
        context_->PSSetShaderResources(1, 1, &depthSrv_);
        context_->OMSetDepthStencilState(depthTestOnly_, 0);
        context_->OMSetBlendState(alphaBlend_, nullptr, 0xffffffff);
        context_->IASetInputLayout(nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vsFullscreen_, nullptr, 0);
        context_->PSSetShader(psFog_, nullptr, 0);
        context_->Draw(3, 0);
        ID3D11ShaderResourceView* none = nullptr;
        context_->PSSetShaderResources(1, 1, &none);
        context_->OMSetRenderTargets(1, &rtv_, dsv_);
        context_->OMSetDepthStencilState(depthOn_, 0);
    }

    // Particles: points expanded to quads by a geometry shader, additive blending.
    SetConstants(RotateY(t * 0.5f), white);
    UINT stride = sizeof(Particle), offset = 0;
    context_->IASetInputLayout(particleLayout_);
    context_->IASetVertexBuffers(0, 1, &particleVB_, &stride, &offset);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    context_->VSSetShader(vsParticle_, nullptr, 0);
    context_->GSSetShader(gsParticle_, nullptr, 0);
    context_->PSSetShader(psParticle_, nullptr, 0);
    context_->OMSetBlendState(additive_, nullptr, 0xffffffff);
    context_->OMSetDepthStencilState(depthRead_, 0);
    context_->Draw(48, 0);
    context_->GSSetShader(nullptr, nullptr, 0);
}

int App::Run() {
    if (!CreateWindowAndDevice()) return 2;
    if (!CreateSwapChain()) return 3;
    if (!LoadShaders()) return 4;
    if (!CreateResources()) return 5;
    Log("resources ready, rendering %u frames", opt_.frames);
    MSG msg;
    for (unsigned frame = 1; frame <= opt_.frames; frame++) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return 0;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Render(frame);
        if (frame == opt_.frames && !opt_.shot.empty()) {
            std::string error;
            if (CaptureBackBuffer(swapChain_, opt_.shot, error)) Log("screenshot saved");
            else Log("screenshot failed: %s", error.c_str());
        }
        HRESULT hr = swapChain_->Present(1, 0);
        if (FAILED(hr)) {
            Log("FAIL: Present 0x%08lx", hr);
            return 6;
        }
    }
    Log("OK: rendered %u frames", opt_.frames);
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring logPath = exe;
    logPath = logPath.substr(0, logPath.find_last_of(L'\\') + 1) + L"dx11test.log";
    g_log = _wfopen(logPath.c_str(), L"w");
    if (!g_log) g_log = stderr;

    Options o;
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"--frames" && i + 1 < argc) o.frames = wcstoul(argv[++i], nullptr, 10);
        else if (a == L"--shot" && i + 1 < argc) o.shot = argv[++i];
        else if (a == L"--tess") o.tess = true;
        else if (a == L"--compute") o.compute = true;
        else if (a == L"--depthread") o.depthRead = true;
        else if (a == L"--swapchain" && i + 1 < argc) {
            std::wstring s = argv[++i];
            o.swapchain = std::string(s.begin(), s.end());
        }
    }
    App app(o);
    int code = app.Run();
    Log("exit code %d", code);
    fclose(g_log);
    return code;
}
