#!/bin/sh
# Build MGS4_D3D12.asi with mingw-w64 gcc (Git Bash: sh build.sh). Output: build/MGS4_D3D12.asi
set -e
cd "$(dirname "$0")"
mkdir -p ../build
MH=../third_party/minhook
gcc -O2 -shared -static -s -Wall -o ../build/MGS4_D3D12.asi mgs4_d3d12.c $MH/src/*.c $MH/src/hde/hde64.c -I$MH/include -I$MH/src
ls -la ../build/MGS4_D3D12.asi
