#!/bin/sh
# Copy the built ASI + ini into the game's scripts folder (Ultimate ASI Loader picks it up).
set -e
cd "$(dirname "$0")"
GAME="${MGS4_DIR:-/c/Program Files (x86)/Steam/steamapps/common/METAL GEAR SOLID 4/MGS4}"
cp ../build/MGS4_D3D12.asi MGS4_D3D12.ini "$GAME/scripts/"
echo "installed to $GAME/scripts/"
