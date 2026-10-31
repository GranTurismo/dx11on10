#include "capture.h"

#include <d3d11.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

float HalfToFloat(uint16_t h) {
    uint32_t sign = (h >> 15) & 1, exponent = (h >> 10) & 0x1f, mantissa = h & 0x3ff;
    float value;
    if (exponent == 0) value = std::ldexp(float(mantissa), -24);
    else if (exponent == 31) value = mantissa ? NAN : INFINITY;
    else value = std::ldexp(float(mantissa | 0x400), int(exponent) - 25);
    return sign ? -value : value;
}

uint8_t ToByte(float v) {
    if (!(v > 0)) return 0;
    if (v >= 1) return 255;
    return uint8_t(v * 255.0f + 0.5f);
}

// Converts one pixel to B, G, R. Returns false for formats we don't read.
bool ReadPixel(DXGI_FORMAT format, const uint8_t* p, uint8_t* bgr) {
    switch (format) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            bgr[0] = p[2], bgr[1] = p[1], bgr[2] = p[0];
            return true;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
            bgr[0] = p[0], bgr[1] = p[1], bgr[2] = p[2];
            return true;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        case DXGI_FORMAT_R10G10B10A2_UNORM: {
            uint32_t v = p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24;
            bgr[2] = uint8_t((v & 0x3ff) >> 2), bgr[1] = uint8_t(((v >> 10) & 0x3ff) >> 2);
            bgr[0] = uint8_t(((v >> 20) & 0x3ff) >> 2);
            return true;
        }
        case DXGI_FORMAT_R16G16B16A16_FLOAT: {
            const uint16_t* h = reinterpret_cast<const uint16_t*>(p);
            bgr[0] = ToByte(HalfToFloat(h[2])), bgr[1] = ToByte(HalfToFloat(h[1])), bgr[2] = ToByte(HalfToFloat(h[0]));
            return true;
        }
        default: return false;
    }
}

UINT BytesPerPixel(DXGI_FORMAT format) { return format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4; }

DXGI_FORMAT Typed(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        default: return format;
    }
}

template <class T>
void SafeRelease(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

}  // namespace

bool CaptureBackBuffer(IDXGISwapChain* swapChain, const std::wstring& path, std::string& error) {
    ID3D11Texture2D* back = nullptr;
    ID3D11Texture2D* resolved = nullptr;
    ID3D11Texture2D* staging = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    bool ok = false;
    do {
        if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back))) {
            error = "GetBuffer failed";
            break;
        }
        back->GetDevice(&device);
        device->GetImmediateContext(&context);
        D3D11_TEXTURE2D_DESC desc;
        back->GetDesc(&desc);
        ID3D11Texture2D* source = back;
        if (desc.SampleDesc.Count > 1) {
            D3D11_TEXTURE2D_DESC r = desc;
            r.SampleDesc = {1, 0};
            r.BindFlags = 0;
            r.MiscFlags = 0;
            r.Format = Typed(desc.Format);
            if (FAILED(device->CreateTexture2D(&r, nullptr, &resolved))) {
                error = "resolve texture creation failed";
                break;
            }
            context->ResolveSubresource(resolved, 0, back, 0, r.Format);
            source = resolved;
        }
        D3D11_TEXTURE2D_DESC s = desc;
        s.SampleDesc = {1, 0};
        s.Usage = D3D11_USAGE_STAGING;
        s.BindFlags = 0;
        s.MiscFlags = 0;
        s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        s.MipLevels = 1;
        s.ArraySize = 1;
        if (FAILED(device->CreateTexture2D(&s, nullptr, &staging))) {
            error = "staging texture creation failed";
            break;
        }
        context->CopySubresourceRegion(staging, 0, 0, 0, 0, source, 0, nullptr);
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (FAILED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
            error = "Map failed";
            break;
        }
        UINT w = desc.Width, h = desc.Height;
        UINT rowBytes = (w * 3 + 3) & ~3u;
        std::vector<uint8_t> pixels(size_t(rowBytes) * h);
        bool known = true;
        for (UINT y = 0; y < h && known; y++) {
            const uint8_t* row = static_cast<const uint8_t*>(mapped.pData) + size_t(mapped.RowPitch) * y;
            uint8_t* dst = &pixels[size_t(rowBytes) * (h - 1 - y)];
            for (UINT x = 0; x < w && known; x++) known = ReadPixel(desc.Format, row + x * BytesPerPixel(desc.Format), dst + 3 * x);
        }
        context->Unmap(staging, 0);
        if (!known) {
            error = "unsupported back buffer format " + std::to_string(desc.Format);
            break;
        }
        FILE* f = _wfopen(path.c_str(), L"wb");
        if (!f) {
            error = "cannot write file";
            break;
        }
        uint32_t fileSize = 54 + uint32_t(pixels.size());
        uint8_t header[54] = {'B', 'M'};
        auto put32 = [&](int at, uint32_t v) { memcpy(header + at, &v, 4); };
        put32(2, fileSize);
        put32(10, 54);
        put32(14, 40);
        put32(18, w);
        put32(22, h);
        header[26] = 1;
        header[28] = 24;
        put32(34, uint32_t(pixels.size()));
        fwrite(header, 1, sizeof(header), f);
        fwrite(pixels.data(), 1, pixels.size(), f);
        fclose(f);
        ok = true;
    } while (false);
    SafeRelease(staging);
    SafeRelease(resolved);
    SafeRelease(context);
    SafeRelease(device);
    SafeRelease(back);
    return ok;
}
