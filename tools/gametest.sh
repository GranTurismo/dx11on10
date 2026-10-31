#!/bin/bash
# Runs a game in the VM's desktop session with the proxy installed, then fetches the
# proxy log, the game's own log, the proxy's frame screenshots and desktop screenshots.
#   tools/gametest.sh <name> <ini-file|none> [seconds]
# "none" removes the proxy from the game folder (a plain run); "keep" leaves the folder as it
# is (to test what the DX11on10 app installed).
# The game is set by GAME_DIR / GAME_EXE / GAME_ARCH / GAME_ARGS (defaults: Twelve Minutes).
set -e
cd "$(dirname "$0")/.."
name=$1
ini=$2
seconds=${3:-120}
GAME_DIR=${GAME_DIR:-'C:\Program Files (x86)\Steam\steamapps\common\Twelve Minutes'}
GAME_EXE=${GAME_EXE:-12M.exe}
GAME_ARCH=${GAME_ARCH:-x86}
GAME_ARGS=${GAME_ARGS:-'-screen-fullscreen 0 -screen-width 1280 -screen-height 720 -logFile C:\dx11on10\game\player.log'}
out=build/gametest/$name
mkdir -p "$out"
VM=tools/vm.sh

half=$((seconds / 2))
# GAME_INPUT replaces the two timed screenshots with an input script (see tools/input.ps1).
if [ -n "$GAME_INPUT" ]; then
    process=${GAME_EXE%.exe}
    play="powershell -NoProfile -ExecutionPolicy Bypass -File C:\\dx11on10\\input.ps1 \"$GAME_INPUT\" $process C:\\dx11on10\\game \"$GAME_DIR\\dx11on10.log\""
else
    play="ping -n $half 127.0.0.1 >nul
powershell -NoProfile -ExecutionPolicy Bypass -File C:\\dx11on10\\screen.ps1 C:\\dx11on10\\game\\screen1.png
ping -n $half 127.0.0.1 >nul
powershell -NoProfile -ExecutionPolicy Bypass -File C:\\dx11on10\\screen.ps1 C:\\dx11on10\\game\\screen2.png"
fi
cat > "$out/game.cmd" <<EOF
@echo off
cd /d C:\\dx11on10
rmdir /s /q game 2>nul
mkdir game
cd /d "$GAME_DIR"
del /q dx11on10.log dx11on10_frame*.bmp 2>nul
start "" "$GAME_EXE" $GAME_ARGS
$play
tasklist /fi "imagename eq $GAME_EXE" | find /i "$GAME_EXE" >nul && (echo still running > C:\\dx11on10\\game\\running.txt) || (echo exited > C:\\dx11on10\\game\\running.txt)
taskkill /im "$GAME_EXE" /f >nul 2>nul
ping -n 3 127.0.0.1 >nul
copy /y dx11on10.log C:\\dx11on10\\game\\ >nul 2>nul
copy /y dx11on10_frame*.bmp C:\\dx11on10\\game\\ >nul 2>nul
echo done > C:\\dx11on10\\done.flag
EOF
sed -i '' 's/$/\r/' "$out/game.cmd"

$VM put tools/screen.ps1
$VM put tools/input.ps1
$VM ssh "if not exist C:\\dx11on10\\stage mkdir C:\\dx11on10\\stage"
$VM put build/$GAME_ARCH/d3d11.dll C:/dx11on10/stage/
if [ "$ini" = keep ]; then
    :
elif [ "$ini" = none ]; then
    $VM ssh "del /q \"$GAME_DIR\\d3d11.dll\" \"$GAME_DIR\\dx11on10.ini\" 2>nul & exit 0"
else
    cp "$ini" "$out/dx11on10.ini"
    $VM put "$out/dx11on10.ini" C:/dx11on10/stage/
    $VM ssh "copy /y C:\\dx11on10\\stage\\d3d11.dll \"$GAME_DIR\\\" >nul && copy /y C:\\dx11on10\\stage\\dx11on10.ini \"$GAME_DIR\\\" >nul"
fi
$VM ssh "del /q C:\\dx11on10\\done.flag 2>nul & exit 0"
$VM gui "$out/game.cmd" >/dev/null
$VM ssh "for /l %i in (1,1,$((seconds / 2 + 60))) do @(if exist C:\\dx11on10\\done.flag (exit 0) else (ping -n 3 127.0.0.1 >nul))" || true
for f in $($VM ssh "dir /b C:\\dx11on10\\game" | tr -d '\r'); do
    $VM get "C:/dx11on10/game/$f" "$out/$f"
done
for f in "$out"/*.bmp "$out"/*.png; do
    [ -e "$f" ] || continue
    case $f in *.bmp) sips -s format png "$f" --out "${f%.bmp}.png" >/dev/null ;; esac
done
echo "results in $out: $(ls "$out" | tr '\n' ' ')"
