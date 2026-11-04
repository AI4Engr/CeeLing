#!/usr/bin/env bash
# tools/fact-gauge/probe_pack.py on a SCANNED pack: is each fact true of the real library?
# Scans Eigen::VectorXd, then (a) judges every fact with the real compiler and (b) builds the same probe as plain C++ with a lazy view under
# AddressSanitizer in WSL. Needs: clang++ (scan), WSL with g++ (sanitiser), Eigen headers. Skipped when one is missing.
#
#   usage: run_probe_test.sh <path-to-cling> <path-to-clang++>
set -u
CLING="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
CLANG="$2"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
EIGEN_DIR="${EIGEN_DIR:-/c/project/eigen}"
WSL_DISTRO="${WSL_DISTRO:-Ubuntu-22.04}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fail=0
ok()  { echo "PASS: $1"; }
bad() { echo "FAIL: $1"; fail=1; }

[ -f "$EIGEN_DIR/Eigen/Dense" ] || { echo "SKIP: no Eigen at $EIGEN_DIR (set EIGEN_DIR)"; exit 0; }
wsl -d "$WSL_DISTRO" -- bash -c 'command -v g++ >/dev/null' >/dev/null 2>&1 || { echo "SKIP: no g++ in WSL distro $WSL_DISTRO (set WSL_DISTRO)"; exit 0; }

python -I "$ROOT/tools/cling-scan/cling_scan.py" --clang "$CLANG" --header "$EIGEN_DIR/Eigen/Dense" --type Eigen::VectorXd -I "$EIGEN_DIR" --out "$TMP/vx.clfacts" >/dev/null || bad "scan failed"
probe() { python -I "$ROOT/tools/fact-gauge/probe_pack.py" --cling "$CLING" --pack "$TMP/vx.clfacts" --conf "$HERE/probe/eigen_vectorxd.conf" -I "$EIGEN_DIR" "$@" 2>&1; }

out="$(probe)"; code=$?
if [ $code -eq 0 ] && echo "$out" | grep -q "OK (sanitiser clean)=" && echo "$out" | grep -q "really invalidates"; then
    ok "the scanned Eigen pack agrees with the compiler and with the real library: $(echo "$out" | head -1)"
else bad "probe of the scanned pack failed (exit $code): $out"; fi

# negative control: declare the methods that really reallocate to be `fresh`; the probe must catch it
out="$(probe --pretend-fresh resize conservativeResize)"; code=$?
if [ $code -ne 0 ] && echo "$out" | grep -q "BEHAVIOUR CONTRADICTS FACT: Eigen::VectorXd::resize (fresh) -- heap-use-after-free"; then
    ok "negative control: a wrong fact (resize declared fresh) is caught by AddressSanitizer"
else bad "negative control did not fail as it must (exit $code): $out"; fi

# --- a second library of a different shape: jsoncpp's Json::Value (a tree with deep copies, not a flat numeric array) ---------------------------------
JSONCPP_DIR="${JSONCPP_DIR:-/c/project/jsoncpp-master/jsoncpp-master}"
if [ -f "$JSONCPP_DIR/include/json/json.h" ]; then
    python -I "$ROOT/tools/cling-scan/cling_scan.py" --clang "$CLANG" --header "$JSONCPP_DIR/include/json/json.h" --class Json::Value -I "$JSONCPP_DIR/include" --out "$TMP/jv.clfacts" >/dev/null || bad "jsoncpp scan failed"
    jprobe() { python -I "$ROOT/tools/fact-gauge/probe_pack.py" --cling "$CLING" --pack "$TMP/jv.clfacts" --conf "$HERE/probe/jsoncpp_value.conf" -I "$JSONCPP_DIR/include" --var "JSON=$JSONCPP_DIR" "$@" 2>&1; }
    out="$(jprobe)"; code=$?
    if [ $code -eq 0 ] && echo "$out" | grep -q "OK (sanitiser clean)=" && echo "$out" | grep -q "really invalidates"; then ok "the scanned jsoncpp pack agrees with the compiler and the real library: $(echo "$out" | head -1)"
    else bad "jsoncpp probe failed (exit $code): $out"; fi
    out="$(jprobe --pretend-fresh clear)"; code=$?
    if [ $code -ne 0 ] && echo "$out" | grep -q "BEHAVIOUR CONTRADICTS FACT: Json::Value::clear (fresh) -- heap-use-after-free"; then ok "negative control: Json::Value::clear declared fresh is caught by AddressSanitizer"
    else bad "jsoncpp negative control did not fail as it must (exit $code): $out"; fi
else
    echo "SKIP: jsoncpp probe (no jsoncpp at $JSONCPP_DIR; set JSONCPP_DIR)"
fi

# --- a third shape: yaml-cpp's YAML::Node, a handle to memory other handles share; the view is a raw pointer into its tag buffer --------------------
YAML_DIR="${YAML_DIR:-/c/project/yaml-cpp}"
if [ -f "$YAML_DIR/include/yaml-cpp/yaml.h" ]; then
    python -I "$ROOT/tools/cling-scan/cling_scan.py" --clang "$CLANG" --header "$YAML_DIR/include/yaml-cpp/yaml.h" --class YAML::Node -I "$YAML_DIR/include" --out "$TMP/yn.clfacts" >/dev/null || bad "yaml-cpp scan failed"
    yprobe() { python -I "$ROOT/tools/fact-gauge/probe_pack.py" --cling "$CLING" --pack "$TMP/yn.clfacts" --conf "$HERE/probe/yamlcpp_node.conf" -I "$YAML_DIR/include" -I "$YAML_DIR/src" --var "YAML=$YAML_DIR" "$@" 2>&1; }
    out="$(yprobe)"; code=$?
    if [ $code -eq 0 ] && echo "$out" | grep -q "OK (sanitiser clean)="; then ok "the scanned yaml-cpp pack agrees with the real library: $(echo "$out" | head -1)"
    else bad "yaml-cpp probe failed (exit $code): $out"; fi
    out="$(yprobe --pretend-fresh SetTag)"; code=$?
    if [ $code -ne 0 ] && echo "$out" | grep -q "BEHAVIOUR CONTRADICTS FACT: YAML::Node::SetTag (fresh) -- heap-use-after-free"; then ok "negative control: YAML::Node::SetTag declared fresh is caught (it frees the tag buffer)"
    else bad "yaml-cpp negative control did not fail as it must (exit $code): $out"; fi
else
    echo "SKIP: yaml-cpp probe (no yaml-cpp at $YAML_DIR; set YAML_DIR)"
fi

exit $fail

