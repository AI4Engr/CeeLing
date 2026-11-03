#!/usr/bin/env bash
# cling-scan (tools/cling-scan/cling_scan.py): a draft fact pack written from a class declaration, then USED.
#
#   usage: run_scan_test.sh <path-to-cling> <path-to-clang++>
set -u
CLING="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
CLANG="$2"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
SCAN="$ROOT/tools/cling-scan/cling_scan.py"
LIB="$ROOT/tests/facts/private_lib"
CXX="${CXX:-g++}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fail=0
ok()  { echo "PASS: $1"; }
bad() { echo "FAIL: $1"; fail=1; }
scan() { python -I "$SCAN" --clang "$CLANG" "$@"; }

# 1. every method shape gets the verdict the signature proves, and nothing more
scan --header "$HERE/shapes.h" --class Shapes --out "$TMP/shapes.clfacts" >/dev/null || bad "scanning shapes.h failed"
got="$(grep -E '^(fresh|borrow|mutator|function|opaque)' "$TMP/shapes.clfacts" | sed 's/ source=.*//' | sort | tr '\n' ';')"
want="borrow Shapes::at;borrow Shapes::raw;borrow Shapes::view;fresh Shapes::count;fresh Shapes::data;fresh Shapes::empty;fresh Shapes::name;function Shapes::version;mutator Shapes::clear;mutator Shapes::pop;opaque Shapes;"
if [ "$got" = "$want" ]; then ok "each method shape gets exactly the fact its signature proves"; else bad "shape verdicts differ:
  got:  $got
  want: $want"; fi
for m in copy make over ref; do
    grep -q "^# $m: no fact" "$TMP/shapes.clfacts" && ok "$m: left to the opaque default, with the reason written down" || bad "$m: no explanation in the pack"
done
if grep -qE '^(fresh|borrow|mutator|function) Shapes::(hidden|as|operator)' "$TMP/shapes.clfacts"; then bad "a private method, a template or an operator got a fact"; else ok "private methods, member templates and operators get no fact"; fi
grep -q "^# skipped: a member template" "$TMP/shapes.clfacts" && ok "skipped member templates are reported" || bad "skipped template not reported"

# 2. the pack loads (strict loader) and is marked as a scanner's draft
printf 'int main() { return 0; }
' > "$TMP/empty.clg"
out="$("$CLING" --facts "$TMP/shapes.clfacts" --report-facts "$TMP/empty.clg" --cpp "$TMP/x.cpp" 2>&1)"
echo "$out" | grep -q "scan-Shapes@1" && echo "$out" | grep -q "scanned header  scan-Shapes: .*shapes.h sha256=" && ok "the generated pack passes the strict loader and its header hash is reported" || bad "pack rejected by the loader: $out"

# 3. the generated pack for the private library behaves like the hand-written one on the same programs
scan --header "$LIB/grid.h" --class Grid --out "$TMP/grid.clfacts" >/dev/null || bad "scanning grid.h failed"
cd "$LIB"
if "$CLING" --facts "$TMP/grid.clfacts" ok.clg --cpp "$TMP/ok.cpp" >/dev/null 2>&1 &&
   "$CXX" -std=c++17 -I"$LIB" "$TMP/ok.cpp" -o "$TMP/ok.exe" 2>/dev/null && [ "$("$TMP/ok.exe")" = "3 2 6 2" ]; then
    ok "scanned pack: the private-library program builds and runs"
else bad "scanned pack: ok.clg did not build/run"; fi
out="$("$CLING" --facts "$TMP/grid.clfacts" dangling_rejected.clg --cpp "$TMP/x.cpp" 2>&1)"
[ $? -ne 0 ] && echo "$out" | grep -q "dangling observer" && ok "scanned pack: window() is assumed to borrow (no fact), so the dangling window is rejected" || bad "dangling not rejected: $out"
out="$("$CLING" --facts "$TMP/grid.clfacts" opaque_unknown_call_kills_rejected.clg --cpp "$TMP/x.cpp" 2>&1)"
[ $? -ne 0 ] && echo "$out" | grep -q "dangling observer" && ok "scanned pack: a mutator kills the observers of its receiver" || bad "mutator did not kill: $out"
"$CLING" --facts "$TMP/grid.clfacts" no_fact_rejected.clg --cpp "$TMP/x.cpp" >/dev/null 2>&1 && ok "scanned pack: shrink() is a proven mutator, callable when nothing observes the grid" || bad "mutator call refused"
"$CLING" --facts "$TMP/grid.clfacts" opaque_retain_rejected.clg --cpp "$TMP/x.cpp" >/dev/null 2>&1 && bad "an unlisted method handed an object was accepted" || ok "scanned pack: a method with no fact still refuses object arguments"

# 4. header binding: --check notices a changed header
cp "$LIB/grid.h" "$TMP/grid.h"
scan --header "$TMP/grid.h" --class Grid --out "$TMP/bound.clfacts" >/dev/null
scan --clang "$CLANG" --check "$TMP/bound.clfacts" >/dev/null 2>&1; [ $? -eq 0 ] && ok "--check: unchanged header matches the pack" || bad "--check failed on an unchanged header"
printf '\n// edited\nint extra();\n' >> "$TMP/grid.h"
scan --check "$TMP/bound.clfacts" >/dev/null 2>&1; [ $? -eq 1 ] && ok "--check: a changed header is reported" || bad "--check did not notice the edit"
printf '@pack p\n@version 1\n@origin t\n@header x.h sha256=zz\nfunction f source=declared\n' > "$TMP/badhash.clfacts"
"$CLING" --facts "$TMP/badhash.clfacts" ok.clg --cpp "$TMP/x.cpp" >/dev/null 2>&1; [ $? -eq 2 ] && ok "the loader rejects a malformed @header line" || bad "malformed @header accepted"

# 5. a real third-party library: jsoncpp's Json::Value, scanned, then used from CeeLing against the real jsoncpp sources
JSONCPP_DIR="${JSONCPP_DIR:-/c/project/jsoncpp-master/jsoncpp-master}"
if [ -f "$JSONCPP_DIR/include/json/json.h" ]; then
    scan --header "$JSONCPP_DIR/include/json/json.h" --class Json::Value -I "$JSONCPP_DIR/include" --out "$TMP/jv.clfacts" >/dev/null && ok "jsoncpp: Json::Value scanned ($(grep -cE '^(fresh|borrow|mutator|function) ' "$TMP/jv.clfacts") facts)" || bad "jsoncpp: scan failed"
    cd "$HERE"
    if "$CLING" --facts "$TMP/jv.clfacts" jsoncpp_use.clg --cpp "$TMP/ju.cpp" >/dev/null 2>&1 &&
       "$CXX" -std=c++17 -I"$JSONCPP_DIR/include" -DJSON_USE_EXCEPTION=0 "$TMP/ju.cpp" "$JSONCPP_DIR/src/lib_json/json_value.cpp" "$JSONCPP_DIR/src/lib_json/json_reader.cpp" "$JSONCPP_DIR/src/lib_json/json_writer.cpp" -o "$TMP/ju.exe" 2>"$TMP/ju.err" &&
       [ "$("$TMP/ju.exe")" = "7 1 0 0" ]; then
        ok "jsoncpp: a CeeLing program using the scanned pack builds against the real jsoncpp sources and runs"
    else bad "jsoncpp: use program did not build/run: $(head -3 "$TMP/ju.err" 2>/dev/null)"; fi
    out="$("$CLING" --facts "$TMP/jv.clfacts" jsoncpp_dangling_rejected.clg --cpp "$TMP/x.cpp" 2>&1)"
    [ $? -ne 0 ] && echo "$out" | grep -q "dangling observer" && ok "jsoncpp: a method the scan could not prove fresh is assumed to borrow, so the dangling use is rejected" || bad "jsoncpp dangling not rejected: $out"
else
    echo "SKIP: jsoncpp trial (no jsoncpp at $JSONCPP_DIR; set JSONCPP_DIR)"
fi

exit $fail
