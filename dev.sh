#!/bin/bash

# CeeLing Development Script - Simplified

set -e

# Colors
GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m'

info() { echo -e "${GREEN}[INFO]${NC} $1"; }
error() { echo -e "${RED}[ERROR]${NC} $1"; exit 1; }

# Check dependencies
command -v cmake >/dev/null || error "CMake not found. Please install CMake."

# Main commands
case "${1:-help}" in
    "setup")
        info "Setting up CeeLing development environment..."
        git submodule update --init --recursive
        if command -v tree-sitter >/dev/null; then
            cd tree-sitter-ceeling && tree-sitter generate && cd ..
        else
            error "Tree-sitter CLI not found. Install: npm install -g tree-sitter-cli"
        fi
        info "✅ Setup complete"
        ;;

    "build")
        info "Building CeeLing..."

        # Clean dist executables first
        mkdir -p dist
        [ -f dist/cling ] && rm dist/cling
        [ -f dist/cling.exe ] && rm dist/cling.exe

        # Generate parser from grammar
        if command -v tree-sitter >/dev/null; then
            cd tree-sitter-ceeling && tree-sitter generate && cd ..
        else
            error "Tree-sitter CLI not found. Install: npm install -g tree-sitter-cli"
        fi

        mkdir -p build
        cd build
        cmake .. -DCMAKE_BUILD_TYPE=Debug
        cmake --build . --parallel
        cd ..

        # Copy to dist/
        mkdir -p dist
        [ -f build/cling ] && cp build/cling dist/
        [ -f build/cling.exe ] && cp build/cling.exe dist/
        info "✅ Build complete"
        ;;

    "wasm")
        info "Building WASM..."
        command -v emcc >/dev/null || error "Emscripten not found. Install Emscripten."

        # Clean dist WASM files first
        mkdir -p dist
        [ -f dist/cling.js ] && rm dist/cling.js
        [ -f dist/cling.wasm ] && rm dist/cling.wasm

        # Generate parser from grammar
        if command -v tree-sitter >/dev/null; then
            cd tree-sitter-ceeling && tree-sitter generate && cd ..
        else
            error "Tree-sitter CLI not found. Install: npm install -g tree-sitter-cli"
        fi
        emcc src/wasm_interface.cpp \
             src/core/generator/cpp_generator.cpp \
             src/core/cst_to_ast/ast_builder.cpp \
             tree-sitter-ceeling/src/parser.c \
             vendor/tree-sitter/lib/src/lib.c \
             -I src \
             -I tree-sitter-ceeling/src \
             -I vendor/tree-sitter/lib/include \
             -I vendor/tree-sitter/lib/src \
             -DWITH_TREE_SITTER \
             -s EXPORTED_FUNCTIONS='["_parse_cling","_evaluate_cling","_free_result"]' \
             -s EXPORTED_RUNTIME_METHODS='["ccall","UTF8ToString"]' \
             -s MODULARIZE=1 \
             -s EXPORT_NAME=CLing \
             -s ENVIRONMENT=node \
             -s NO_EXIT_RUNTIME=1 \
             -s NODEJS_CATCH_EXIT=0 \
             -s NODEJS_CATCH_REJECTION=0 \
             -o dist/cling.js
        info "✅ WASM build complete"
        ;;

    "test")
        info "Running tests..."
        [ ! -f dist/cling ] && [ ! -f dist/cling.exe ] && $0 build
        cd build && ctest --output-on-failure && cd ..
        ;;

    "run")
        [ ! -f dist/cling ] && [ ! -f dist/cling.exe ] && $0 build
        EXEC="dist/cling"
        [ -f "dist/cling.exe" ] && EXEC="dist/cling.exe"

        INPUT_FILE=${2:-tests/binary_tree.clg}
        OUTPUT_FILE=${3:-build/generated_output.cpp}

        info "Generating C++ from $INPUT_FILE..."
        ${EXEC} --cpp "$OUTPUT_FILE" "$INPUT_FILE"
        info "✅ C++ saved to $OUTPUT_FILE"
        ;;

    "regression")
        info "Running regression tests..."
        [ -f regression/run_regression.sh ] || error "Regression script not found"
        ./regression/run_regression.sh "$2"
        ;;

    "serve")
        info "Starting demo server..."
        [ ! -f dist/cling.js ] && $0 wasm
        command -v node >/dev/null || error "Node.js not found. Install Node.js to run demo server."
        info "🚀 Starting server at http://localhost:8080"
        node serve.js
        ;;

    "clean")
        info "Cleaning..."
        rm -rf build/ build-wasm/ dist/
        ;;

    "help"|*)
        echo "CLing Development Commands:"
        echo ""
        echo "  setup      - Initialize submodules and generate grammar"
        echo "  build      - Build native executable"
        echo "  wasm       - Build WASM version"
        echo "  test       - Run test suite"
        echo "  run [input] [output] - Transpile input .clg to C++"
        echo "                         Default output: build/generated_output.cpp"
        echo "  regression - Run regression tests"
        echo "  serve      - Start demo server at http://localhost:8080"
        echo "  clean      - Clean build artifacts"
        echo "  help       - Show this help"
        echo ""
        echo "Quick start:"
        echo "  ./dev.sh setup    # First time setup"
        echo "  ./dev.sh build    # Build and test"
        echo "  ./dev.sh wasm     # Build for web"
        echo "  ./dev.sh serve    # Start web demo"
        echo ""
        echo "Run examples:"
        echo "  ./dev.sh run tests/binary_tree.clg"
        echo "  ./dev.sh run tests/linked_list.clg build/linked_list.cpp"
        ;;
esac
