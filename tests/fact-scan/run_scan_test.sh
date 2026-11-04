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
want="borrow Shapes::at;borrow Shapes::raw;borrow Shapes::view;fresh Shapes::big;fresh Shapes::count;fresh Shapes::data;fresh Shapes::empty;fresh Shapes::name;function Shapes::make;function Shapes::version;mutator Shapes::clear;mutator Shapes::pop;opaque Shapes;"
if [ "$got" = "$want" ]; then ok "each method shape gets exactly the fact its signature proves"; else bad "shape verdicts differ:
  got:  $got
  want: $want"; fi
# a class that FOLLOWS another in the dump must not leak into it (found by the jsoncpp probe: Json::Value picked up ValueIteratorBase::index)
if grep -qE '^(fresh|borrow|mutator|function) Shapes::viewCount' "$TMP/shapes.clfacts"; then bad "a later class whose name contains Shapes leaked into the Shapes pack"; else ok "a later class whose name contains the scanned one's (the next node in the filtered dump) does not leak into its pack"; fi
for m in copy over ref; do
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
# 4b. the COMPILER enforces the header hashes (not only the scanner's --check)
cp "$LIB/grid.h" "$TMP/h1.h"
scan --header "$TMP/h1.h" --class Grid --out "$TMP/h1.clfacts" >/dev/null
printf 'int main() { return 0; }\n' > "$TMP/h1.clg"
"$CLING" --facts "$TMP/h1.clfacts" "$TMP/h1.clg" --cpp "$TMP/h1.cpp" >/dev/null 2>&1 && ok "compiler: a pack whose headers are unchanged loads" || bad "compiler: refused a pack with unchanged headers"
# only the line endings change (LF -> CRLF): the hash is taken over LF-normalised content
awk '{ printf "%s\r\n", $0 }' "$LIB/grid.h" > "$TMP/h1.h"
"$CLING" --facts "$TMP/h1.clfacts" "$TMP/h1.clg" --cpp "$TMP/h1.cpp" >/dev/null 2>&1 && ok "compiler: changing only the line endings of a header does not invalidate the pack" || bad "compiler: line endings alone invalidated the pack"
cp "$LIB/grid.h" "$TMP/h1.h"; printf '\n// one more line\n' >> "$TMP/h1.h"
out="$("$CLING" --facts "$TMP/h1.clfacts" "$TMP/h1.clg" --cpp "$TMP/h1.cpp" 2>&1)"; code=$?
if [ $code -eq 2 ] && echo "$out" | grep -q "scan-Grid" && echo "$out" | grep -q "h1.h" && echo "$out" | grep -q "changed after the pack was written"; then
    ok "compiler: a changed header makes the pack unusable (exit 2), naming the pack and the header"
else bad "compiler: changed header not refused (exit $code): $out"; fi
rm "$TMP/h1.h"
out="$("$CLING" --facts "$TMP/h1.clfacts" "$TMP/h1.clg" --cpp "$TMP/h1b.cpp" 2>&1)"; code=$?
if [ $code -eq 0 ] && echo "$out" | grep -q "warning" && echo "$out" | grep -q "not found at the recorded place"; then ok "compiler: a header that cannot be found is a warning, said out loud"; else bad "compiler: missing header handling wrong (exit $code): $out"; fi
"$CLING" --strict-headers --facts "$TMP/h1.clfacts" "$TMP/h1.clg" --cpp "$TMP/h1c.cpp" >/dev/null 2>&1; [ $? -eq 2 ] && ok "compiler: --strict-headers turns a missing header into an error" || bad "compiler: --strict-headers did not refuse"
"$CLING" --facts "$TMP/h1.clfacts" --strict-headers "$TMP/h1.clg" --cpp "$TMP/h1d.cpp" >/dev/null 2>&1; [ $? -eq 2 ] && ok "compiler: --strict-headers works placed after --facts too" || bad "compiler: --strict-headers order matters"
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

# 6. a TEMPLATE type from a real library: Eigen::VectorXd (Matrix<double,-1,1>), its public base classes read too
EIGEN_DIR="${EIGEN_DIR:-/c/project/eigen}"
if [ -f "$EIGEN_DIR/Eigen/Dense" ]; then
    scan --header "$EIGEN_DIR/Eigen/Dense" --type Eigen::VectorXd -I "$EIGEN_DIR" --out "$TMP/vx.clfacts" >/dev/null && ok "eigen: Eigen::VectorXd scanned ($(grep -cE '^(fresh|borrow|mutator|function) ' "$TMP/vx.clfacts") facts, $(grep -c '^@header' "$TMP/vx.clfacts") headers bound)" || bad "eigen: scan failed"
    for want in "fresh Eigen::VectorXd::sum" "fresh Eigen::VectorXd::size" "mutator Eigen::VectorXd::fill" "function Eigen::VectorXd::Ones"; do
        grep -q "^$want " "$TMP/vx.clfacts" && ok "eigen: $want" || bad "eigen: missing $want"
    done
    grep -qE '^(fresh|borrow|mutator|function) Eigen::VectorXd::(head|transpose|block) ' "$TMP/vx.clfacts" && bad "eigen: a view-returning method got a fact" || ok "eigen: head, transpose, block (expression templates) are left conservative"
    cd "$HERE"
    if "$CLING" --facts "$TMP/vx.clfacts" eigen_vec_use.clg --cpp "$TMP/ev.cpp" >/dev/null 2>&1 &&
       "$CXX" -std=c++17 -I"$EIGEN_DIR" "$TMP/ev.cpp" -o "$TMP/ev.exe" 2>"$TMP/ev.err" && [ "$("$TMP/ev.exe")" = "8 4 16 4" ]; then
        ok "eigen: a CeeLing program using the scanned Eigen pack builds against the real Eigen headers and runs"
    else bad "eigen: use program did not build/run: $(head -3 "$TMP/ev.err" 2>/dev/null)"; fi
    out="$("$CLING" --facts "$TMP/vx.clfacts" eigen_vec_dangling_rejected.clg --cpp "$TMP/x.cpp" 2>&1)"
    [ $? -ne 0 ] && echo "$out" | grep -q "dangling observer" && ok "eigen: the lazy view dies when its owner is changed by a proven mutator" || bad "eigen dangling not rejected: $out"
    scan --check "$TMP/vx.clfacts" >/dev/null 2>&1; [ $? -eq 0 ] && ok "eigen: --check finds every bound header unchanged" || bad "eigen: --check failed"
else
    echo "SKIP: eigen template trial (no Eigen at $EIGEN_DIR; set EIGEN_DIR)"
fi

# 7. yaml-cpp's YAML::Node (a handle to shared memory) and the comparison of template instantiations
YAML_DIR="${YAML_DIR:-/c/project/yaml-cpp}"
if [ -f "$YAML_DIR/include/yaml-cpp/yaml.h" ]; then
    scan --header "$YAML_DIR/include/yaml-cpp/yaml.h" --class YAML::Node -I "$YAML_DIR/include" --out "$TMP/yn.clfacts" >/dev/null && ok "yaml-cpp: YAML::Node scanned ($(grep -cE '^(fresh|borrow|mutator|function) ' "$TMP/yn.clfacts") facts)" || bad "yaml-cpp: scan failed"
    grep -q "^fresh YAML::Node::size " "$TMP/yn.clfacts" && ok "yaml-cpp: std::size_t is recognised as a number (size() is fresh)" || bad "yaml-cpp: size() has no fact (std::size_t not recognised)"
    grep -q "^borrow YAML::Node::Scalar " "$TMP/yn.clfacts" && ok "yaml-cpp: Scalar() returns a reference: borrow" || bad "yaml-cpp: Scalar() is not a borrow"
    grep -qE "^(fresh|borrow|mutator) YAML::Node::(reset|push_back|remove) " "$TMP/yn.clfacts" && bad "yaml-cpp: a method taking a Node got a fact" || ok "yaml-cpp: methods that take a Node (reset, push_back, remove) stay conservative"
fi
if [ -f "$EIGEN_DIR/Eigen/Dense" ]; then
    COMPARE="python -I $ROOT/tools/cling-scan/compare_packs.py"
    scan --header "$EIGEN_DIR/Eigen/Dense" --type Eigen::MatrixXd -I "$EIGEN_DIR" --out "$TMP/mx.clfacts" >/dev/null
    scan --header "$EIGEN_DIR/Eigen/Dense" --type Eigen::Matrix3f -I "$EIGEN_DIR" --out "$TMP/m3.clfacts" >/dev/null
    $COMPARE "$TMP/mx.clfacts" "$TMP/m3.clfacts" >/dev/null && ok "compare_packs: MatrixXd and Matrix3f have the same verdict for every method" || bad "compare_packs: MatrixXd and Matrix3f differ"
    out="$($COMPARE "$TMP/vx.clfacts" "$TMP/mx.clfacts")"; code=$?
    [ $code -eq 1 ] && echo "$out" | grep -q "DIFFER cbegin" && ok "compare_packs: VectorXd and MatrixXd DIFFER (cbegin/cend): a pack for one instantiation is not a pack for another" || bad "compare_packs did not find the VectorXd/MatrixXd difference: $out"
fi

exit $fail
