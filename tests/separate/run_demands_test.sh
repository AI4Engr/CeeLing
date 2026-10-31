#!/usr/bin/env bash
# --ptr-demands / --ptr-demands-out: separate compilation where the file that USES a class needs a pointer kind the file that DECLARES it would not choose.
#   tests/separate/run_demands_test.sh <path-to-cling>
# demand_lib.clg declares Bag (a vector of Item); demand_use.clg copies an element out of it twice, which needs shared_ptr elements.
#   1. without demands the library chooses unique_ptr and the user file does not compile with it  (the problem; if this step starts to compile, the test proves nothing)
#   2. the user file writes what it needs (shared elements items); with that read by both files the pair compiles with g++ and the field is shared
#   3. a demand can only make a member shared: demands that name nothing in the library change nothing
set -uo pipefail
CLING="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"     # (absolute: the scripts change directory)
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
cp "$HERE"/demand_lib.clg "$HERE"/demand_use.clg "$W/"
cd "$W"
quiet() { "$@" 2>&1 | grep -v Detected | grep -v 'generated successfully' || true; }
fail() { echo "FAIL: $1"; exit 1; }
gen() { # extra flags...  : (re)generate both files
    "$CLING" "$@" --cpp demand_lib.cpp demand_lib.clg > /dev/null 2>&1 || fail "library did not transpile"
    "$CLING" "$@" --cpp demand_lib.h demand_lib.clg > /dev/null 2>&1 || fail "library header did not transpile"
    "$CLING" "$@" --cpp demand_use.cpp demand_use.clg > /dev/null 2>&1 || fail "user file did not transpile"
}
gen
grep -q 'std::vector<std::unique_ptr<Item>> items' demand_lib.h || fail "step 1: expected unique elements without demands"
if g++ --std=c++17 -fsyntax-only demand_use.cpp demand_lib.cpp > /dev/null 2>&1; then fail "step 1: the pair compiles without demands, so this test no longer shows the problem"; fi

"$CLING" --ptr-demands-out use.demands --cpp use.probe.cpp demand_use.clg > /dev/null 2>&1 || fail "step 2: writing demands failed"
grep -qx 'shared elements items' use.demands || fail "step 2: the user file did not ask for shared elements: $(cat use.demands)"
gen --ptr-demands use.demands
grep -q 'std::vector<std::shared_ptr<Item>> items' demand_lib.h || fail "step 2: the library did not follow the demand"
g++ --std=c++17 -fsyntax-only demand_use.cpp demand_lib.cpp || fail "step 2: the pair does not compile with demands"

echo 'shared field Nothing.here' > none.demands
echo 'shared elements nothing' >> none.demands
gen --ptr-demands none.demands
grep -q 'std::vector<std::unique_ptr<Item>> items' demand_lib.h || fail "step 3: a demand that names nothing changed the library"
echo "ok: pointer demands make a separately compiled pair fit"
