cmake_minimum_required(VERSION 3.16)
project(CeeLing VERSION 0.1.0 LANGUAGES C CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set C standard
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# Compiler flags
if(MSVC)
    set(CMAKE_CXX_FLAGS_DEBUG "/Od /Zi /W4")
    set(CMAKE_CXX_FLAGS_RELEASE "/O2 /DNDEBUG")
    set(CMAKE_C_FLAGS_DEBUG "/Od /Zi /W4")
    set(CMAKE_C_FLAGS_RELEASE "/O2 /DNDEBUG")
else()
    set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -Wall -Wextra")
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
    set(CMAKE_C_FLAGS_DEBUG "-g -O0 -Wall -Wextra")
    set(CMAKE_C_FLAGS_RELEASE "-O3 -DNDEBUG")
endif()

# Include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/vendor/tree-sitter/lib/include
    ${CMAKE_SOURCE_DIR}/vendor/tree-sitter/lib/src
    ${CMAKE_SOURCE_DIR}/tree-sitter-ceeling/src
)

# Source files
set(CORE_SOURCES
    src/core/generator/cpp_generator.cpp
)

# Optional Tree-sitter support
option(WITH_TREE_SITTER "Build with Tree-sitter support" ON)

if(WITH_TREE_SITTER)
    # Add AST builder to core sources when Tree-sitter is enabled
    list(APPEND CORE_SOURCES src/core/cst_to_ast/ast_builder.cpp)

    # Tree-sitter sources (when available)
    set(TREE_SITTER_SOURCES
        vendor/tree-sitter/lib/src/lib.c
    )

    set(CLING_GRAMMAR_SOURCES
        tree-sitter-ceeling/src/parser.c
    )
endif()

# Core library
add_library(cling_core STATIC ${CORE_SOURCES})

# Main executable (only for native builds)
if(NOT EMSCRIPTEN)
    add_executable(cling src/main.cpp)
    target_link_libraries(cling cling_core)
endif()

# WASM interface library (for Emscripten)
if(EMSCRIPTEN)
    add_executable(cling_wasm src/wasm_interface.cpp)
    target_link_libraries(cling_wasm cling_core)

    # WASM-specific flags
    set_target_properties(cling_wasm PROPERTIES
        LINK_FLAGS "-s WASM=1 -s NO_EXIT_RUNTIME=1 -s MODULARIZE=1 -s EXPORT_NAME='CeeLing' -s \"EXPORTED_FUNCTIONS=[_parse_cling,_evaluate_cling,_free_result]\" -s \"EXPORTED_RUNTIME_METHODS=[ccall,cwrap,UTF8ToString,stringToUTF8]\" -s ALLOW_MEMORY_GROWTH=1"
        OUTPUT_NAME "cling"
        SUFFIX ".js"
    )
endif()

# Configure Tree-sitter libraries and linking after executables are defined
if(WITH_TREE_SITTER)
    # Check if Tree-sitter sources exist
    if(EXISTS "${CMAKE_SOURCE_DIR}/vendor/tree-sitter/lib/src/lib.c")
        add_library(tree_sitter STATIC ${TREE_SITTER_SOURCES})
        target_compile_definitions(tree_sitter PRIVATE TREE_SITTER_HIDE_SYMBOLS)

        if(EXISTS "${CMAKE_SOURCE_DIR}/tree-sitter-ceeling/src/parser.c")
            add_library(cling_grammar STATIC ${CLING_GRAMMAR_SOURCES})

            # Link Tree-sitter to main targets
            if(NOT EMSCRIPTEN)
                target_link_libraries(cling tree_sitter cling_grammar)
                target_compile_definitions(cling PRIVATE WITH_TREE_SITTER)
            endif()

            if(EMSCRIPTEN)
                target_link_libraries(cling_wasm tree_sitter cling_grammar)
                target_compile_definitions(cling_wasm PRIVATE WITH_TREE_SITTER)
            endif()
        endif()
    endif()
endif()

# Testing
if(NOT EMSCRIPTEN)
    enable_testing()
    # Use cling.exe for testing with test files (using absolute paths)
    add_test(NAME cling_test_example COMMAND cling ${CMAKE_SOURCE_DIR}/example.clg)

    # Custom target for running tests
    add_custom_target(run_tests
        COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
        DEPENDS cling
        COMMENT "Running all tests"
    )
endif()

# Install targets
if(EMSCRIPTEN)
    # No install targets for WASM builds
else()
    install(TARGETS cling
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib
    )
endif()

# Print configuration summary
message(STATUS "CeeLing Configuration Summary:")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Tree-sitter Support: ${WITH_TREE_SITTER}")
if(EMSCRIPTEN)
    message(STATUS "  Target: WASM (Emscripten)")
else()
    message(STATUS "  Target: Native")
endif()