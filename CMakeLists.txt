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
    src/core/generator/smart_pointer_analyzer.cpp
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

    function(add_transpile_compile_test test_name input_file generated_cpp output_exe)
        add_test(NAME ${test_name}
            COMMAND cling --cpp ${generated_cpp} ${input_file})

        if(MSVC)
            add_test(NAME ${test_name}_compile
                COMMAND ${CMAKE_CXX_COMPILER} /nologo /EHsc /std:c++17 ${generated_cpp} /Fe:${output_exe})
        else()
            add_test(NAME ${test_name}_compile
                COMMAND ${CMAKE_CXX_COMPILER} -std=c++17 ${generated_cpp} -o ${output_exe})
        endif()

        set_tests_properties(${test_name}_compile PROPERTIES DEPENDS ${test_name})
    endfunction()

    # Smoke tests for supported fixtures.
    add_transpile_compile_test(
        cling_test_binary_tree
        ${CMAKE_SOURCE_DIR}/tests/binary_tree.clg
        ${CMAKE_BINARY_DIR}/test_output_binary_tree.cpp
        ${CMAKE_BINARY_DIR}/test_output_binary_tree.exe)
    add_transpile_compile_test(
        cling_test_linked_list
        ${CMAKE_SOURCE_DIR}/tests/linked_list.clg
        ${CMAKE_BINARY_DIR}/test_output_linked_list.cpp
        ${CMAKE_BINARY_DIR}/test_output_linked_list.exe)
    add_transpile_compile_test(
        cling_test_new_expression_paths
        ${CMAKE_SOURCE_DIR}/tests/new_expression_paths.clg
        ${CMAKE_BINARY_DIR}/test_output_new_expression_paths.cpp
        ${CMAKE_BINARY_DIR}/test_output_new_expression_paths.exe)
    add_transpile_compile_test(
        cling_test_bidirectional_analysis
        ${CMAKE_SOURCE_DIR}/tests/bidirectional_analysis.clg
        ${CMAKE_BINARY_DIR}/test_output_bidirectional_analysis.cpp
        ${CMAKE_BINARY_DIR}/test_output_bidirectional_analysis.exe)

    add_test(NAME cling_test_bidirectional_analysis_report
        COMMAND ${CMAKE_COMMAND}
            -DFILE=${CMAKE_BINARY_DIR}/test_output_bidirectional_analysis.cpp
            -DNEEDLE1=CeeLing\ pointer\ analysis\ report
            -DNEEDLE2=std::shared_ptr<Node>\ next
            -DNEEDLE3=std::weak_ptr<Node>\ prev
            -DNEEDLE4=std::move(second)
            -P ${CMAKE_SOURCE_DIR}/tests/assert_contains.cmake)
    set_tests_properties(cling_test_bidirectional_analysis_report PROPERTIES
        DEPENDS cling_test_bidirectional_analysis)

    # Unsupported/legacy syntax should fail fast instead of silently succeeding.
    add_test(NAME cling_test_legacy_syntax_rejected
        COMMAND cling --cpp ${CMAKE_BINARY_DIR}/test_output_legacy.cpp ${CMAKE_SOURCE_DIR}/regression/fixtures/example.clg)
    set_tests_properties(cling_test_legacy_syntax_rejected PROPERTIES WILL_FAIL TRUE)

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
