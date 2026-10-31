#!/bin/bash
# Runs dx11test in the VM's desktop session twice - without the proxy (reference, real
# DX11) and with it (feature level 10_x) - then fetches logs and screenshots.
#   tools/vmtest.sh <name> [arch] [dx11test args...]
# Results land in build/vmtest/<name>/.
set -e
cd "$(dirname "$0")/.."
name=$1
arch=${2:-x64}
shift 2 || shift $#
args="$*"
out=build/vmtest/$name
mkdir -p "$out"
VM="tools/vm.sh"

cat > "$out/run.cmd" <<EOF
@echo off
cd /d C:\\dx11on10
del /q done.flag 2>nul
rmdir /s /q ref proxy 2>nul
mkdir ref proxy
copy /y dx11test.exe ref\\ >nul
copy /y scene.hlsl ref\\ >nul
copy /y dx11test.exe proxy\\ >nul
copy /y scene.hlsl proxy\\ >nul
copy /y stage\\d3d11.dll proxy\\ >nul
copy /y stage\\test.ini proxy\\dx11on10.ini >nul
start "" /wait ref\\dx11test.exe --frames 120 --shot C:\\dx11on10\\ref\\shot.bmp $args
start "" /wait proxy\\dx11test.exe --frames 120 --shot C:\\dx11on10\\proxy\\shot.bmp $args
echo done > done.flag
EOF
sed -i '' 's/$/\r/' "$out/run.cmd"

cat > "$out/test.ini" <<EOF
[dx11on10]
MaxFeatureLevel=${LEVEL:-10_1}
DebugLayer=1
DumpShaders=1
Screenshots=120
EOF

$VM put build/$arch/dx11test.exe
$VM ssh "if not exist C:\\dx11on10\\stage mkdir C:\\dx11on10\\stage"
$VM put build/$arch/d3d11.dll C:/dx11on10/stage/
$VM put tests/shaders/scene.hlsl
$VM put "$out/test.ini" C:/dx11on10/stage/
$VM ssh "del /q C:\\dx11on10\\done.flag 2>nul & exit 0"
$VM gui "$out/run.cmd" >/dev/null
# Wait on the Windows side (up to ~3 minutes) for the run to finish.
$VM ssh "for /l %i in (1,1,90) do @(if exist C:\\dx11on10\\done.flag (exit 0) else (ping -n 3 127.0.0.1 >nul))" || true
for f in ref/dx11test.log ref/shot.bmp proxy/dx11test.log proxy/shot.bmp proxy/dx11on10.log proxy/dx11on10_frame120.bmp; do
    $VM get "C:/dx11on10/$f" "$out/$(echo $f | tr / _)" 2>/dev/null || echo "missing: $f"
done
echo "results in $out"
