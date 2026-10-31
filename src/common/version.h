// Version shared by the proxy DLL and the app. The app compares a game's d3d11.dll with
// the copy it carries byte for byte, so this only needs to change for releases.
#pragma once
#define DX11ON10_VERSION "0.1.0"
#define DX11ON10_VERSION_W L"0.1.0"
#define DX11ON10_VERSION_BIN 0, 1, 0, 0
