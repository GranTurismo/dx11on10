// Saves a swap chain's back buffer as a 24-bit BMP (used by the proxy and by dx11test).
#pragma once
#include <dxgi.h>

#include <string>

bool CaptureBackBuffer(IDXGISwapChain* swapChain, const std::wstring& path, std::string& error);
