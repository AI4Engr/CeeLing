#!/bin/bash

# Regression test runner for MADOLA
# Usage: ./run_regression.sh [native|wasm] [update]

set -e

# Configuration
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
FIXTURES_DIR="$SCRIPT_DIR/fixtures"
OUTPUT_DIR="$SCRIPT_DIR/results"
EXPECTED_DIR="$SCRIPT_DIR/expected"
DIFF_DIR="$SCRIPT_DIR/diff"

# Parse arguments
MODE="${1:-native}"

if [ "$1" = "update" ]; then
    UPDATE=true
    MODE=""
else
    UPDATE=false
fi

# Handle update-only mode (no regression run)
if [ "$UPDATE" = true ] && [ -z "$MODE" ]; then
    echo "Updating expected results from existing output..."
    if [ ! -d "$OUTPUT_DIR" ] || [ -z "$(ls -A "$OUTPUT_DIR/markdown" 2>/dev/null)" ]; then
        echo "Error: No output files found. Run regression tests first."
        exit 1
    fi
    cp "$OUTPUT_DIR/markdown"/* "$EXPECTED_DIR/markdown/" 2>/dev/null || true
    cp "$OUTPUT_DIR/evaluation"/* "$EXPECTED_DIR/evaluation/" 2>/dev/null || true
    echo "Expected results updated from output to expected"
    exit 0
fi

echo "Running regression tests in $MODE mode..."
if [ "$UPDATE" = true ]; then
    echo "Will update expected results after running tests"
fi

# Ensure directories exist
mkdir -p "$OUTPUT_DIR/markdown" "$OUTPUT_DIR/evaluation"
mkdir -p "$EXPECTED_DIR/markdown" "$EXPECTED_DIR/evaluation"
mkdir -p "$DIFF_DIR/markdown" "$DIFF_DIR/evaluation"

# Clear previous output and diff
rm -f "$OUTPUT_DIR/markdown"/* "$OUTPUT_DIR/evaluation"/* 2>/dev/null || true
rm -f "$DIFF_DIR/markdown"/* "$DIFF_DIR/evaluation"/* 2>/dev/null || true

# Check if madola executable exists
if [ "$MODE" = "native" ]; then
    MADOLA_CMD="$PROJECT_DIR/dist/madola.exe"
    if [ ! -f "$MADOLA_CMD" ]; then
        echo "Error: $MADOLA_CMD not found. Run 'dev.bat build' first."
        exit 1
    fi
elif [ "$MODE" = "wasm" ]; then
    MADOLA_JS="$PROJECT_DIR/dist/madola.js"
    MADOLA_WASM="$PROJECT_DIR/dist/madola.wasm"
    if [ ! -f "$MADOLA_JS" ] || [ ! -f "$MADOLA_WASM" ]; then
        echo "Warning: WASM files not found. Tests will fail but output files will be created."
        echo "Run 'dev.bat wasm' to build WASM files for proper testing."
    fi
else
    echo "Error: Invalid mode '$MODE'. Use 'native' or 'wasm'."
    exit 1
fi

# Function to run test case
run_test() {
    local test_file="$1"
    local base_name=$(basename "$test_file" .clg)

    echo "Running test: $base_name"

    if [ "$MODE" = "native" ]; then
        # Run native executable
        "$MADOLA_CMD" "$test_file" > "$OUTPUT_DIR/evaluation/$base_name.txt" 2>&1 || true
        "$MADOLA_CMD" "$test_file" --format > "$OUTPUT_DIR/markdown/$base_name.md" 2>&1 || true
    else
        # Run WASM version using madola_runner.js
        cd "$PROJECT_DIR"
        # Run WASM evaluation (equivalent to native execution)
        node madola_runner.js "$test_file" --output "$OUTPUT_DIR/evaluation/$base_name.txt" || true
        # Run WASM markdown formatting (equivalent to --format flag)
        node madola_runner.js "$test_file" --format --output "$OUTPUT_DIR/markdown/$base_name.md" || true
    fi
}

# Run all test cases
if [ ! -d "$FIXTURES_DIR" ] || [ -z "$(ls -A "$FIXTURES_DIR" 2>/dev/null)" ]; then
    echo "No test fixtures found in $FIXTURES_DIR"
    echo "Create some .clg files in the fixtures directory first."
    exit 1
fi

test_count=0
for test_file in "$FIXTURES_DIR"/*.clg; do
    if [ -f "$test_file" ]; then
        run_test "$test_file"
        ((test_count++))
    fi
done

echo "Ran $test_count test cases"

# Update expected results if requested
if [ "$UPDATE" = true ]; then
    echo "Updating expected results..."
    cp "$OUTPUT_DIR/markdown"/* "$EXPECTED_DIR/markdown/" 2>/dev/null || true
    cp "$OUTPUT_DIR/evaluation"/* "$EXPECTED_DIR/evaluation/" 2>/dev/null || true
    echo "Expected results updated from output to expected"
    exit 0
fi

# Compare results and generate diffs
echo "Comparing results..."
diff_count=0
pass_count=0

for output_file in "$OUTPUT_DIR/markdown"/*.md; do
    if [ -f "$output_file" ]; then
        base_name=$(basename "$output_file")
        expected_file="$EXPECTED_DIR/markdown/$base_name"
        diff_file="$DIFF_DIR/markdown/$base_name"

        if [ -f "$expected_file" ]; then
            if diff -u "$expected_file" "$output_file" > "$diff_file" 2>/dev/null; then
                rm "$diff_file"  # No differences
                echo "✓ $base_name (markdown) - PASS"
                ((pass_count++))
            else
                echo "✗ $base_name (markdown) - FAIL (see $diff_file)"
                ((diff_count++))
            fi
        else
            echo "? $base_name (markdown) - NO BASELINE"
            echo "Run with 'update' argument to create baseline: ./run_regression.sh $MODE update"
        fi
    fi
done

for output_file in "$OUTPUT_DIR/evaluation"/*.txt; do
    if [ -f "$output_file" ]; then
        base_name=$(basename "$output_file")
        expected_file="$EXPECTED_DIR/evaluation/$base_name"
        diff_file="$DIFF_DIR/evaluation/$base_name"

        if [ -f "$expected_file" ]; then
            if diff -u "$expected_file" "$output_file" > "$diff_file" 2>/dev/null; then
                rm "$diff_file"  # No differences
                echo "✓ $base_name (evaluation) - PASS"
                ((pass_count++))
            else
                echo "✗ $base_name (evaluation) - FAIL (see $diff_file)"
                ((diff_count++))
            fi
        else
            echo "? $base_name (evaluation) - NO BASELINE"
            echo "Run with 'update' argument to create baseline: ./run_regression.sh $MODE update"
        fi
    fi
done

echo ""
echo "Results: $pass_count passed, $diff_count failed"

if [ $diff_count -gt 0 ]; then
    echo ""
    echo "To see differences, check files in: $DIFF_DIR"
    echo "To update baselines, run: ./run_regression.sh $MODE update"
    exit 1
fi

echo "All tests passed!"