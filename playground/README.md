# CeeLing Playground

This directory provides a CMake-based playground for building and debugging CeeLing programs with full source mapping support.

## Features

- **Automatic conversion**: Converts `.clg` files to `.cpp` using CeeLing compiler
- **Smart pointer transformations**: Automatically applies class-type to shared_ptr transformations
- **Source mapping**: Maps generated C++ code back to original CeeLing source for debugging
- **VS Code integration**: Full debugging support with breakpoints in original `.clg` files
- **GDB support**: Command-line debugging with source mapping

## Prerequisites

- CMake 3.16 or higher
- Ninja build system (recommended) or Make
- GCC compiler with C++17 support
- Python 3 (for source map generation)
- VS Code with C/C++ extension (for debugging)

## Quick Start

### 1. Configure and Build

```bash
# Configure the project
cd playground
cmake -S . -B build -G Ninja

# Build the project (converts .clg to .cpp and compiles)
cmake --build build
```

### 2. Run the Program

```bash
# Run the executable
cmake --build build --target run

# Or run directly
./build/example.exe
```

### 3. Debug with VS Code

1. Open the CeeLing project root in VS Code
2. Open `example.clg` in the editor
3. Set breakpoints in the `.clg` file
4. Press F5 or use "Run and Debug" -> "Debug CeeLing Program"
5. The debugger will stop at breakpoints in your original CeeLing code!

### 4. Debug with GDB (Command Line)

```bash
# Debug with GDB using source mapping
cmake --build build --target debug

# Or manually:
cd build
gdb -x ../.gdbinit ./example.exe
```

## Available CMake Targets

- `example` - Build the executable from generated C++
- `run` - Build and run the program
- `debug` - Launch GDB with source mapping
- `generate_source_map` - Generate source map for debugging
- `rebuild` - Clean and rebuild everything

## VS Code Tasks

The following tasks are available in VS Code (Ctrl+Shift+P -> "Tasks: Run Task"):

- **configure-cmake** - Configure CMake project
- **build-ceeling-program** - Build the CeeLing program
- **run-cling-program** - Build and run the program
- **clean-rebuild** - Clean and rebuild everything

## Files Generated

- `build/example.cpp` - Generated C++ code from CeeLing source
- `build/example.cpp.map` - Source map for debugging
- `build/example.exe` - Compiled executable

## Source Mapping

The source mapping works by:

1. Analyzing transformations between `.clg` and `.cpp` files
2. Creating a line-by-line mapping
3. Configuring debugger to map generated code back to original source
4. Enabling breakpoints and stepping in original CeeLing code

## Debugging Tips

- Set breakpoints directly in `example.clg`
- Variables will show with their transformed names (e.g., `std::shared_ptr<Node>`)
- Use "Step Over" (F10) and "Step Into" (F11) as normal
- Watch expressions work with both original and transformed variable names

## Example Usage

1. Edit `../example.clg` with your CeeLing code
2. Run `cmake --build build` to recompile
3. Set breakpoints in the `.clg` file
4. Start debugging with F5 in VS Code

The system automatically detects changes to the `.clg` file and regenerates everything as needed.