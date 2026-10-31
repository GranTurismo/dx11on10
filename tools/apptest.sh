#!/bin/bash
# Drives the DX11on10 app in the VM's desktop session with the real mouse and keyboard.
#   tools/apptest.sh setup                     test games: C:\Games\Test Game (anti-cheat + ReShade's d3d11.dll),
#                                              C:\Games\Plain (32-bit), C:\Program Files\DX11on10 Test (admin)
#   tools/apptest.sh run <name> "<steps>" [s]  start the app with --hits and play the steps (tools/input.ps1)
#   tools/apptest.sh cleanup                   remove the test games and the app's settings
set -e
cd "$(dirname "$0")/.."
VM=tools/vm.sh
SCRATCH=build/apptest
mkdir -p $SCRATCH

case "$1" in
setup)
    $VM ssh "if not exist C:\\dx11on10\\app mkdir C:\\dx11on10\\app"
    $VM put build/app/DX11on10.exe C:/dx11on10/app/
    $VM put build/x64/dx11test.exe C:/dx11on10/app/dx11test64.exe
    $VM put build/x86/dx11test.exe C:/dx11on10/app/dx11test32.exe
    $VM put tests/shaders/scene.hlsl C:/dx11on10/app/
    cat > $SCRATCH/setup.cmd <<'EOF'
@echo off
set A=C:\dx11on10\app
rmdir /s /q C:\Games "C:\Program Files\DX11on10 Test" 2>nul
del /q "%APPDATA%\DX11on10\settings.ini" 2>nul
mkdir "C:\Games\Test Game\EasyAntiCheat" C:\Games\Plain "C:\Program Files\DX11on10 Test"
copy /y %A%\dx11test64.exe "C:\Games\Test Game\TestGame.exe" >nul
copy /y %A%\scene.hlsl "C:\Games\Test Game\" >nul
echo This is ReShade's d3d11.dll (test stand-in)> "C:\Games\Test Game\d3d11.dll"
copy /y %A%\dx11test32.exe C:\Games\Plain\Plain.exe >nul
copy /y %A%\dx11test32.exe C:\Games\Plain\Other.exe >nul
copy /y %A%\scene.hlsl C:\Games\Plain\ >nul
copy /y %A%\dx11test64.exe "C:\Program Files\DX11on10 Test\AdminGame.exe" >nul
copy /y %A%\scene.hlsl "C:\Program Files\DX11on10 Test\" >nul
%A%\DX11on10.exe --add "C:\Games\Test Game\TestGame.exe"
%A%\DX11on10.exe --add "C:\Program Files\DX11on10 Test\AdminGame.exe"
dir /b /s C:\Games "C:\Program Files\DX11on10 Test"
EOF
    sed -i '' 's/$/\r/' $SCRATCH/setup.cmd
    $VM cmd $SCRATCH/setup.cmd | tr -d '\r'
    ;;
cleanup)
    $VM ssh "taskkill /im DX11on10.exe /f >nul 2>nul & rmdir /s /q C:\\Games \"C:\\Program Files\\DX11on10 Test\" 2>nul & del /q \"%APPDATA%\\DX11on10\\settings.ini\" 2>nul & exit 0"
    ;;
run)
    name=$2 steps=$3 seconds=${4:-120}
    out=$SCRATCH/$name
    rm -rf "$out"
    mkdir -p "$out"
    $VM put build/app/DX11on10.exe C:/dx11on10/app/
    $VM put tools/input.ps1
    cat > "$out/app.cmd" <<EOF
@echo off
cd /d C:\\dx11on10\\app
rmdir /s /q out 2>nul
mkdir out
start "" DX11on10.exe --hits C:\\dx11on10\\app\\out\\hits.txt
powershell -NoProfile -ExecutionPolicy Bypass -File C:\\dx11on10\\input.ps1 "$steps" DX11on10 C:\\dx11on10\\app\\out none C:\\dx11on10\\app\\out\\hits.txt
taskkill /im DX11on10.exe >nul 2>nul
ping -n 3 127.0.0.1 >nul
taskkill /im DX11on10.exe /f >nul 2>nul
echo done > C:\\dx11on10\\app\\done.flag
EOF
    sed -i '' 's/$/\r/' "$out/app.cmd"
    $VM ssh "del /q C:\\dx11on10\\app\\done.flag 2>nul & exit 0"
    $VM gui "$out/app.cmd" >/dev/null
    $VM ssh "for /l %i in (1,1,$seconds) do @(if exist C:\\dx11on10\\app\\done.flag (exit 0) else (ping -n 2 127.0.0.1 >nul))" || true
    files=$($VM ssh "dir /b C:\\dx11on10\\app\\out" | tr -d '\r')
    for f in $files; do $VM get "C:/dx11on10/app/out/$f" "$out/$f"; done
    cat "$out/input.log"
    ;;
*) echo "usage: apptest.sh setup | run <name> \"<steps>\" [seconds] | cleanup" >&2; exit 1 ;;
esac
