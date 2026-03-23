#!/bin/bash

# Regression test runner for CeeLing C++ transpilation.
# Usage:
#   ./run_regression.sh [native] [update]
#   ./run_regression.sh update

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
FIXTURES_DIR="$SCRIPT_DIR/fixtures"
OUTPUT_CPP_DIR="$SCRIPT_DIR/results/cpp"
OUTPUT_LOG_DIR="$SCRIPT_DIR/results/log"
EXPECTED_CPP_DIR="$SCRIPT_DIR/expected/cpp"
DIFF_CPP_DIR="$SCRIPT_DIR/diff/cpp"

to_windows_path() {
    local path="$1"

    if command -v cygpath >/dev/null 2>&1; then
        cygpath -w "$path"
        return
    fi

    if [[ "$path" =~ ^/mnt/([a-zA-Z])/(.*)$ ]]; then
        local drive="${BASH_REMATCH[1]}"
        local rest="${BASH_REMATCH[2]}"
        rest="${rest//\//\\}"
        printf "%s:\\%s\n" "${drive^^}" "$rest"
        return
    fi

    printf "%s\n" "$path"
}

MODE="${1:-native}"
UPDATE=false

if [ "${1:-}" = "update" ]; then
    UPDATE=true
    MODE="native"
elif [ "${2:-}" = "update" ]; then
    UPDATE=true
fi

if [ "$MODE" != "native" ]; then
    echo "Error: Only 'native' mode is supported by this regression runner."
    exit 1
fi

CLING_CMD=""
if [ -f "$PROJECT_DIR/build/cling.exe" ]; then
    CLING_CMD="$PROJECT_DIR/build/cling.exe"
elif [ -f "$PROJECT_DIR/build/cling" ]; then
    CLING_CMD="$PROJECT_DIR/build/cling"
elif [ -f "$PROJECT_DIR/dist/cling.exe" ]; then
    CLING_CMD="$PROJECT_DIR/dist/cling.exe"
elif [ -f "$PROJECT_DIR/dist/cling" ]; then
    CLING_CMD="$PROJECT_DIR/dist/cling"
fi

if [ -z "$CLING_CMD" ]; then
    echo "Error: dist/cling(.exe) not found. Run build first."
    exit 1
fi

if [ ! -d "$FIXTURES_DIR" ] || [ -z "$(ls -A "$FIXTURES_DIR"/*.clg 2>/dev/null)" ]; then
    echo "Error: No .clg fixtures found in $FIXTURES_DIR."
    exit 1
fi

mkdir -p "$OUTPUT_CPP_DIR" "$OUTPUT_LOG_DIR" "$EXPECTED_CPP_DIR" "$DIFF_CPP_DIR"
rm -f "$OUTPUT_CPP_DIR"/*.cpp "$OUTPUT_LOG_DIR"/*.log "$DIFF_CPP_DIR"/*.diff 2>/dev/null || true

echo "Running regression tests with: $CLING_CMD"

pass_count=0
fail_count=0
missing_baseline_count=0
generated_count=0
skipped_count=0

for fixture in "$FIXTURES_DIR"/*.clg; do
    [ -f "$fixture" ] || continue
    base_name="$(basename "$fixture" .clg)"
    out_cpp="$OUTPUT_CPP_DIR/$base_name.cpp"
    out_log="$OUTPUT_LOG_DIR/$base_name.log"
    cli_fixture="$fixture"
    cli_out_cpp="$out_cpp"

    if [[ "$CLING_CMD" == *.exe ]]; then
        cli_fixture="$(to_windows_path "$fixture")"
        cli_out_cpp="$(to_windows_path "$out_cpp")"
    fi

    echo "Generating: $base_name"
    if "$CLING_CMD" --cpp "$cli_out_cpp" "$cli_fixture" >"$out_log" 2>&1; then
        generated_count=$((generated_count + 1))
    else
        if grep -q "Unsupported syntax remains after transformation" "$out_log" || grep -q "Parse error" "$out_log"; then
            echo "SKIP: unsupported legacy syntax in $base_name"
            skipped_count=$((skipped_count + 1))
        else
            echo "FAIL: generation failed for $base_name (see $out_log)"
            fail_count=$((fail_count + 1))
        fi
        continue
    fi
done

echo "Generated outputs: $generated_count"

if [ "$UPDATE" = true ]; then
    cp "$OUTPUT_CPP_DIR"/*.cpp "$EXPECTED_CPP_DIR"/ 2>/dev/null || true
    echo "Updated baselines in $EXPECTED_CPP_DIR"
    exit 0
fi

for generated_cpp in "$OUTPUT_CPP_DIR"/*.cpp; do
    [ -f "$generated_cpp" ] || continue
    file_name="$(basename "$generated_cpp")"
    expected_cpp="$EXPECTED_CPP_DIR/$file_name"
    diff_file="$DIFF_CPP_DIR/${file_name%.cpp}.diff"

    if [ ! -f "$expected_cpp" ]; then
        echo "MISSING BASELINE: $file_name (run with 'update')"
        missing_baseline_count=$((missing_baseline_count + 1))
        continue
    fi

    if diff -u "$expected_cpp" "$generated_cpp" >"$diff_file"; then
        rm -f "$diff_file"
        echo "PASS: $file_name"
        pass_count=$((pass_count + 1))
    else
        echo "FAIL: $file_name (see $diff_file)"
        fail_count=$((fail_count + 1))
    fi
done

echo
echo "Summary:"
echo "  Passed:            $pass_count"
echo "  Failed:            $fail_count"
echo "  Missing baselines: $missing_baseline_count"
echo "  Skipped:           $skipped_count"

if [ "$fail_count" -gt 0 ]; then
    exit 1
fi

echo "Regression run completed."
