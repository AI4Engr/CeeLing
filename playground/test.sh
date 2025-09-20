#!/bin/bash

# CeeLing Playground Test Script
echo "=== CeeLing Playground Test ==="

# Check if we're in the right directory
if [ ! -f "CMakeLists.txt" ]; then
    echo "❌ Error: Please run this script from the playground directory"
    exit 1
fi

# Test 1: Configure CMake
echo "🔧 Configuring CMake..."
if cmake -S . -B build -G Ninja; then
    echo "✅ CMake configuration successful"
else
    echo "❌ CMake configuration failed"
    exit 1
fi

# Test 2: Build project
echo "🔨 Building project..."
if cmake --build build; then
    echo "✅ Build successful"
else
    echo "❌ Build failed"
    exit 1
fi

# Test 3: Check generated files
echo "📁 Checking generated files..."
if [ -f "build/example.cpp" ]; then
    echo "✅ Generated C++ file exists"
else
    echo "❌ Generated C++ file missing"
    exit 1
fi

if [ -f "build/example.cpp.map" ]; then
    echo "✅ Source map file exists"
else
    echo "❌ Source map file missing"
    exit 1
fi

if [ -f "build/example.exe" ]; then
    echo "✅ Executable file exists"
else
    echo "❌ Executable file missing"
    exit 1
fi

# Test 4: Run the program
echo "🚀 Running the program..."
if cmake --build build --target run; then
    echo "✅ Program execution successful"
else
    echo "❌ Program execution failed"
    exit 1
fi

# Test 5: Verify transformations
echo "🔍 Verifying smart pointer transformations..."
if grep -q "std::shared_ptr<Node>" build/example.cpp; then
    echo "✅ Smart pointer transformation found"
else
    echo "❌ Smart pointer transformation missing"
    exit 1
fi

if grep -q "std::make_shared<Node>" build/example.cpp; then
    echo "✅ make_shared transformation found"
else
    echo "❌ make_shared transformation missing"
    exit 1
fi

# Test 6: Check source map content
echo "🗺️ Verifying source map..."
if grep -q '"lineMapping"' build/example.cpp.map; then
    echo "✅ Source map contains line mappings"
else
    echo "❌ Source map missing line mappings"
    exit 1
fi

echo ""
echo "🎉 All tests passed! CeeLing playground is working correctly."
echo ""
echo "Usage instructions:"
echo "  - Edit ../example.clg with your CeeLing code"
echo "  - Run 'cmake --build build' to recompile"
echo "  - Use VS Code debugging with F5"
echo "  - Set breakpoints directly in .clg files"
echo ""
echo "Available targets:"
echo "  - cmake --build build --target run     (build and run)"
echo "  - cmake --build build --target debug   (debug with GDB)"
echo "  - cmake --build build --target rebuild (clean rebuild)"