// computetest: runs the compute shaders in compute.hlsl the normal DX11 way and writes
// every output byte to computetest.bin (and a hash per test to computetest.log). Run it
// natively and through the proxy; the outputs must match.
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace {

FILE* g_log;
FILE* g_out;
ID3D11Device* g_device;
ID3D11DeviceContext* g_context;
std::string g_source;

typedef HRESULT(WINAPI* CompileFn)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT,
                                   UINT, ID3DBlob**, ID3DBlob**);
CompileFn g_compile;

ID3D11ComputeShader* Shader(const char* entry) {
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = g_compile(g_source.data(), g_source.size(), "compute.hlsl", nullptr, nullptr, entry, "cs_5_0",
                           D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (errors) {
        fprintf(g_log, "%s\n", (const char*)errors->GetBufferPointer());
        errors->Release();
    }
    if (FAILED(hr)) return nullptr;
    ID3D11ComputeShader* cs = nullptr;
    hr = g_device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs);
    code->Release();
    if (FAILED(hr)) fprintf(g_log, "CreateComputeShader(%s): 0x%08lx\n", entry, hr);
    return cs;
}

// Copies a resource to the CPU and records its bytes.
void Record(const char* name, ID3D11Resource* resource) {
    D3D11_RESOURCE_DIMENSION dim;
    resource->GetType(&dim);
    ID3D11Resource* staging = nullptr;
    UINT rows = 1, rowBytes = 0, slices = 1, depthPitch = 0;
    if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
        D3D11_BUFFER_DESC d;
        static_cast<ID3D11Buffer*>(resource)->GetDesc(&d);
        rowBytes = d.ByteWidth;
        d.Usage = D3D11_USAGE_STAGING, d.BindFlags = 0, d.MiscFlags = 0, d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        g_device->CreateBuffer(&d, nullptr, reinterpret_cast<ID3D11Buffer**>(&staging));
    } else {
        D3D11_TEXTURE2D_DESC d;
        static_cast<ID3D11Texture2D*>(resource)->GetDesc(&d);
        UINT bpp = d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4;
        rowBytes = d.Width * bpp, rows = d.Height, slices = d.ArraySize;
        d.Usage = D3D11_USAGE_STAGING, d.BindFlags = 0, d.MiscFlags = 0, d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        g_device->CreateTexture2D(&d, nullptr, reinterpret_cast<ID3D11Texture2D**>(&staging));
    }
    if (!staging) {
        fprintf(g_log, "test %s: no staging copy\n", name);
        return;
    }
    g_context->CopyResource(staging, resource);
    uint32_t hash = 2166136261u;
    size_t total = 0;
    for (UINT s = 0; s < slices; s++) {
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(g_context->Map(staging, s, D3D11_MAP_READ, 0, &m))) continue;
        for (UINT y = 0; y < rows; y++) {
            const uint8_t* row = static_cast<const uint8_t*>(m.pData) + size_t(m.RowPitch) * y;
            fwrite(row, 1, rowBytes, g_out);
            for (UINT i = 0; i < rowBytes; i++) hash = (hash ^ row[i]) * 16777619u;
            total += rowBytes;
        }
        g_context->Unmap(staging, s);
    }
    (void)depthPitch;
    fprintf(g_log, "test %s: hash %08x, %zu bytes\n", name, hash, total);
    fflush(g_log);
    staging->Release();
}

ID3D11Texture2D* Texture(UINT w, UINT h, UINT slices, DXGI_FORMAT format, const void* data, UINT pitch) {
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w, d.Height = h, d.MipLevels = 1, d.ArraySize = slices, d.Format = format, d.SampleDesc = {1, 0};
    d.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    std::vector<D3D11_SUBRESOURCE_DATA> init(slices, {data, pitch, 0});
    ID3D11Texture2D* t = nullptr;
    HRESULT hr = g_device->CreateTexture2D(&d, data ? init.data() : nullptr, &t);
    if (FAILED(hr)) fprintf(g_log, "CreateTexture2D(format %d): 0x%08lx\n", format, hr);
    return t;
}

void Run(const char* name, const char* entry, ID3D11Resource* output, ID3D11UnorderedAccessView* uav, UINT x, UINT y,
         UINT z) {
    ID3D11ComputeShader* cs = Shader(entry);
    if (!cs || !uav) {
        fprintf(g_log, "test %s: setup failed\n", name);
        return;
    }
    g_context->CSSetShader(cs, nullptr, 0);
    g_context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    g_context->Dispatch(x, y, z);
    ID3D11InfoQueue* queue = nullptr;  // present when the debug layer is on
    if (SUCCEEDED(g_device->QueryInterface(__uuidof(ID3D11InfoQueue), (void**)&queue))) {
        for (UINT64 i = 0; i < queue->GetNumStoredMessages() && i < 8; i++) {
            SIZE_T size = 0;
            queue->GetMessage(i, nullptr, &size);
            std::vector<char> buf(size);
            D3D11_MESSAGE* m = reinterpret_cast<D3D11_MESSAGE*>(buf.data());
            if (SUCCEEDED(queue->GetMessage(i, m, &size))) fprintf(g_log, "  debug: %.*s\n", int(m->DescriptionByteLength), m->pDescription);
        }
        queue->ClearStoredMessages();
        queue->Release();
    }
    ID3D11UnorderedAccessView* none = nullptr;
    g_context->CSSetUnorderedAccessViews(0, 1, &none, nullptr);
    Record(name, output);
    cs->Release();
}

ID3D11UnorderedAccessView* Uav(ID3D11Resource* r) {
    ID3D11UnorderedAccessView* v = nullptr;
    HRESULT hr = g_device->CreateUnorderedAccessView(r, nullptr, &v);
    if (FAILED(hr)) fprintf(g_log, "CreateUnorderedAccessView: 0x%08lx\n", hr);
    return v;
}

int Main() {
    D3D_FEATURE_LEVEL wanted = D3D_FEATURE_LEVEL_11_0, got;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &wanted, 1, D3D11_SDK_VERSION,
                                   &g_device, &got, &g_context);
    if (FAILED(hr)) {
        fprintf(g_log, "FAIL: D3D11CreateDevice 0x%08lx\n", hr);
        return 2;
    }
    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    g_compile = compiler ? (CompileFn)GetProcAddress(compiler, "D3DCompile") : nullptr;
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    wcscpy(wcsrchr(path, L'\\') + 1, L"compute.hlsl");
    FILE* f = _wfopen(path, L"rb");
    if (!f || !g_compile) {
        fprintf(g_log, "FAIL: compute.hlsl or d3dcompiler_47.dll missing\n");
        return 3;
    }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) g_source.append(buf, n);
    fclose(f);

    // Shared inputs: a 64x64 pattern, a sampler and the parameter buffer.
    std::vector<uint32_t> pattern(64 * 64);
    for (UINT y = 0; y < 64; y++)
        for (UINT x = 0; x < 64; x++)
            pattern[y * 64 + x] = 0xff000000u | ((x * 4) << 16) | ((y * 4) << 8) | (((x ^ y) & 8) ? 0xc0 : 0x30);
    ID3D11Texture2D* source = Texture(64, 64, 1, DXGI_FORMAT_R8G8B8A8_UNORM, pattern.data(), 64 * 4);
    ID3D11ShaderResourceView* sourceView = nullptr;
    g_device->CreateShaderResourceView(source, nullptr, &sourceView);
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11SamplerState* sampler = nullptr;
    g_device->CreateSamplerState(&sd, &sampler);
    uint32_t params[12] = {4, 2, 40, 40, 8, 16, 0, 0};
    float scale[4] = {1.5f, 0.75f, 0, 0};
    memcpy(&params[8], scale, sizeof(scale));
    D3D11_BUFFER_DESC bd = {sizeof(params), D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    D3D11_SUBRESOURCE_DATA init = {params, 0, 0};
    ID3D11Buffer* constants = nullptr;
    g_device->CreateBuffer(&bd, &init, &constants);
    g_context->CSSetShaderResources(0, 1, &sourceView);
    g_context->CSSetSamplers(0, 1, &sampler);
    g_context->CSSetConstantBuffers(0, 1, &constants);

    std::vector<uint8_t> zeros(128 * 64 * 8, 0);
    struct Case {
        const char* name;
        const char* entry;
        UINT w, h, slices;
        DXGI_FORMAT format;
        UINT x, y, z;
    };
    const Case cases[] = {
        {"copy", "cs_copy", 64, 64, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 8, 8, 1},
        {"math", "cs_math", 64, 64, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, 8, 1},
        {"two_stores", "cs_two_stores", 128, 64, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 8, 16, 1},
        {"conditional", "cs_conditional", 64, 64, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 4, 4, 1},
        {"uint", "cs_uint", 64, 64, 1, DXGI_FORMAT_R32_UINT, 8, 8, 1},
        {"array", "cs_array", 32, 32, 4, DXGI_FORMAT_R8G8B8A8_UNORM, 4, 4, 4},
        {"blur", "cs_blur", 64, 64, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, 64, 1},
        {"size", "cs_size", 48, 40, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, 6, 5, 1},
        {"blur_vertical", "cs_blur_vertical", 64, 64, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, 64, 2, 1},
    };
    for (const Case& c : cases) {
        UINT bpp = c.format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4;
        ID3D11Texture2D* out = Texture(c.w, c.h, c.slices, c.format, zeros.data(), c.w * bpp);
        ID3D11UnorderedAccessView* uav = out ? Uav(out) : nullptr;
        Run(c.name, c.entry, out, uav, c.x, c.y, c.z);
        if (uav) uav->Release();
        if (out) out->Release();
    }

    // Read-modify-write starts from known values.
    std::vector<float> start(64 * 64);
    for (size_t i = 0; i < start.size(); i++) start[i] = float(i % 97) * 0.5f;
    ID3D11Texture2D* rmw = Texture(64, 64, 1, DXGI_FORMAT_R32_FLOAT, start.data(), 64 * 4);
    ID3D11UnorderedAccessView* rmwUav = rmw ? Uav(rmw) : nullptr;
    Run("rmw", "cs_rmw", rmw, rmwUav, 8, 8, 1);

    D3D11_BUFFER_DESC ob = {256 * 16, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, 0, 0, 0};
    ID3D11Buffer* buffer = nullptr;
    hr = g_device->CreateBuffer(&ob, nullptr, &buffer);
    if (FAILED(hr)) fprintf(g_log, "CreateBuffer(UAV): 0x%08lx\n", hr);
    ID3D11UnorderedAccessView* bufferUav = nullptr;
    if (buffer) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = 256;
        hr = g_device->CreateUnorderedAccessView(buffer, &ud, &bufferUav);
        if (FAILED(hr)) fprintf(g_log, "CreateUnorderedAccessView(buffer): 0x%08lx\n", hr);
    }
    Run("buffer", "cs_buffer", buffer, bufferUav, 4, 1, 1);
    fprintf(g_log, "done\n");
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    dir = dir.substr(0, dir.find_last_of(L'\\') + 1);
    g_log = _wfopen((dir + L"computetest.log").c_str(), L"w");
    g_out = _wfopen((dir + L"computetest.bin").c_str(), L"wb");
    int code = Main();
    fclose(g_out);
    fclose(g_log);
    return code;
}
