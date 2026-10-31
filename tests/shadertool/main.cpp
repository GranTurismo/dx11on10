// shadertool: compile / convert / disassemble / validate shaders on a real D3D11 runtime.
//   shadertool roundtrip <file.hlsl> <entry> <target> <10_0|10_1>   compile, convert, disassemble, create on WARP
//   shadertool compile <file.hlsl> <entry> <target> <out.dxbc>
//   shadertool convert <in.dxbc> <out.dxbc> <10_0|10_1>
//   shadertool disasm <in.dxbc>
//   shadertool check <in.dxbc> <10_0|10_1|11_0>
//   shadertool compare <file.hlsl> <ps-entry>    native SM5 at 11_0 vs converted at 10_0, bit for bit
//   shadertool computegs <file.hlsl> <cs-entry>  compute shader -> geometry shaders, validated at 10_0
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "../../src/dxbc/compute.h"
#include "../../src/dxbc/convert.h"
#include "../../src/proxy/emulation_shaders.h"
#include "../../src/dxbc/dxbc.h"

typedef HRESULT(WINAPI* CompileFn)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT,
                                   UINT, ID3DBlob**, ID3DBlob**);
typedef HRESULT(WINAPI* DisassembleFn)(LPCVOID, SIZE_T, UINT, LPCSTR, ID3DBlob**);

static CompileFn g_compile;
static DisassembleFn g_disassemble;

static bool LoadCompiler() {
    HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!m) return false;
    g_compile = (CompileFn)GetProcAddress(m, "D3DCompile");
    g_disassemble = (DisassembleFn)GetProcAddress(m, "D3DDisassemble");
    return g_compile && g_disassemble;
}

static bool ReadFile(const char* path, std::vector<uint8_t>& out) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    out.resize(ftell(f));
    fseek(f, 0, SEEK_SET);
    size_t n = fread(out.data(), 1, out.size(), f);
    fclose(f);
    return n == out.size();
}

static bool WriteFile(const char* path, const std::vector<uint8_t>& data) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    fwrite(data.data(), 1, data.size(), f);
    fclose(f);
    return true;
}

static bool Compile(const char* path, const char* entry, const char* target, std::vector<uint8_t>& out) {
    std::vector<uint8_t> src;
    if (!ReadFile(path, src)) {
        printf("cannot read %s\n", path);
        return false;
    }
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = g_compile(src.data(), src.size(), path, nullptr, nullptr, entry, target,
                           D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (errors) {
        printf("%s", (const char*)errors->GetBufferPointer());
        errors->Release();
    }
    if (FAILED(hr)) {
        printf("compile failed: 0x%08lx\n", hr);
        return false;
    }
    const uint8_t* p = (const uint8_t*)code->GetBufferPointer();
    out.assign(p, p + code->GetBufferSize());
    code->Release();
    return true;
}

static void Disassemble(const std::vector<uint8_t>& blob) {
    ID3DBlob* text = nullptr;
    HRESULT hr = g_disassemble(blob.data(), blob.size(), 0, nullptr, &text);
    if (FAILED(hr)) {
        printf("disassembly failed: 0x%08lx\n", hr);
        return;
    }
    printf("%s\n", (const char*)text->GetBufferPointer());
    text->Release();
}

static D3D_FEATURE_LEVEL ParseLevel(const char* s) {
    if (!strcmp(s, "10_0")) return D3D_FEATURE_LEVEL_10_0;
    if (!strcmp(s, "10_1")) return D3D_FEATURE_LEVEL_10_1;
    return D3D_FEATURE_LEVEL_11_0;
}

static void PrintMessages(ID3D11Device* device) {
    ID3D11InfoQueue* q = nullptr;
    if (FAILED(device->QueryInterface(__uuidof(ID3D11InfoQueue), (void**)&q))) return;
    UINT64 n = q->GetNumStoredMessages();
    for (UINT64 i = 0; i < n; i++) {
        SIZE_T size = 0;
        q->GetMessage(i, nullptr, &size);
        std::vector<char> buf(size);
        D3D11_MESSAGE* m = (D3D11_MESSAGE*)buf.data();
        if (SUCCEEDED(q->GetMessage(i, m, &size))) printf("  debug: %.*s\n", (int)m->DescriptionByteLength, m->pDescription);
    }
    q->ClearStoredMessages();
    q->Release();
}

// Creates the shader on a WARP device at the given feature level. Returns true on success.
static bool Check(const std::vector<uint8_t>& blob, D3D_FEATURE_LEVEL level) {
    ID3D11Device* device = nullptr;
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_DEBUG, &level, 1,
                                   D3D11_SDK_VERSION, &device, &got, nullptr);
    if (FAILED(hr)) {
        printf("  (no debug layer: 0x%08lx)\n", hr);
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, &got,
                               nullptr);
    }
    if (FAILED(hr)) {
        printf("  device creation failed: 0x%08lx\n", hr);
        return false;
    }
    uint32_t version = 0;
    dxbc::Container c;
    std::string err;
    if (dxbc::Parse(blob.data(), blob.size(), c, err)) {
        const dxbc::Chunk* code = c.Find(dxbc::kSHDR);
        if (!code) code = c.Find(dxbc::kSHEX);
        if (code && code->data.size() >= 4) memcpy(&version, code->data.data(), 4);
    }
    IUnknown* shader = nullptr;
    switch (version >> 16) {
        case 0: hr = device->CreatePixelShader(blob.data(), blob.size(), nullptr, (ID3D11PixelShader**)&shader); break;
        case 1: hr = device->CreateVertexShader(blob.data(), blob.size(), nullptr, (ID3D11VertexShader**)&shader); break;
        case 2: hr = device->CreateGeometryShader(blob.data(), blob.size(), nullptr, (ID3D11GeometryShader**)&shader); break;
        case 3: hr = device->CreateHullShader(blob.data(), blob.size(), nullptr, (ID3D11HullShader**)&shader); break;
        case 4: hr = device->CreateDomainShader(blob.data(), blob.size(), nullptr, (ID3D11DomainShader**)&shader); break;
        case 5: hr = device->CreateComputeShader(blob.data(), blob.size(), nullptr, (ID3D11ComputeShader**)&shader); break;
        default: hr = E_INVALIDARG; break;
    }
    printf("  create at FL %x.%x: %s (0x%08lx)\n", level >> 12, (level >> 8) & 0xf, SUCCEEDED(hr) ? "OK" : "FAILED", hr);
    PrintMessages(device);
    if (shader) shader->Release();
    device->Release();
    return SUCCEEDED(hr);
}

// Draws a full-screen triangle into a 64x64 R32G32B32A32_UINT target on WARP and reads it back.
static bool RenderPixels(D3D_FEATURE_LEVEL level, const std::vector<uint8_t>& vs, const std::vector<uint8_t>& ps,
                         std::vector<uint32_t>& pixels) {
    const UINT size = 64;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_DEBUG, &level, 1,
                                 D3D11_SDK_VERSION, &device, nullptr, &context))) {
        printf("  WARP device at %x failed\n", level);
        return false;
    }
    bool ok = false;
    ID3D11Texture2D* target = nullptr;
    ID3D11Texture2D* staging = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11Buffer* constants = nullptr;
    ID3D11VertexShader* vertex = nullptr;
    ID3D11PixelShader* pixel = nullptr;
    do {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = td.Height = size;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R32G32B32A32_UINT;
        td.SampleDesc = {1, 0};
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(device->CreateTexture2D(&td, nullptr, &target))) break;
        td.BindFlags = 0;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device->CreateTexture2D(&td, nullptr, &staging))) break;
        if (FAILED(device->CreateRenderTargetView(target, nullptr, &rtv))) break;
        uint32_t data[64];
        uint32_t x = 12345;
        for (uint32_t& d : data) {
            x = x * 1664525u + 1013904223u;
            d = x ^ (x >> 13);
        }
        D3D11_BUFFER_DESC bd = {sizeof(data), D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        D3D11_SUBRESOURCE_DATA init = {data, 0, 0};
        if (FAILED(device->CreateBuffer(&bd, &init, &constants))) break;
        HRESULT hr = device->CreateVertexShader(vs.data(), vs.size(), nullptr, &vertex);
        if (SUCCEEDED(hr)) hr = device->CreatePixelShader(ps.data(), ps.size(), nullptr, &pixel);
        if (FAILED(hr)) {
            printf("  shader creation at %x failed: 0x%08lx\n", level, hr);
            break;
        }
        D3D11_VIEWPORT vp = {0, 0, float(size), float(size), 0, 1};
        context->RSSetViewports(1, &vp);
        context->OMSetRenderTargets(1, &rtv, nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vertex, nullptr, 0);
        context->PSSetShader(pixel, nullptr, 0);
        context->PSSetConstantBuffers(0, 1, &constants);
        context->Draw(3, 0);
        context->CopyResource(staging, target);
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (FAILED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) break;
        pixels.resize(size * size * 4);
        for (UINT y = 0; y < size; y++)
            memcpy(&pixels[y * size * 4], (const uint8_t*)mapped.pData + y * mapped.RowPitch, size * 16);
        context->Unmap(staging, 0);
        ok = true;
    } while (false);
    PrintMessages(device);
    if (pixel) pixel->Release();
    if (vertex) vertex->Release();
    if (constants) constants->Release();
    if (rtv) rtv->Release();
    if (staging) staging->Release();
    if (target) target->Release();
    context->Release();
    device->Release();
    return ok;
}

// Runs the SM5 shader natively at 11_0 and its conversion at 10_0, and compares every value.
static int Compare(const char* file, const char* entry) {
    std::vector<uint8_t> vs, ps;
    if (!Compile(file, "vs_fullscreen", "vs_4_0", vs) || !Compile(file, entry, "ps_5_0", ps)) return 1;
    dxbc::ConvertResult r = dxbc::ToSM4(ps.data(), ps.size(), 0);
    printf("%s: %s\n", entry, r.status == dxbc::ConvertResult::Converted ? r.message.c_str() : "NOT CONVERTED");
    if (r.status != dxbc::ConvertResult::Converted) return 2;
    std::vector<uint32_t> native, converted;
    if (!RenderPixels(D3D_FEATURE_LEVEL_11_0, vs, ps, native) ||
        !RenderPixels(D3D_FEATURE_LEVEL_10_0, vs, r.blob, converted))
        return 3;
    size_t mismatches = 0;
    for (size_t i = 0; i < native.size(); i++) {
        if (native[i] == converted[i]) continue;
        if (mismatches++ < 5)
            printf("  pixel %zu.%c: native 0x%08x, converted 0x%08x\n", i / 4, "xyzw"[i % 4], native[i], converted[i]);
    }
    printf("  %zu values compared, %zu mismatches\n", native.size(), mismatches);
    return mismatches ? 4 : 0;
}

// Rewrites a compute shader as geometry shaders and validates them at feature level 10_0.
static int ComputeToGS(const char* file, const char* entry) {
    std::vector<uint8_t> cs;
    if (!Compile(file, entry, "cs_5_0", cs)) return 1;
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
    dxbc::ComputeProgram p = dxbc::ComputeToGS(cs.data(), cs.size(), 0, signatures, psSignatures);
    printf("%s: %s%s\n", entry, p.ok ? "" : "NOT CONVERTED: ", p.message.c_str());
    if (!p.ok) return 2;
    bool ok = true;
    for (const dxbc::ComputePass& pass : p.passes) {
        printf("---- pass u%u (dimension %u, type %d, %u points)\n", pass.uavSlot, pass.dimension, int(pass.type),
               pass.maxPoints);
        if (getenv("SHOW")) Disassemble(pass.gs);
        ok &= Check(pass.gs, D3D_FEATURE_LEVEL_10_0);
        if (!pass.ps.empty()) {
            printf("     fast path pixel shader:\n");
            if (getenv("SHOW")) Disassemble(pass.ps);
            ok &= Check(pass.ps, D3D_FEATURE_LEVEL_10_0);
        }
    }
    return ok ? 0 : 3;
}

// Renders a compute shader's fast-path pixel shader on hardware or WARP (feature level
// 10_0) and prints what it wrote: for chasing driver differences.
static int PixelTest(const char* file, const char* entry, bool hardware, const char* variant) {
    std::vector<uint8_t> cs, vs;
    if (!Compile(file, entry, "cs_5_0", cs)) return 1;
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
    dxbc::ComputeProgram p = dxbc::ComputeToGS(cs.data(), cs.size(), 0, signatures, psSignatures);
    if (!p.ok || p.passes.empty() || p.passes[0].ps.empty()) {
        printf("no fast path: %s\n", p.message.c_str());
        return 2;
    }
    std::vector<uint8_t> ps = p.passes[0].ps;
    if (variant && !strcmp(variant, "fxc")) {
        // Control: a plain fxc-compiled shader with the same signature.
        if (!Compile(file, "ps_control", "ps_4_0", ps)) return 1;
    }
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_10_0;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    HRESULT hr = D3D11CreateDevice(nullptr, hardware ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP, nullptr, 0,
                                   &level, 1, D3D11_SDK_VERSION, &device, nullptr, &context);
    if (FAILED(hr)) {
        printf("device: 0x%08lx\n", hr);
        return 3;
    }
    ID3D11VertexShader* v = nullptr;
    ID3D11PixelShader* pixel = nullptr;
    hr = device->CreateVertexShader(k_vs_fullscreen, sizeof(k_vs_fullscreen), nullptr, &v);
    HRESULT hp = device->CreatePixelShader(ps.data(), ps.size(), nullptr, &pixel);
    printf("shaders: vs 0x%08lx ps 0x%08lx\n", hr, hp);
    D3D11_TEXTURE2D_DESC td = {64, 64, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT,
                               D3D11_BIND_RENDER_TARGET, 0, 0};
    ID3D11Texture2D* target = nullptr;
    device->CreateTexture2D(&td, nullptr, &target);
    ID3D11RenderTargetView* rtv = nullptr;
    device->CreateRenderTargetView(target, nullptr, &rtv);
    std::vector<uint32_t> pattern(64 * 64, 0xff8040c0);
    D3D11_TEXTURE2D_DESC sd = {64, 64, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_IMMUTABLE,
                               D3D11_BIND_SHADER_RESOURCE, 0, 0};
    D3D11_SUBRESOURCE_DATA init = {pattern.data(), 256, 0};
    ID3D11Texture2D* source = nullptr;
    device->CreateTexture2D(&sd, &init, &source);
    ID3D11ShaderResourceView* srv = nullptr;
    device->CreateShaderResourceView(source, nullptr, &srv);
    D3D11_SAMPLER_DESC samp = {};
    samp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    ID3D11SamplerState* sampler = nullptr;
    device->CreateSamplerState(&samp, &sampler);
    uint32_t params[12] = {4, 2, 40, 40, 8, 16, 0, 0};
    float scale[4] = {1.5f, 0.75f, 0, 0};
    memcpy(&params[8], scale, sizeof(scale));
    uint32_t dispatch[16] = {64 / p.threads[0], 64 / p.threads[1], 1, 0};
    ID3D11Buffer* cbs[14] = {};
    D3D11_BUFFER_DESC bd = {sizeof(params), D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    D3D11_SUBRESOURCE_DATA pi = {params, 0, 0};
    device->CreateBuffer(&bd, &pi, &cbs[0]);
    bd.ByteWidth = sizeof(dispatch);
    D3D11_SUBRESOURCE_DATA di = {dispatch, 0, 0};
    device->CreateBuffer(&bd, &di, &cbs[p.dispatchSlot]);
    D3D11_VIEWPORT vp = {0, 0, 64, 64, 0, 1};
    float clear[4] = {0, 0, 0, 0};
    context->ClearRenderTargetView(rtv, clear);
    context->OMSetRenderTargets(1, &rtv, nullptr);
    context->RSSetViewports(1, &vp);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(v, nullptr, 0);
    context->PSSetShader(pixel, nullptr, 0);
    context->PSSetShaderResources(0, 1, &srv);
    context->PSSetSamplers(0, 1, &sampler);
    context->PSSetConstantBuffers(0, variant && !strcmp(variant, "two") ? 2 : 14, cbs);
    context->Draw(3, 0);
    td.Usage = D3D11_USAGE_STAGING, td.BindFlags = 0, td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    device->CreateTexture2D(&td, nullptr, &staging);
    context->CopyResource(staging, target);
    D3D11_MAPPED_SUBRESOURCE m;
    context->Map(staging, 0, D3D11_MAP_READ, 0, &m);
    const uint16_t* px = static_cast<const uint16_t*>(m.pData);
    int written = 0;
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            const uint16_t* q = px + y * (m.RowPitch / 2) + x * 4;
            if (q[0] | q[1] | q[2] | q[3]) written++;
        }
    printf("%s, dispatch constants in cb%u: %d of 4096 pixels written; pixel (5,5) = %04x %04x %04x %04x\n",
           hardware ? "hardware" : "WARP", p.dispatchSlot, written, px[5 * (m.RowPitch / 2) + 20],
           px[5 * (m.RowPitch / 2) + 21], px[5 * (m.RowPitch / 2) + 22], px[5 * (m.RowPitch / 2) + 23]);
    context->Unmap(staging, 0);
    return 0;
}

static int Convert(const std::vector<uint8_t>& in, int minor, std::vector<uint8_t>& out) {
    dxbc::ConvertResult r = dxbc::ToSM4(in.data(), in.size(), minor);
    const char* status[] = {"converted", "already SM4", "UNSUPPORTED", "INVALID"};
    printf("convert %s %d.%d -> %s%s%s\n", dxbc::StageName(r.stage), r.major, r.minor, status[r.status],
           r.message.empty() ? "" : ": ", r.message.c_str());
    if (r.status == dxbc::ConvertResult::Converted) out = r.blob;
    if (r.status == dxbc::ConvertResult::AlreadySM4) out = in;
    return r.status;
}

int main(int argc, char** argv) {
    if (!LoadCompiler()) {
        printf("d3dcompiler_47.dll not available\n");
        return 1;
    }
    std::string cmd = argc > 1 ? argv[1] : "";
    std::vector<uint8_t> blob, out;
    if (cmd == "roundtrip" && argc == 6) {
        if (!Compile(argv[2], argv[3], argv[4], blob)) return 1;
        printf("compiled %s %s: %zu bytes, checksum %s\n", argv[3], argv[4], blob.size(),
               dxbc::ChecksumMatches(blob.data(), blob.size()) ? "reproduced" : "MISMATCH");
        D3D_FEATURE_LEVEL level = ParseLevel(argv[5]);
        int status = Convert(blob, level == D3D_FEATURE_LEVEL_10_1 ? 1 : 0, out);
        if (status > 1) return 2;
        printf("---- converted disassembly ----\n");
        Disassemble(out);
        return Check(out, level) ? 0 : 3;
    }
    if (cmd == "compile" && argc == 6) {
        if (!Compile(argv[2], argv[3], argv[4], blob)) return 1;
        return WriteFile(argv[5], blob) ? 0 : 1;
    }
    if (cmd == "convert" && argc == 5) {
        if (!ReadFile(argv[2], blob)) return 1;
        int status = Convert(blob, ParseLevel(argv[4]) == D3D_FEATURE_LEVEL_10_1 ? 1 : 0, out);
        if (status > 1) return 2;
        return WriteFile(argv[3], out) ? 0 : 1;
    }
    if (cmd == "disasm" && argc == 3) {
        if (!ReadFile(argv[2], blob)) return 1;
        printf("checksum %s\n", dxbc::ChecksumMatches(blob.data(), blob.size()) ? "valid" : "MISMATCH");
        Disassemble(blob);
        return 0;
    }
    if (cmd == "compare" && argc == 4) return Compare(argv[2], argv[3]);
    if (cmd == "computegs" && argc == 4) return ComputeToGS(argv[2], argv[3]);
    if (cmd == "pstest" && argc >= 5) return PixelTest(argv[2], argv[3], !strcmp(argv[4], "hw"), argc > 5 ? argv[5] : nullptr);
    if (cmd == "check" && argc == 4) {
        if (!ReadFile(argv[2], blob)) return 1;
        return Check(blob, ParseLevel(argv[3])) ? 0 : 3;
    }
    printf("usage: shadertool roundtrip|compile|convert|disasm|check ...\n");
    return 1;
}
