#!/bin/bash
# Builds the proxy d3d11.dll and the test tools for 64-bit and 32-bit Windows (MinGW).
#   ./build.sh            everything
#   ./build.sh x64        one architecture
#   ./build.sh app        only the app (DX11on10.exe, carrying both d3d11.dll builds)
set -e
cd "$(dirname "$0")"

CXXFLAGS="-std=c++20 -O2 -Wall -Wno-unknown-pragmas -Wno-cast-function-type"
ARCHS=${1:-"x64 x86 app"}

build_app() {
    # 32-bit, so the one .exe runs on 32-bit and 64-bit Windows.
    local T=i686-w64-mingw32 out=build/app
    mkdir -p $out
    $T-windres -I. app/app.rc -o $out/app_res.o
    $T-g++ $CXXFLAGS -municode -mwindows -static -s -Wl,--large-address-aware -o $out/DX11on10.exe \
        app/main.cpp app/ui.cpp app/scan.cpp app/install.cpp app/system.cpp $out/app_res.o \
        -ld2d1 -ldwrite -lwindowscodecs -lole32 -loleaut32 -lshell32 -lshlwapi -lcomctl32 -ldwmapi -luuid -lversion
    echo "built $out/DX11on10.exe"
}

for arch in $ARCHS; do
    if [ "$arch" = app ]; then build_app; continue; fi
    case $arch in
        x64) T=x86_64-w64-mingw32 ;;
        x86) T=i686-w64-mingw32 ;;
        *) echo "unknown arch $arch" >&2; exit 1 ;;
    esac
    out=build/$arch
    gen=$out/gen
    mkdir -p "$gen"
    python3 tools/gen_exports.py tools/d3d11_exports.txt $arch $gen/stubs.S $gen/d3d11.def $gen/exports.inc
    $T-gcc -std=c11 -O2 -c src/proxy/slots.c -o $gen/slots.o
    $T-gcc -c $gen/stubs.S -o $gen/stubs.o
    $T-windres src/proxy/version.rc -o $gen/version.o

    # The proxy must not import d3d11.dll itself, so no -ld3d11 here.
    $T-g++ $CXXFLAGS -I$gen -shared -static -o $out/d3d11.dll \
        src/proxy/main.cpp src/proxy/log.cpp src/proxy/hooks.cpp src/proxy/device.cpp \
        src/proxy/context.cpp src/proxy/present.cpp src/proxy/crash.cpp src/proxy/depth.cpp src/proxy/compute.cpp src/common/capture.cpp \
        src/dxbc/dxbc.cpp src/dxbc/convert.cpp src/dxbc/compute.cpp \
        $gen/slots.o $gen/stubs.o $gen/version.o $gen/d3d11.def

    $T-g++ $CXXFLAGS -municode -mwindows -static -o $out/dx11test.exe \
        tests/dx11test/main.cpp src/common/capture.cpp -ld3d11 -ldxgi
    $T-g++ $CXXFLAGS -municode -mwindows -static -o $out/computetest.exe tests/computetest/main.cpp -ld3d11
    $T-g++ $CXXFLAGS -static -o $out/shadertool.exe \
        tests/shadertool/main.cpp src/dxbc/dxbc.cpp src/dxbc/convert.cpp src/dxbc/compute.cpp -ld3d11
    cp tests/shaders/scene.hlsl dist/dx11on10.ini $out/ 2>/dev/null || cp tests/shaders/scene.hlsl $out/
    echo "built $out"
done
