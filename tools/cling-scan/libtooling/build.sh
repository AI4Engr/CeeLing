#!/usr/bin/env bash
# Builds cling-scan-extract, the LibTooling front end of cling-scan, against the LLVM/Clang development packages.
#
#   build.sh [output-path]            (default: ./cling-scan-extract next to this script)
#
# Needs: g++ (C++17), llvm-config (LLVM_CONFIG, default llvm-config-14 or llvm-config), and the clang development headers and library
# (Ubuntu: clang-14 libclang-14-dev libclang-cpp14-dev llvm-14-dev). One compile and one link, about half a minute.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${1:-$HERE/cling-scan-extract}"
LLVM_CONFIG="${LLVM_CONFIG:-$(command -v llvm-config-14 || command -v llvm-config)}"
[ -n "$LLVM_CONFIG" ] || { echo "build.sh: llvm-config not found (install the LLVM/Clang development packages, or set LLVM_CONFIG)" >&2; exit 1; }
LIBDIR="$("$LLVM_CONFIG" --libdir)"
VERSION="$("$LLVM_CONFIG" --version)"
RESOURCE="$LIBDIR/clang/$VERSION"
[ -d "$RESOURCE/include" ] || RESOURCE="$(ls -d "$LIBDIR"/clang/*/ 2>/dev/null | head -1)"
g++ -std=c++17 -fno-rtti -O1 "$HERE/cling_scan_extract.cpp" -o "$OUT" \
    $("$LLVM_CONFIG" --cxxflags | sed 's/-std=c++[0-9a-z]*//; s/-fno-exceptions//; s/-fno-rtti//') \
    -DCLING_SCAN_RESOURCE_DIR="\"${RESOURCE%/}\"" \
    -L"$LIBDIR" -Wl,-rpath,"$LIBDIR" -lclang-cpp -lLLVM
echo "built $OUT (clang $VERSION, resource dir ${RESOURCE%/})"
