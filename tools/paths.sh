#!/bin/sh
# The Git Bash half of tools/paths.py: resolves the game folder the same way (MGS4_DIR in the environment >
# config.ini in the repo root > the Steam libraries), as a POSIX path so cp/install work with it.
#
#   . "$(dirname "$0")/../tools/paths.sh"    - sets MGS4_DIR (and MGS4_OUT)
#   sh tools/paths.sh                        - print them

# shellcheck disable=SC3028   # BASH_SOURCE: Git Bash is bash even when invoked as sh
mgs4_self="${BASH_SOURCE:-$0}"
mgs4_repo=$(cd "$(dirname "$mgs4_self")/.." && pwd)
mgs4_config="$mgs4_repo/config.ini"

# Windows path -> POSIX path (C:\x\y -> /c/x/y), so the result can be used by cp/mkdir here.
mgs4_posix() {
    if command -v cygpath >/dev/null 2>&1; then cygpath -u "$1"; else
        printf '%s\n' "$1" | sed 's|\\|/|g; s|^\([A-Za-z]\):|/\L\1|'
    fi
}

mgs4_from_config() {
    [ -f "$mgs4_config" ] || return 0
    sed -n "s/^[[:space:]]*$1[[:space:]]*=[[:space:]]*//Ip" "$mgs4_config" | sed 's/[[:space:]]*$//; s/^"//; s/"$//' | grep -v '^$' | head -1
}

# Every Steam library folder, from the registry and the usual install spots.
mgs4_steam_libraries() {
    roots=""
    for key in "HKCU\\SOFTWARE\\Valve\\Steam //v SteamPath" "HKLM\\SOFTWARE\\WOW6432Node\\Valve\\Steam //v InstallPath"; do
        r=$(eval reg query "$key" 2>/dev/null | sed -n 's/.*REG_SZ[[:space:]]*//p' | head -1)
        [ -n "$r" ] && roots="$roots
$r"
    done
    roots="$roots
C:\\Program Files (x86)\\Steam
C:\\Program Files\\Steam"
    printf '%s\n' "$roots" | while IFS= read -r root; do
        [ -n "$root" ] || continue
        root=$(mgs4_posix "$root")
        [ -d "$root" ] && printf '%s\n' "$root"
        vdf="$root/steamapps/libraryfolders.vdf"
        [ -f "$vdf" ] || continue
        sed -n 's/^[[:space:]]*"path"[[:space:]]*"\(.*\)"[[:space:]]*$/\1/p' "$vdf" | while IFS= read -r lib; do
            lib=$(printf '%s\n' "$lib" | sed 's|\\\\|\\|g')
            lib=$(mgs4_posix "$lib")
            [ -d "$lib" ] && printf '%s\n' "$lib"
        done
    done
}

mgs4_detect_game() {
    mgs4_steam_libraries | while IFS= read -r lib; do
        for name in "METAL GEAR SOLID 4"; do
            cand="$lib/steamapps/common/$name/MGS4"
            if [ -f "$cand/mgs4.exe" ]; then printf '%s\n' "$cand"; return 0; fi
        done
    done | head -1
}

if [ -z "$MGS4_DIR" ]; then MGS4_DIR=$(mgs4_from_config MGS4_DIR); fi
if [ -z "$MGS4_DIR" ]; then MGS4_DIR=$(mgs4_detect_game); fi
case "$MGS4_DIR" in *\\*|[A-Za-z]:*) MGS4_DIR=$(mgs4_posix "$MGS4_DIR");; esac
# accept the install root as well as the MGS4 folder inside it
[ -f "$MGS4_DIR/MGS4/mgs4.exe" ] && MGS4_DIR="$MGS4_DIR/MGS4"

if [ -z "$MGS4_OUT" ]; then MGS4_OUT=$(mgs4_from_config MGS4_OUT); fi
[ -z "$MGS4_OUT" ] && MGS4_OUT="$mgs4_repo/work"
case "$MGS4_OUT" in *\\*|[A-Za-z]:*) MGS4_OUT=$(mgs4_posix "$MGS4_OUT");; esac
export MGS4_DIR MGS4_OUT

# The game folder, or an explanation of how to set it.
mgs4_require_game() {
    if [ ! -f "$MGS4_DIR/mgs4.exe" ]; then
        echo "mgs4.exe not found${MGS4_DIR:+ in $MGS4_DIR}." >&2
        echo "Set MGS4_DIR in $mgs4_config (copy config.example.ini) or in the environment." >&2
        exit 1
    fi
}

case "$0" in *paths.sh) printf 'MGS4_DIR=%s\nMGS4_OUT=%s\n' "$MGS4_DIR" "$MGS4_OUT";; esac
