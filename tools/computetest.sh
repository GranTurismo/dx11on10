#!/bin/bash
# Runs computetest in the VM natively and through the proxy (feature level 10), then
# compares every test's output.  tools/computetest.sh [name]
set -e
cd "$(dirname "$0")/.."
name=${1:-compute}
out=build/computetest/$name
mkdir -p "$out"
VM=tools/vm.sh
cat > "$out/run.cmd" <<'EOF'
@echo off
cd /d C:\dx11on10
rmdir /s /q cref cproxy 2>nul
mkdir cref cproxy
copy /y computetest.exe cref\ >nul
copy /y compute.hlsl cref\ >nul
copy /y computetest.exe cproxy\ >nul
copy /y compute.hlsl cproxy\ >nul
copy /y stage\d3d11.dll cproxy\ >nul
copy /y stage\compute.ini cproxy\dx11on10.ini >nul
start "" /wait cref\computetest.exe
start "" /wait cproxy\computetest.exe
echo done > done.flag
EOF
sed -i '' 's/$/\r/' "$out/run.cmd"
printf '[dx11on10]\nMaxFeatureLevel=10_1\nDebugLayer=%s\nDriver=%s\n' "${DEBUG:-0}" "${DRIVER:-hardware}" > "$out/compute.ini"
$VM ssh "if not exist C:\\dx11on10\\stage mkdir C:\\dx11on10\\stage"
$VM put build/x64/computetest.exe
$VM put tests/shaders/compute.hlsl
$VM put build/x64/d3d11.dll C:/dx11on10/stage/
$VM put "$out/compute.ini" C:/dx11on10/stage/
$VM ssh "del /q C:\\dx11on10\\done.flag 2>nul & exit 0"
$VM gui "$out/run.cmd" >/dev/null
$VM ssh "for /l %i in (1,1,60) do @(if exist C:\\dx11on10\\done.flag (exit 0) else (ping -n 2 127.0.0.1 >nul))" || true
for f in cref/computetest.log cref/computetest.bin cproxy/computetest.log cproxy/computetest.bin cproxy/dx11on10.log; do
    $VM get "C:/dx11on10/$f" "$out/$(echo $f | tr / _)" 2>/dev/null || echo "missing: $f"
done
python3 - "$out" <<'PY'
import re, sys
out = sys.argv[1]
def tests(log):
    return [(m.group(1), int(m.group(3))) for m in re.finditer(r'test (\S+): hash (\w+), (\d+) bytes', open(log).read())]
ref, got = tests(f'{out}/cref_computetest.log'), tests(f'{out}/cproxy_computetest.log')
a, b = open(f'{out}/cref_computetest.bin', 'rb').read(), open(f'{out}/cproxy_computetest.bin', 'rb').read()
pa = pb = 0
names = dict(got)
for name, size in ref:
    if name not in names:
        print(f'{name:12} MISSING through the proxy'); pa += size; continue
    x, y = a[pa:pa + size], b[pb:pb + names[name]]
    pa += size; pb += names[name]
    diff = sum(1 for i in range(min(len(x), len(y))) if x[i] != y[i])
    first = next((i for i in range(min(len(x), len(y))) if x[i] != y[i]), None)
    print(f'{name:12} {"OK" if diff == 0 and len(x) == len(y) else "DIFFERENT"}  ({size} bytes, {diff} differ' + (f', first at byte {first})' if first is not None else ')'))
PY
