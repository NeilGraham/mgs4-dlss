#!/bin/sh
# Copy the built ASI + ini into the game's scripts folder (Ultimate ASI Loader picks it up).
# The game folder comes from MGS4_DIR / config.ini / the Steam libraries (see tools/paths.sh).
set -e
cd "$(dirname "$0")"
. ../tools/paths.sh
mgs4_require_game
mkdir -p "$MGS4_DIR/scripts"
cp ../build/MGS4_D3D12.asi MGS4_D3D12.ini "$MGS4_DIR/scripts/"
echo "installed to $MGS4_DIR/scripts/"
