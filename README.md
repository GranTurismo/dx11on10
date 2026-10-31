# dx11on10

Runs DirectX 11 games on DirectX 10 graphics cards (GeForce 8/9/200, Radeon HD 2000–4000, Intel HD 2000/3000).

A proxy `d3d11.dll` placed next to the game creates the real device at feature level 10_0/10_1, tells
DX11-only games they got 11_0, converts Shader Model 5 bytecode to Shader Model 4, replays compute
shaders as pixel or geometry shader draws, and quietly skips what the hardware can't do (tessellation,
atomics) instead of crashing.

## Use: the app

`build/app/DX11on10.exe` is a single portable program (32-bit, Windows 7 SP1 or later) that carries both
`d3d11.dll` builds. It lists installed Steam, GOG and Epic games (and any `.exe` you add or drop on it),
picks the game's real `.exe`, shows which DirectX version it uses, and turns DX11on10 on or off per game:

- **On** puts `d3d11.dll` and `dx11on10.ini` next to the game's `.exe`. Another program's `d3d11.dll`
  (ReShade, DXVK, …) is renamed to `d3d11.dll.dx11on10-backup` first.
- **Off** removes them and puts the backup back. A failed step undoes the earlier ones.
- Folders that need administrator rights are handled by an elevated copy of the app (`--ops`), only then.
- Warnings: anti-cheat (Easy Anti-Cheat, BattlEye, …: needs an explicit "I accept the risk"), games that
  don't use DirectX 11, another `d3d11.dll` or `dxgi.dll` mod, a running game, a graphics card that already
  has DirectX 11 (testing only) or only DirectX 9, and problems from the game's last `dx11on10.log`
  (crash, no picture, skipped effects).

Per-game settings live in `%APPDATA%\DX11on10\settings.ini`. For testing: `DX11on10.exe --report out.txt`,
`--screenshot out.png [--theme dark] [--select name] [--gpu 10_1]`, `--toggle <name> out.txt`, `--add <exe>`.

## Use: by hand

1. Copy `d3d11.dll` (64-bit or 32-bit, matching the game) and `dx11on10.ini` next to the game's `.exe`.
2. Start the game. `dx11on10.log` next to the exe shows what was converted or skipped.

Steam's "verify files" leaves the DLL alone (it isn't one of the game's files). Don't use it with games that
have anti-cheat.

## Build (macOS or Linux, MinGW)

    brew install mingw-w64
    ./build.sh            # build/x64, build/x86 and build/app/DX11on10.exe

## Test

- `tools/vmtest.sh <name> [x64|x86] [--tess] [--compute] [--swapchain flip]` runs `dx11test` in the Windows
  VM natively and through the proxy, and fetches both screenshots; `tools/compare.py` diffs them.
- `shadertool roundtrip scene.hlsl <entry> <target> <10_0|10_1>` compiles, converts and validates one shader
  on a real D3D11 runtime (WARP + debug layer).

## Layout

- `src/dxbc/`: DXBC container, checksum, SM5 → SM4 converter (portable C++)
- `src/proxy/`: the proxy DLL (export forwarding, vtable hooks, fallbacks, compute replay)
- `app/`: the DX11on10 app (Direct2D UI, game scanning, install/uninstall); `tools/make_icon.swift` draws its icon
- `tests/dx11test/`: a small DX11-only "game" used as the reference workload
- `third_party/microsoft/`: Microsoft's tokenized shader format header (NCSA license)
