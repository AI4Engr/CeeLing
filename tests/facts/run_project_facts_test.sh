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


# --- opaque classes: `opaque Name` = trusted, nobody wrote method facts (the conservative model) ---------------------------
OP="opaque.clfacts"
expect_reject() {   # <pack> <file> <needle> <description>
    out="$("$CLING" --facts "$1" "$2" --cpp "$TMP/x.cpp" 2>&1)"
    if [ $? -ne 0 ] && echo "$out" | grep -q -- "$3"; then ok "$4"; else bad "$4 -- got: $out"; fi
}

# an opaque class: every method is callable, plain numbers need no facts, the program builds and runs
if "$CLING" --facts $OP opaque_ok.clg --cpp "$TMP/o.cpp" >/dev/null 2>&1 && "$CXX" -std=c++17 -I"$LIB" "$TMP/o.cpp" -o "$TMP/o.exe" 2>/dev/null && [ "$("$TMP/o.exe")" = "3 6" ]; then
    ok "opaque: a class with no method facts can be used and runs"
else bad "opaque: ok program did not build/run"; fi
expect_reject $OP opaque_window_dangles_rejected.clg "dangling observer" "opaque: an unlisted method's result is assumed to borrow the receiver"
expect_reject $OP opaque_unknown_call_kills_rejected.clg "dangling observer" "opaque: an unlisted method is assumed to change the receiver and kills its observers"
expect_reject $OP opaque_refined_ok.clg "dangling observer" "opaque: without a fresh fact, even width() is assumed to disturb the window (conservative)"
if "$CLING" --facts opaque_refined.clfacts opaque_refined_ok.clg --cpp "$TMP/r.cpp" >/dev/null 2>&1; then ok "opaque: fresh facts refine the default (the same program is accepted)"; else bad "opaque: refined pack should accept"; fi
expect_reject $OP opaque_field_borrow_rejected.clg "which is a field" "opaque: a result borrowed from a field receiver is refused, not silently untracked"
expect_reject $OP opaque_two_owners_rejected.clg "may borrow" "opaque: a result that may borrow the receiver and an argument is refused"
expect_reject $OP opaque_retain_rejected.clg "may keep a reference" "opaque: an unlisted method handed an object may retain it, which nothing follows: refused"
expect_reject $OP opaque_assign_borrow_rejected.clg "dangling observer" "opaque: assigning an unlisted method's result makes the target an observer"
printf 'int main() { Grid a = new Grid(1, 1); Grid b = new Grid(1, 1); Grid c = a + b; return 0; }
' > "$TMP/op.clg"
sed -i '1i #include "grid.h"' "$TMP/op.clg"
expect_reject $OP "$TMP/op.clg" "unknown operator" "opaque: operators still need an operator fact"
out="$("$CLING" --facts $OP --report-facts opaque_ok.clg --cpp "$TMP/x.cpp" 2>&1)"
if echo "$out" | grep -q "opaque Grid  \[grid-opaque, declared, review=hand\]"; then ok "opaque: --report-facts names the opaque class"; else bad "report lacks the opaque fact: $out"; fi


# --- a real third-party library under the opaque model: Eigen::VectorXd with a two-line pack -----------------------------
# Needs the Eigen headers: set EIGEN_DIR (the directory that contains Eigen/Dense); skipped otherwise.
EIGEN_DIR="${EIGEN_DIR:-/c/project/eigen}"
if [ -f "$EIGEN_DIR/Eigen/Dense" ]; then
    cd "$HERE/eigen_trial"
    if "$CLING" --facts vec.clfacts use.clg --cpp "$TMP/eu.cpp" >/dev/null 2>&1 && "$CXX" -std=c++17 -I"$EIGEN_DIR" "$TMP/eu.cpp" -o "$TMP/eu.exe" 2>"$TMP/eg.err" && [ "$("$TMP/eu.exe")" = "4 4 1" ]; then
        ok "eigen: real Eigen::VectorXd used from CeeLing with a two-line opaque pack, built against the real headers, ran"
    else bad "eigen: use.clg did not build/run: $(head -3 "$TMP/eg.err" 2>/dev/null)"; fi
    expect_reject vec.clfacts dangle.clg "dangling observer" "eigen: a block of a real Eigen vector is tracked as a borrow of its owner"
    cd "$LIB"
else
    echo "SKIP: eigen trial (no Eigen headers at $EIGEN_DIR; set EIGEN_DIR)"
fi

exit $fail
