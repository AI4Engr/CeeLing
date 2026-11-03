#!/usr/bin/env bash
# Rescans the standard library classes and rewrites the *.facts.tsv files in this directory (MSVC STL) and gcc/ (libstdc++).
# Then run gen_pack.py to rewrite facts/std-*.clfacts. Needs clang++ (CLANG), the MSVC STL headers (found by clang itself on
# this machine) and the MinGW libstdc++ headers (MINGW, GCCVER).
#
#   CLANG=C:/apps/emsdk/upstream/bin/clang++.exe MINGW=C:/apps/msys64/mingw64 GCCVER=16.2.0 ./scan.sh
#
# The scan runs at C++17, the standard the generated code is compiled with. The AST dumps are written to $WORK (default
# $TMP/cling-scan) and are not committed (hundreds of MB of text); only the small tables are.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
CLANG="${CLANG:-C:/apps/emsdk/upstream/bin/clang++.exe}"
MINGW="${MINGW:-C:/apps/msys64/mingw64}"
GCCVER="${GCCVER:-16.2.0}"
WORK="${WORK:-${TMP:-/tmp}/cling-scan}"
mkdir -p "$WORK" "$here/gcc"

msvc() { "$CLANG" -std=c++17 -fsyntax-only -Xclang -ast-dump "$1" > "$2" 2>/dev/null; }
gcc_() { "$CLANG" -std=c++17 -fsyntax-only -target x86_64-w64-mingw32 -nostdinc++ \
    -isystem "$MINGW/include/c++/$GCCVER" -isystem "$MINGW/include/c++/$GCCVER/x86_64-w64-mingw32" -isystem "$MINGW/include" \
    -Xclang -ast-dump "$1" > "$2" 2>/dev/null; }

# line of the explicit instantiation of class $2 (the Nth one, default 1) in dump $1
spec() { grep -n -E "^\|-ClassTemplateSpecializationDecl .*class $2 definition.*explicit_instantiation" "$1" | sed -n "${3:-1}p" | cut -d: -f1; }
# line of the implicit instantiation of the base class $2 (MSVC STL keeps most of map's members in _Tree), the Nth one
base() { grep -n -E "^\|   [|\`]-ClassTemplateSpecializationDecl .*class $2 definition" "$1" | sed -n "${3:-1}p" | cut -d: -f1; }

for impl in msvc gcc; do
    f=$([ $impl = msvc ] && echo msvc_ || echo gcc_)
    if [ $impl = msvc ]; then run=msvc; out="$here"; else run=gcc_; out="$here/gcc"; fi
    $run "$here/probe.cc" "$WORK/vs_$impl.txt"
    $run "$here/probe_map.cc" "$WORK/map_$impl.txt"
    v=$(spec "$WORK/vs_$impl.txt" vector); s=$(spec "$WORK/vs_$impl.txt" basic_string)
    python -I "$here/classify.py" "$WORK/vs_$impl.txt" "$out" "vector=vector@$v" "string=basic_string@$s"
    m1=$(spec "$WORK/map_$impl.txt" map 1); m2=$(spec "$WORK/map_$impl.txt" map 2)
    if [ $impl = msvc ]; then
        t1=$(base "$WORK/map_$impl.txt" _Tree 1); t2=$(base "$WORK/map_$impl.txt" _Tree 2)
        python -I "$here/classify.py" "$WORK/map_$impl.txt" "$out" "map=map,_Tree@$m1+$t1" "map_si=map,_Tree@$m2+$t2"
    else
        python -I "$here/classify.py" "$WORK/map_$impl.txt" "$out" "map=map@$m1" "map_si=map@$m2"
    fi
done
