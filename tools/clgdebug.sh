#!/usr/bin/env bash
# Compile a CeeLing program for the debugger and start gdb on it, stopped at the first statement of main().
#   tools/clgdebug.sh FILE.clg [more gdb arguments, e.g. -ex 'break FILE.clg:12']
# What it does: cling -g FILE.clg -> FILE.debug.cpp, g++ -g -O0 -> FILE.debug.exe (both next to FILE.clg, so gdb finds the .clg source), then
# gdb -x tools/cling.gdb with a breakpoint on main. `step` goes through your .clg lines and over the library code. CLING and CXX name the compilers.
set -euo pipefail
[ $# -ge 1 ] || { echo "usage: $0 FILE.clg [gdb arguments]" >&2; exit 2; }
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(dirname "$HERE")"
SRC="$1"; shift
[ -f "$SRC" ] || { echo "no such file: $SRC" >&2; exit 2; }
CLING="${CLING:-}"
for c in "$REPO/build/cling.exe" "$REPO/build/cling"; do [ -z "$CLING" ] && [ -x "$c" ] && CLING="$c"; done
[ -n "$CLING" ] || { echo "no CeeLing compiler: build it or set CLING" >&2; exit 2; }
CXX="${CXX:-g++}"
command -v gdb > /dev/null 2>&1 || { echo "gdb is not installed" >&2; exit 2; }
DIR="$(cd "$(dirname "$SRC")" && pwd)"; FILE="$(basename "$SRC")"; BASE="${FILE%.clg}"
cd "$DIR"
"$CLING" -g "$FILE" --cpp "$BASE.debug.cpp" > /dev/null
"$CXX" -std=c++17 -pthread -g -O0 "$BASE.debug.cpp" -o "$BASE.debug.exe"
echo "built $DIR/$BASE.debug.exe from $FILE; starting gdb (type 'step', 'next', 'bt', 'print x', 'continue', 'quit')"
exec gdb -q -x "$REPO/tools/cling.gdb" -ex 'break main' -ex 'run' "$@" "./$BASE.debug.exe"
