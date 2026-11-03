#!/usr/bin/env bash
# `cling --facts pack.clfacts`: a project can teach the compiler about its own C++ library without editing the compiler.
# tests/facts/private_lib/ holds a "private library" (grid.h) and the facts its users wrote (grid.clfacts).
#
#   usage: run_project_facts_test.sh <path-to-cling>
set -u
CLING="$(cd "$(dirname "${1:-./build/cling.exe}")" && pwd)/$(basename "${1:-./build/cling.exe}")"
HERE="$(cd "$(dirname "$0")" && pwd)"
LIB="$HERE/private_lib"
CXX="${CXX:-g++}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fail=0
ok()   { echo "PASS: $1"; }
bad()  { echo "FAIL: $1"; fail=1; }

cd "$LIB"

# 1. with the pack: accepted, the generated C++ compiles against the library header and prints the right numbers
if "$CLING" --facts grid.clfacts ok.clg --cpp "$TMP/ok.cpp" >/dev/null 2>&1; then
    if "$CXX" -std=c++17 -I"$LIB" "$TMP/ok.cpp" -o "$TMP/ok.exe" 2>"$TMP/gpp.err" && [ "$("$TMP/ok.exe")" = "3 2 6 2" ]; then
        ok "a program using a private C++ class compiles, builds and runs"
    else
        bad "generated C++ did not build or print '3 2 6 2': $(head -3 "$TMP/gpp.err")"
    fi
else
    bad "the program was rejected although the pack declares everything it uses"
fi

# 2. the same program without the pack is rejected: the compiler knows nothing about Grid
if "$CLING" ok.clg --cpp "$TMP/x.cpp" >/dev/null 2>&1; then bad "accepted without the pack"; else ok "without the pack the same program is rejected"; fi

# 3. the borrow fact is enforced: window() borrows its receiver, so replacing the receiver kills the window
out="$("$CLING" --facts grid.clfacts dangling_rejected.clg --cpp "$TMP/x.cpp" 2>&1)"
if [ $? -ne 0 ] && echo "$out" | grep -q "dangling observer"; then ok "a borrow fact is enforced (dangling window rejected)"; else bad "dangling window not rejected: $out"; fi

# 4. a method the pack does not list is rejected
out="$("$CLING" --facts grid.clfacts no_fact_rejected.clg --cpp "$TMP/x.cpp" 2>&1)"
if [ $? -ne 0 ] && echo "$out" | grep -q "unknown function/method \`shrink\`"; then ok "an unlisted method is rejected"; else bad "unlisted method not rejected: $out"; fi

# 5. --report-facts names every fact relied on, with its source
out="$("$CLING" --facts grid.clfacts --report-facts ok.clg --cpp "$TMP/x.cpp" 2>&1)"
if echo "$out" | grep -q "fresh Grid::sum  \[grid, declared, review=hand\]" && echo "$out" | grep -q "borrow Grid::window"; then ok "--report-facts lists the facts the program relied on"; else bad "report incomplete: $out"; fi

# 6. a project pack cannot change how std is judged
printf '@pack evil\n@version 1\n@origin test\nvector.sig begin/nonconst/- result=void mutates=no source=declared\n' > "$TMP/evil.clfacts"
out="$("$CLING" --facts "$TMP/evil.clfacts" ok.clg --cpp "$TMP/x.cpp" 2>&1)"; code=$?
if [ $code -eq 2 ] && echo "$out" | grep -q "project fact pack may only hold"; then ok "a project pack cannot add std signature facts"; else bad "std kind accepted in a project pack (exit $code): $out"; fi

# 7. ... nor repeat a fact the compiler ships (it can add, never replace)
printf '@pack dup\n@version 1\n@origin test\nfunction std::stoi source=declared\n' > "$TMP/dup.clfacts"
out="$("$CLING" --facts "$TMP/dup.clfacts" ok.clg --cpp "$TMP/x.cpp" 2>&1)"; code=$?
if [ $code -eq 2 ] && echo "$out" | grep -q "duplicate"; then ok "a project pack cannot repeat a shipped fact"; else bad "duplicate accepted (exit $code): $out"; fi

# 8. a malformed pack and a missing file are errors, not silently ignored
printf '@pack bad\n@version 1\n@origin test\nfunction f\n' > "$TMP/bad.clfacts"
"$CLING" --facts "$TMP/bad.clfacts" ok.clg --cpp "$TMP/x.cpp" >/dev/null 2>&1; [ $? -eq 2 ] && ok "a malformed pack is an error" || bad "malformed pack accepted"
"$CLING" --facts "$TMP/missing.clfacts" ok.clg --cpp "$TMP/x.cpp" >/dev/null 2>&1; [ $? -eq 2 ] && ok "a missing pack file is an error" || bad "missing pack accepted"

exit $fail
