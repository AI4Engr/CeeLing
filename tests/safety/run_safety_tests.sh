#!/usr/bin/env bash
#
# CeeLing safety-contract runner (Phase 0 harness).
#
# For every fixture tagged with `//@ expect: reject` or `//@ expect: accept`, run the
# compiler and compare the exit code to the expectation:
#   - expect: accept  -> compiler must exit 0
#   - expect: reject  -> compiler must exit non-zero (it refused unsafe code)
#   - an accept fixture may add `//@ run: <stdout>` (program must print exactly this) or
#     `//@ run-fail: <text>` (program must exit non-zero with <text> on stderr)
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
#
# Parallelism note: each fixture is fully independent (its own cling invocation,
# its own temp .cpp/.exe, its own g++ call) -- there is no shared mutable state
# between them except the final pass/xfail/fail tally, which is why this runs
# one background job per fixture and collects each job's one-line verdict from
# its own result file afterward, rather than incrementing shared counters from
# parallel subshells (which would race). This cut real time from ~30s to a few
# seconds on a 32-core machine (previously fully serial: one cling + one g++
# process-start per fixture, dominated by process-launch overhead, not CPU --
# confirmed by `user` time being ~4s out of ~30s real time before this change).

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

# Collect fixtures: the new-syntax safety suite plus any tagged legacy fixtures.
mapfile -t FIXTURES < <(find "$SCRIPT_DIR" "$REPO_ROOT/tests" -maxdepth 1 -name '*.clg' 2>/dev/null | sort -u)

# One job per fixture, run in the background, capped at $JOBS concurrent --
# each job writes its OWN single-line verdict to $TMP_OUT/results/<name>.status
# ("PASS"/"XFAIL"/"FAIL: <reason>") plus its full report line to
# $TMP_OUT/results/<name>.report, so the parent can print in stable (sorted)
# order afterward regardless of which job finished first.
mkdir -p "$TMP_OUT/results"
JOBS="${SAFETY_TEST_JOBS:-$( (command -v nproc >/dev/null 2>&1 && nproc) || echo "${NUMBER_OF_PROCESSORS:-4}")}"

run_one_fixture() {
    local fx="$1"
    local expect name gen exe report_file status_file
    expect="$(grep -m1 -oE '//@ expect:[[:space:]]*(reject|accept)' "$fx" | grep -oE '(reject|accept)$')"
    name="$(basename "$fx")"
    report_file="$TMP_OUT/results/$name.report"
    status_file="$TMP_OUT/results/$name.status"
    [ -z "$expect" ] && return 0   # untagged fixture: skip (no report/status file)

    gen="$TMP_OUT/${name%.clg}.cpp"
    exe="$TMP_OUT/${name%.clg}.exe"

    # optional per-fixture compiler flags: `//@ flags: --pool-shared`
    fx_flags="$(grep -m1 '^//@ flags:' "$fx" | sed 's|^//@ flags: *||' | tr -d '')"
    "$CLING" $fx_flags "$fx" --cpp "$gen" >/dev/null 2>&1
    code=$?
    # A crash of the compiler itself (signal: exit code >= 128) is neither an accept nor a reject. It used
    # to count as a "reject" and could silently satisfy an `expect: reject` fixture.
    if [ "$code" -ge 128 ]; then
        printf '%-48s %-8s %s\n' "$name" "$expect" "FAIL (compiler crashed, exit $code)" > "$report_file"
        echo FAIL > "$status_file"
        return 0
    fi
    if [ "$code" -eq 0 ]; then actual="accept"; else actual="reject"; fi

    if [ "$actual" = "$expect" ]; then
        # An accepted fixture is only really passing if the C++ it produced is
        # legal C++. `cling` exiting 0 proves the ANALYSIS accepted the program; it
        # says nothing about whether the emitter produced something a compiler will
        # take. Those are different claims, and the gap between them is where the
        # `throw "literal"` bug lived (compiled fine, std::terminate'd at runtime).
        if [ "$expect" = "accept" ] && [ "$CXX_AVAILABLE" -eq 1 ]; then
            # A fixture may declare the output its compiled program must print:
            #   //@ run: woof
            # Use this whenever the claim is a RUNTIME property -- virtual dispatch,
            # exception type matching, shared_ptr aliasing. -fsyntax-only cannot
            # distinguish "dispatches to the override" from "silently calls the base".
            want_run="$(grep -m1 -oE '//@ run:.*' "$fx" | sed -E 's|^//@ run:[[:space:]]*||')"
            # A fixture may instead declare that the compiled program must DIE at
            # runtime with a diagnostic (a compiler-inserted runtime check firing):
            #   //@ run-fail: out of bounds
            # The program must exit non-zero AND its stderr must contain the text.
            # Exit status alone is not enough -- a segfault is also non-zero, and a
            # segfault is exactly what a runtime check exists to replace.
            want_fail="$(grep -m1 -oE '//@ run-fail:.*' "$fx" | sed -E 's|^//@ run-fail:[[:space:]]*||' | tr -d '\r')"

            if [ -n "$want_fail" ]; then
                # -O2, deliberately: libstdc++ turns its own operator[] assertions ON for unoptimized
                # builds only, so without it a fixture could "pass" because the LIBRARY aborted, not
                # because the check CeeLing inserted fired. Optimized, only our check can stop it.
                if ! "$CXX" -std=c++17 -O2 "$gen" -o "$exe" >/dev/null 2>&1; then
                    printf '%-48s %-8s %s\n' "$name" "$expect" "FAIL (generated C++ does not compile)" > "$report_file"
                    echo "FAIL" > "$status_file"
                    return 0
                fi
                got_err="$("$exe" 2>&1 >/dev/null)"
                got_code=$?
                if [ "$got_code" -eq 0 ]; then
                    printf '%-48s %-8s %s\n' "$name" "$expect" \
                        "FAIL (expected runtime failure '$want_fail', but exited 0)" > "$report_file"
                    echo "FAIL" > "$status_file"
                    return 0
                fi
                case "$got_err" in
                    *"$want_fail"*) ;;
                    *)
                        printf '%-48s %-8s %s\n' "$name" "$expect" \
                            "FAIL (exited $got_code, stderr lacks '$want_fail': '$got_err')" > "$report_file"
                        echo "FAIL" > "$status_file"
                        return 0
                        ;;
                esac
            elif [ -n "$want_run" ]; then
                if ! "$CXX" -std=c++17 "$gen" -o "$exe" >/dev/null 2>&1; then
                    printf '%-48s %-8s %s\n' "$name" "$expect" "FAIL (generated C++ does not compile)" > "$report_file"
                    echo "FAIL" > "$status_file"
                    return 0
                fi
                got_run="$("$exe" 2>/dev/null)"
                if [ "$got_run" != "$want_run" ]; then
                    printf '%-48s %-8s %s\n' "$name" "$expect" \
                        "FAIL (ran, expected '$want_run', got '$got_run')" > "$report_file"
                    echo "FAIL" > "$status_file"
                    return 0
                fi
            elif ! "$CXX" -std=c++17 -fsyntax-only "$gen" >/dev/null 2>&1; then
                printf '%-48s %-8s %s\n' "$name" "$expect" "FAIL (generated C++ does not compile)" > "$report_file"
                echo "FAIL" > "$status_file"
                return 0
            fi
        fi
        printf '%-48s %-8s %s\n' "$name" "$expect" "PASS" > "$report_file"
        echo "PASS" > "$status_file"
    else
        # Mismatch. A missed reject (wanted reject, got accept) is a KNOWN gap until
        # the analysis exists -> XFAIL. A wanted-accept that got rejected is a hard FAIL
        # (over-rejection / regression) regardless of strict mode.
        if [ "$expect" = "reject" ] && [ "$actual" = "accept" ]; then
            if [ "$STRICT" -eq 1 ]; then
                printf '%-48s %-8s %s\n' "$name" "$expect" "FAIL (missed reject)" > "$report_file"
                echo "FAIL" > "$status_file"
            else
                printf '%-48s %-8s %s\n' "$name" "$expect" "XFAIL (analysis not yet implemented)" > "$report_file"
                echo "XFAIL" > "$status_file"
            fi
        else
            printf '%-48s %-8s %s\n' "$name" "$expect" "FAIL (rejected a valid program)" > "$report_file"
            echo "FAIL" > "$status_file"
        fi
    fi
}
export -f run_one_fixture
export TMP_OUT CLING STRICT CXX CXX_AVAILABLE

printf '%s\0' "${FIXTURES[@]}" | xargs -0 -P "$JOBS" -I{} bash -c 'run_one_fixture "$@"' _ {}

printf '%-48s %-8s %s\n' "FIXTURE" "EXPECT" "RESULT"
printf '%-48s %-8s %s\n' "------------------------------------------------" "--------" "------"

pass=0 xfail=0 fail=0
# Print in fixture order (not completion order) for stable, diffable output --
# find sorted the same way FIXTURES was originally collected.
for fx in "${FIXTURES[@]}"; do
    name="$(basename "$fx")"
    status_file="$TMP_OUT/results/$name.status"
    [ -f "$status_file" ] || continue   # untagged fixture: skip, matches original behavior
    cat "$TMP_OUT/results/$name.report"
    case "$(cat "$status_file")" in
        PASS) pass=$((pass+1)) ;;
        XFAIL) xfail=$((xfail+1)) ;;
        FAIL) fail=$((fail+1)) ;;
    esac
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
if [ "$STRICT" -eq 1 ] && [ "$CXX_AVAILABLE" -ne 1 ]; then
    # Without a C++ compiler an accept fixture proves only that cling exited 0, and every '//@ run:'
    # and '//@ run-fail:' assertion is skipped -- a green result would be a false pass.
    echo "RESULT: FAIL (--strict requires the compile gate; put g++ on PATH)"
    exit 1
fi
echo "RESULT: OK (baseline green; $xfail known gap(s) awaiting Phase 2+)"
exit 0
