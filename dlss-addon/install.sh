#!/bin/sh
# Copy the built add-on (and the ini, unless one is already there) next to mgs4.exe.
# The game folder comes from MGS4_DIR / config.ini / the Steam libraries (see tools/paths.sh).
set -e
cd "$(dirname "$0")"
. ../tools/paths.sh
mgs4_require_game
cp ../build/mgs4_dlss.addon64 "$MGS4_DIR/"
if [ -f "$MGS4_DIR/mgs4_dlss.ini" ]; then
    echo "kept the existing $MGS4_DIR/mgs4_dlss.ini"
else
    cp mgs4_dlss.ini "$MGS4_DIR/"
fi
echo "installed to $MGS4_DIR/"
