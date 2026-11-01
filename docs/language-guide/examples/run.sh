#!/usr/bin/env bash
# Compile and run the examples of the language guide.
#   run.sh                     every example: one line each (what the compiler said, what the program did)
#   run.sh 04_after_move       one example by name (with or without .clg): shows the source, the compiler's answer and, if it was accepted, what the program printed
#   run.sh 04_after_move.cpp   a C++ twin: compiled with g++ -Wall -Wextra and run (it has undefined behaviour or a leak by design: expect nonsense, a crash, or nothing)
# CLING names the CeeLing compiler (default: build/cling.exe or build/cling in the repository), CXX the C++ compiler (default g++).
# The checked version of all this, which also compares every chapter's code blocks with these files, is check.py.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
CLING="${CLING:-}"
for c in "$REPO/build/cling.exe" "$REPO/build/cling"; do [ -z "$CLING" ] && [ -x "$c" ] && CLING="$c"; done
[ -n "$CLING" ] || { echo "no CeeLing compiler: build it (see the repository's README) or set CLING" >&2; exit 2; }
CXX="${CXX:-g++}"
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT

stage() { # file...: copies examples into $W/stage WITHOUT their //@ lines (what the chapters show, so line numbers agree), and the .clg they #include
    mkdir -p "$W/stage"
    local f inc
    for f in "$@"; do
        [ -f "$W/stage/$f" ] && continue
        [ -f "$HERE/$f" ] || continue
        sed '/^\/\/@/d' "$HERE/$f" | sed '/./,$!d' > "$W/stage/$f"
        for inc in $(sed -n 's|^[[:space:]]*#include[[:space:]]*"\([^"]*\)\.clh".*|\1.clg|p' "$HERE/$f"); do stage "$inc"; done
    done
}

one() { # name -> prints a report; verbose 1 shows the source
    local f="$1" verbose="$2" base="${1%.*}"
    if [ "${f##*.}" = cpp ]; then
        [ "$verbose" = 1 ] && { echo "---- $f"; sed '/^\/\/@/d' "$HERE/$f"; echo "----"; }
        if "$CXX" -std=c++17 -Wall -Wextra -pthread "$HERE/$f" -o "$W/$base.exe" 2> "$W/err.txt"; then
            echo "$f: compiled$( [ -s "$W/err.txt" ] && echo " with warnings" || echo ", no warnings")"
            [ "$verbose" = 1 ] && { cat "$W/err.txt"; echo "---- running it"; timeout 20 "$W/$base.exe"; echo "---- exit code $?"; }
        else echo "$f: g++ does not accept it"; cat "$W/err.txt"; fi
        return
    fi
    [ "$verbose" = 1 ] && { echo "---- $f"; sed '/^\/\/@/d' "$HERE/$f"; echo "----"; }
    local flags; flags="$(sed -n 's|^//@ flags: *||p' "$HERE/$f" | head -1)"
    stage "$f"; if (cd "$W/stage" && "$CLING" $flags "$f" --cpp "$W/$base.cpp") > "$W/out.txt" 2> "$W/err.txt"; then
        if "$CXX" -std=c++17 -pthread -O2 "$W/$base.cpp" -o "$W/$base.exe" 2> "$W/cxx.txt"; then
            timeout 60 "$W/$base.exe" > "$W/run.txt" 2> "$W/runerr.txt"; local code=$?
            echo "$f: accepted; ran, exit $code$( [ -s "$W/run.txt" ] && echo ", printed: $(head -c 120 "$W/run.txt" | tr '\n' '|')" )$( [ -s "$W/runerr.txt" ] && echo ", stderr: $(head -c 120 "$W/runerr.txt" | tr '\n' '|')" )"
        else echo "$f: accepted, but the generated C++ does not compile"; head -5 "$W/cxx.txt"; fi
    else
        echo "$f: rejected"
        [ "$verbose" = 1 ] && cat "$W/err.txt" || head -1 "$W/err.txt"
    fi
}

if [ $# -ge 1 ]; then
    for n in "$@"; do
        n="${n##*/}"; [ -f "$HERE/$n" ] || { [ -f "$HERE/$n.clg" ] && n="$n.clg"; }
        [ -f "$HERE/$n" ] || { echo "no such example: $n" >&2; exit 2; }
        one "$n" 1
    done
else
    for f in "$HERE"/*.clg; do one "$(basename "$f")" 0; done
fi
