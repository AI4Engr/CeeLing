@echo off
setlocal enabledelayedexpansion

REM CeeLing Development Workflow Script for Windows (CMake Version)

echo CeeLing Development Workflow (CMake)
echo ===================================

REM Check if CMake is available
cmake --version >nul 2>&1
if errorlevel 1 (
    echo [ERROR] CMake not found. Please install CMake.
    exit /b 1
)

set BUILD_DIR=build

if "%1"=="init-submodules" (
    echo [INFO] Initializing git submodules...
    git submodule update --init --recursive
    echo [INFO] Submodules initialized
    goto :eof
)

if "%1"=="generate-grammar" (
    echo [INFO] Generating Tree-sitter parser from grammar...
    REM Ensure submodules are initialized
    if not exist "vendor\tree-sitter\lib\src\lib.c" (
        echo [INFO] Initializing Tree-sitter submodule first...
        call %0 init-submodules
    )

    tree-sitter --version >nul 2>&1
    if errorlevel 1 (
        echo [ERROR] Tree-sitter CLI not found. Install with: npm install -g tree-sitter-cli
        exit /b 1
    )

    cd tree-sitter-ceeling
    tree-sitter generate
    cd ..
    echo [INFO] Grammar generation complete
    goto :eof
)

if "%1"=="clean" (
    echo [INFO] Cleaning build artifacts...
    if exist %BUILD_DIR% rmdir /s /q %BUILD_DIR%
    if exist dist rmdir /s /q dist
    goto :eof
)

if "%1"=="configure" (
    echo [INFO] Configuring CMake build with GCC...
    if not exist %BUILD_DIR% mkdir %BUILD_DIR%
    cd %BUILD_DIR%
    cmake .. -G "Ninja" -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DCMAKE_BUILD_TYPE=Debug
    cd ..
    echo [INFO] Configuration complete
    goto :eof
)

if "%1"=="configure-release" (
    echo [INFO] Configuring CMake build ^(Release^) with GCC...
    if not exist %BUILD_DIR% mkdir %BUILD_DIR%
    cd %BUILD_DIR%
    cmake .. -G "Ninja" -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DCMAKE_BUILD_TYPE=Release
    cd ..
    echo [INFO] Release configuration complete
    goto :eof
)

if "%1"=="build" (
    echo [INFO] Building CLing ^(native^)...
    if not exist %BUILD_DIR% (
        echo [INFO] Configuring first...
        call %0 configure
    )

    cd %BUILD_DIR%
    ninja
    cd ..

    REM Copy executables to dist/
    if not exist dist mkdir dist
    if exist %BUILD_DIR%\cling.exe copy %BUILD_DIR%\cling.exe dist\
    if exist %BUILD_DIR%\cling copy %BUILD_DIR%\cling dist\
    if exist %BUILD_DIR%\test_cling.exe copy %BUILD_DIR%\test_cling.exe dist\
    if exist %BUILD_DIR%\test_cling copy %BUILD_DIR%\test_cling dist\

    echo [INFO] Build complete
    goto :eof
)

if "%1"=="test" (
    echo [INFO] Building and running tests...
    call %0 build

    echo [INFO] Running CTest...
    cd %BUILD_DIR%
    ctest --output-on-failure
    cd ..
    goto :eof
)

if "%1"=="regression" (
    echo [INFO] Running regression tests...
    if exist regression\run_regression.bat (
        call regression\run_regression.bat %2 %3
    ) else (
        bash regression/run_regression.sh %2 %3
    )
    goto :eof
)

if "%1"=="wasm" (
    echo [INFO] Building CLing for WASM...
    emcc --version >nul 2>&1
    if errorlevel 1 (
        echo [ERROR] Emscripten ^(emcc^) not found. Please install Emscripten for WASM builds.
        exit /b 1
    )

    set WASM_BUILD_DIR=build-wasm
    if not exist !WASM_BUILD_DIR! mkdir !WASM_BUILD_DIR!
    cd !WASM_BUILD_DIR!

    emcmake cmake .. -DCMAKE_BUILD_TYPE=Release
    ninja

    cd ..
    if not exist dist mkdir dist
    if not exist web mkdir web
    if exist !WASM_BUILD_DIR!\cling.js copy !WASM_BUILD_DIR!\cling.js dist\
    if exist !WASM_BUILD_DIR!\cling.wasm copy !WASM_BUILD_DIR!\cling.wasm dist\
    if exist !WASM_BUILD_DIR!\cling.js copy !WASM_BUILD_DIR!\cling.js web\
    if exist !WASM_BUILD_DIR!\cling.wasm copy !WASM_BUILD_DIR!\cling.wasm web\

    echo [INFO] WASM build complete
    goto :eof
)

if "%1"=="run" (
    set EXECUTABLE=dist\cling.exe
    if not exist !EXECUTABLE! (
        set EXECUTABLE=dist\cling
        if not exist !EXECUTABLE! (
            echo [INFO] Building CLing first...
            call %0 build
            set EXECUTABLE=dist\cling.exe
            if not exist !EXECUTABLE! set EXECUTABLE=dist\cling
        )
    )

    set INPUT_FILE=%2
    if "!INPUT_FILE!"=="" set INPUT_FILE=tests\binary_tree.clg

    set OUTPUT_FILE=%3
    if "!OUTPUT_FILE!"=="" set OUTPUT_FILE=build\generated_output.cpp

    echo [INFO] Generating C++ from !INPUT_FILE!...
    !EXECUTABLE! --cpp !OUTPUT_FILE! !INPUT_FILE!
    if errorlevel 1 (
        echo [ERROR] C++ generation failed
        exit /b 1
    )
    echo [INFO] C++ saved to !OUTPUT_FILE!
    goto :eof
)

if "%1"=="debug" (
    echo [INFO] Building debug version...
    call %0 configure
    call %0 build
    echo [INFO] Debug build complete
    goto :eof
)

if "%1"=="release" (
    echo [INFO] Building release version...
    call %0 configure-release
    call %0 build
    echo [INFO] Release build complete
    goto :eof
)

if "%1"=="tree-sitter" (
    echo [INFO] Building with Tree-sitter support...
    REM First generate the grammar
    call %0 generate-grammar

    if not exist %BUILD_DIR% mkdir %BUILD_DIR%
    cd %BUILD_DIR%
    cmake .. -DCMAKE_BUILD_TYPE=Debug -DWITH_TREE_SITTER=ON
    cmake --build . --parallel
    cd ..
    echo [INFO] Tree-sitter build complete
    goto :eof
)

REM Default help
echo CLing Development Workflow Commands ^(CMake^):
echo.
echo   init-submodules    - Initialize git submodules ^(Tree-sitter^)
echo   clean              - Clean build artifacts
echo   generate-grammar   - Generate Tree-sitter parser from grammar.js
echo   configure          - Configure CMake ^(Debug^)
echo   configure-release  - Configure CMake ^(Release^)
echo   build              - Build native executable
echo   test               - Build and run test suite
echo   regression         - Run regression tests ^(native only^)
echo   wasm               - Build WASM version ^(requires emscripten^)
echo   run [input] [output] - Transpile input .clg to C++ ^(default output: build\generated_output.cpp^)
echo   debug              - Build debug version
echo   release            - Build release version
echo   tree-sitter        - Build with Tree-sitter support
echo   help               - Show this help
echo.
echo Examples:
echo   dev.bat generate-grammar
echo   dev.bat build
echo   dev.bat test
echo   dev.bat regression
echo   dev.bat run tests\binary_tree.clg
echo   dev.bat run tests\linked_list.clg build\linked_list.cpp
echo   dev.bat wasm
echo   dev.bat tree-sitter
