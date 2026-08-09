#!/usr/bin/env bash
#
# CeeLing safety-contract runner (Phase 0 harness).
#
# For every fixture tagged with `//@ expect: reject` or `//@ expect: accept`, run the
# compiler and compare the exit code to the expectation:
#   - expect: accept  -> compiler must exit 0
#   - expect: reject  -> compiler must exit non-zero (it refused unsafe code)
#
# Because the dangling/use-after-move analysis is NOT yet implemented, every
# `expect: reject` fixture currently passes through (exit 0) -- a known false-negative.
# Such a mismatch is reported as XFAIL (expected-to-fail), not FAIL, so the baseline is
# green. As Phase 2+ lands, each fixture flips from XFAIL to real PASS; a reject fixture
# that starts actually failing (i.e. gets rejected) becomes XPASS -> retag it.
#
# Usage: run_safety_tests.sh [--strict] [path-to-cling]
#   --strict : treat XFAIL as failure (use once the analysis is implemented)
#
# Exit code: 0 if no hard failures, 1 otherwise.

set -u

STRICT=0
CLING=""
for arg in "$@"; do
    case "$arg" in
        --strict) STRICT=1 ;;
        *) CLING="$arg" ;;
    esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Locate the compiler if not given explicitly.
if [ -z "$CLING" ]; then
    for cand in "$REPO_ROOT/build/cling.exe" "$REPO_ROOT/build/cling" \
                "$REPO_ROOT/dist/cling.exe" "$REPO_ROOT/dist/cling"; do
        if [ -x "$cand" ]; then CLING="$cand"; break; fi
    done
fi
if [ -z "$CLING" ] || [ ! -x "$CLING" ]; then
    echo "ERROR: could not find cling executable (build first, or pass its path)."
    exit 1
fi

TMP_OUT="$(mktemp -d)"
trap 'rm -rf "$TMP_OUT"' EXIT

# A real C++ compiler, used to gate every accept fixture's OUTPUT (see the compile
# check below). If none is available the suite still runs -- it just cannot make the
# stronger claim, and says so, rather than silently downgrading to exit-code-only.
CXX="${CXX:-g++}"
CXX_AVAILABLE=0
if command -v "$CXX" >/dev/null 2>&1; then
    CXX_AVAILABLE=1
fi

pass=0 xfail=0 fail=0 xpass=0

# Collect fixtures: the new-syntax safety suite plus any tagged legacy fixtures.
mapfile -t FIXTURES < <(find "$SCRIPT_DIR" "$REPO_ROOT/tests" -maxdepth 1 -name '*.clg' 2>/dev/null | sort -u)

printf '%-48s %-8s %s\n' "FIXTURE" "EXPECT" "RESULT"
printf '%-48s %-8s %s\n' "------------------------------------------------" "--------" "------"

for fx in "${FIXTURES[@]}"; do
    expect="$(grep -m1 -oE '//@ expect:[[:space:]]*(reject|accept)' "$fx" | grep -oE '(reject|accept)$')"
    [ -z "$expect" ] && continue   # untagged fixture: skip

    name="$(basename "$fx")"
    "$CLING" "$fx" --cpp "$TMP_OUT/${name%.clg}.cpp" >/dev/null 2>&1
    code=$?
    if [ "$code" -eq 0 ]; then actual="accept"; else actual="reject"; fi

    if [ "$actual" = "$expect" ]; then
        # An accepted fixture is only really passing if the C++ it produced is
        # legal C++. `cling` exiting 0 proves the ANALYSIS accepted the program; it
        # says nothing about whether the emitter produced something a compiler will
        # take. Those are different claims, and the gap between them is where the
        # `throw "literal"` bug lived (compiled fine, std::terminate'd at runtime).
        if [ "$expect" = "accept" ] && [ "$CXX_AVAILABLE" -eq 1 ]; then
            gen="$TMP_OUT/${name%.clg}.cpp"
            exe="$TMP_OUT/${name%.clg}.exe"

            # A fixture may declare the output its compiled program must print:
            #   //@ run: woof
            # Use this whenever the claim is a RUNTIME property -- virtual dispatch,
            # exception type matching, shared_ptr aliasing. -fsyntax-only cannot
            # distinguish "dispatches to the override" from "silently calls the base".
            want_run="$(grep -m1 -oE '//@ run:.*' "$fx" | sed -E 's|^//@ run:[[:space:]]*||')"

            if [ -n "$want_run" ]; then
                if ! "$CXX" -std=c++17 "$gen" -o "$exe" >/dev/null 2>&1; then
                    printf '%-48s %-8s %s\n' "$name" "$expect" "FAIL (generated C++ does not compile)"
                    fail=$((fail+1)); continue
                fi
                got_run="$("$exe" 2>/dev/null)"
                if [ "$got_run" != "$want_run" ]; then
                    printf '%-48s %-8s %s\n' "$name" "$expect" \
                        "FAIL (ran, expected '$want_run', got '$got_run')"
                    fail=$((fail+1)); continue
                fi
            elif ! "$CXX" -std=c++17 -fsyntax-only "$gen" >/dev/null 2>&1; then
                printf '%-48s %-8s %s\n' "$name" "$expect" "FAIL (generated C++ does not compile)"
                fail=$((fail+1)); continue
            fi
        fi
        printf '%-48s %-8s %s\n' "$name" "$expect" "PASS"
        pass=$((pass+1))
    else
        # Mismatch. A missed reject (wanted reject, got accept) is a KNOWN gap until
        # the analysis exists -> XFAIL. A wanted-accept that got rejected is a hard FAIL
        # (over-rejection / regression) regardless of strict mode.
        if [ "$expect" = "reject" ] && [ "$actual" = "accept" ]; then
            if [ "$STRICT" -eq 1 ]; then
                printf '%-48s %-8s %s\n' "$name" "$expect" "FAIL (missed reject)"
                fail=$((fail+1))
            else
                printf '%-48s %-8s %s\n' "$name" "$expect" "XFAIL (analysis not yet implemented)"
                xfail=$((xfail+1))
            fi
        else
            printf '%-48s %-8s %s\n' "$name" "$expect" "FAIL (rejected a valid program)"
            fail=$((fail+1))
        fi
    fi
done

echo
echo "Summary: PASS=$pass  XFAIL=$xfail  FAIL=$fail"
if [ "$CXX_AVAILABLE" -eq 1 ]; then
    echo "Compile gate: ON  (every accept fixture's generated C++ compiled with $CXX;"
    echo "                   fixtures tagged '//@ run:' were executed and their output asserted)"
else
    echo "Compile gate: OFF (no '$CXX' on PATH -- fixtures were only checked for cling's"
    echo "                   exit code, NOT that the C++ they generated is even legal)"
fi
if [ "$fail" -gt 0 ]; then
    echo "RESULT: FAIL"
    exit 1
fi
echo "RESULT: OK (baseline green; $xfail known gap(s) awaiting Phase 2+)"
exit 0
