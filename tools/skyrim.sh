#!/bin/bash
# Quick Skyrim SE check in the VM (about 80 s): start the game, open the console at the
# main menu, "coc riverwood" (loads Riverwood without touching any save), screenshot.
# Needs tools/skyrim/Skyrim.ini and SkyrimPrefs.ini installed in the VM (no intro, no autosaves).
#   tools/skyrim.sh <name> [proxy.ini]      default: DX10.1 with the layer
set -e
cd "$(dirname "$0")/.."
name=$1
ini=${2:-build/gametest/dx10.ini}
if [ "$ini" != keep ] && [ ! -f "$ini" ]; then
    mkdir -p build/gametest
    printf '[dx11on10]\nMaxFeatureLevel=10_1\nFakeFeatureLevel=always\n' > "$ini"
fi
# Put the test settings back every time: the Skyrim launcher rewrites them when the game
# is started from Steam.
tools/vm.sh ssh "if not exist C:\\dx11on10\\stage mkdir C:\\dx11on10\\stage"
tools/vm.sh put tools/skyrim/Skyrim.ini C:/dx11on10/stage/
tools/vm.sh put tools/skyrim/SkyrimPrefs.ini C:/dx11on10/stage/
tools/vm.sh ssh "set D=C:\\Users\\kupat\\OneDrive\\Documents\\My Games\\Skyrim Special Edition& call copy /y C:\\dx11on10\\stage\\Skyrim.ini \"%D%\\Skyrim.ini\" >nul & call copy /y C:\\dx11on10\\stage\\SkyrimPrefs.ini \"%D%\\SkyrimPrefs.ini\" >nul"
export GAME_DIR='C:\Program Files (x86)\Steam\steamapps\common\Skyrim Special Edition'
export GAME_EXE=SkyrimSE.exe GAME_ARCH=x64 GAME_ARGS=' '
export GAME_INPUT='waitlog:first_frame_presented wait:27 key:GRAVE wait:1 type:coc_riverwood wait:1 key:ENTER wait:14 shot:world wait:4 shot:world2'
tools/gametest.sh "$name" "$ini" 70
