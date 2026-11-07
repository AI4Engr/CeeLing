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

# Built-in fact packs (facts/*.clfacts) become a generated source file, so cling stays a single executable
include(${CMAKE_SOURCE_DIR}/cmake/embed_facts.cmake)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/generated)
cling_embed_facts(${CMAKE_BINARY_DIR}/generated/builtin_facts_data.cpp ${CMAKE_SOURCE_DIR}/facts)

# Source files
set(CORE_SOURCES
    src/core/generator/cpp_generator.cpp
    src/core/generator/smart_pointer_analyzer.cpp
    src/core/generator/dangling_checker.cpp
    src/core/generator/escape_hatch_checker.cpp
    src/core/generator/pairing_checker.cpp
    src/core/generator/closure_desugar.cpp
    src/core/generator/ast_emitter.cpp
    src/core/generator/library_summary.cpp
    src/core/generator/fact_table.cpp
    src/core/generator/sha256.cpp
    ${CMAKE_BINARY_DIR}/generated/builtin_facts_data.cpp
    src/core/generator/class_hierarchy.cpp
)

# The parser: tree-sitter (vendor/tree-sitter is a git submodule) with the checked-in generated grammar
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/vendor/tree-sitter/lib/src/lib.c")
    message(FATAL_ERROR "vendor/tree-sitter is empty: run `git submodule update --init` (or clone with --recursive)")
endif()
list(APPEND CORE_SOURCES
    src/core/cst_to_ast/ast_builder.cpp
    src/core/generator/struct_json.cpp
    src/core/generator/channel_check.cpp)

add_library(tree_sitter STATIC vendor/tree-sitter/lib/src/lib.c)
target_compile_definitions(tree_sitter PRIVATE TREE_SITTER_HIDE_SYMBOLS)
add_library(cling_grammar STATIC tree-sitter-ceeling/src/parser.c)

# Core library and the compiler
add_library(cling_core STATIC ${CORE_SOURCES})
add_executable(cling src/main.cpp)
target_link_libraries(cling cling_core tree_sitter cling_grammar)
target_compile_definitions(cling PRIVATE WITH_TREE_SITTER)

# Testing
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

# Transpile each fixture, then compile the generated C++ with the C++ compiler.
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

# The generated code keeps the pointer kinds the analysis chose (a forward link, a weak back link, a shared local).
add_test(NAME cling_test_bidirectional_analysis_report
    COMMAND ${CMAKE_COMMAND}
        -DFILE=${CMAKE_BINARY_DIR}/test_output_bidirectional_analysis.cpp
        -DNEEDLE1=std::shared_ptr<Node>\ next
        -DNEEDLE2=std::weak_ptr<Node>\ prev
        -DNEEDLE3=std::shared_ptr<Node>\ tail\ =\ second
        -P ${CMAKE_SOURCE_DIR}/tests/assert_contains.cmake)
set_tests_properties(cling_test_bidirectional_analysis_report PROPERTIES
    DEPENDS cling_test_bidirectional_analysis)

# A basic function example transpiles.
add_test(NAME cling_test_example_transpiles
    COMMAND cling --cpp ${CMAKE_BINARY_DIR}/test_output_example.cpp ${CMAKE_SOURCE_DIR}/tests/example_transpiles.clg)

# A factory function returning an object lowers the return type to a smart pointer and moves the owning local out, so the
# result compiles under a real C++ compiler.
add_test(NAME cling_test_ast_return_object
    COMMAND cling --cpp ${CMAKE_BINARY_DIR}/test_output_ast_return_object.cpp
        ${CMAKE_SOURCE_DIR}/tests/return_object_factory.clg)
if(MSVC)
    add_test(NAME cling_test_ast_return_object_compile
        COMMAND ${CMAKE_CXX_COMPILER} /nologo /EHsc /std:c++17
            ${CMAKE_BINARY_DIR}/test_output_ast_return_object.cpp
            /Fe:${CMAKE_BINARY_DIR}/test_output_ast_return_object.exe)
else()
    add_test(NAME cling_test_ast_return_object_compile
        COMMAND ${CMAKE_CXX_COMPILER} -std=c++17
            ${CMAKE_BINARY_DIR}/test_output_ast_return_object.cpp
            -o ${CMAKE_BINARY_DIR}/test_output_ast_return_object.exe)
endif()
set_tests_properties(cling_test_ast_return_object_compile PROPERTIES
    DEPENDS cling_test_ast_return_object)

# The safety contract as fixtures: tests/safety/run_safety_tests.sh scans every fixture tagged `//@ expect: reject|accept`, runs cling,
# and checks the verdict (accepted fixtures are also compiled and, when tagged, run with their output asserted). --strict: a missed
# reject is a failure.
find_program(BASH_PROGRAM bash)
if(BASH_PROGRAM)
    add_test(NAME cling_safety_contract_baseline
        COMMAND ${BASH_PROGRAM}
            ${CMAKE_SOURCE_DIR}/tests/safety/run_safety_tests.sh --strict
            $<TARGET_FILE:cling>)
    # Separate compilation: --ptr-demands / --ptr-demands-out (a file that uses a class needs a pointer kind the declaring file would not choose)
    add_test(NAME cling_ptr_demands
        COMMAND ${BASH_PROGRAM} ${CMAKE_SOURCE_DIR}/tests/separate/run_demands_test.sh $<TARGET_FILE:cling>)
    # Dangling / use-after-move across files: every safety fixture split in two gets the same verdict, plus hand-written cross-file cases
    add_test(NAME cling_cross_file_safety
        COMMAND ${BASH_PROGRAM} ${CMAKE_SOURCE_DIR}/tests/separate/run_cross_file_test.sh $<TARGET_FILE:cling>)
    # Debugging: `cling -g` puts a #line before every statement; every accepted fixture still compiles, and gdb steps through the .clg source
    add_test(NAME cling_debug_lines
        COMMAND ${BASH_PROGRAM} ${CMAKE_SOURCE_DIR}/tests/debug/run_debug_lines_test.sh $<TARGET_FILE:cling>)
    # The language guide: every example compiles/runs as its tags say, every code block and message quoted in a chapter is what the compiler really produces
endif()
find_program(PYTHON_PROGRAM NAMES python3 python)
if(BASH_PROGRAM AND PYTHON_PROGRAM)
    add_test(NAME cling_guide_examples
        COMMAND ${PYTHON_PROGRAM} -I ${CMAKE_SOURCE_DIR}/docs/language-guide/examples/check.py check --cling $<TARGET_FILE:cling>)
endif()

# Fact packs: the loader rejects everything it does not understand, and the shipped packs say what the old hand tables said
add_executable(fact_table_test tests/facts/fact_table_test.cpp)
target_link_libraries(fact_table_test cling_core)
add_test(NAME cling_fact_table COMMAND fact_table_test)
# cling-scan: a draft pack written from a class declaration, then used. Needs clang++ (found on the path, or -DCLING_CLANG=...); the
# libclang / LibTooling variants additionally need WSL with python3 + libclang (and the built extractor) and skip themselves without them.
find_program(CLING_CLANG NAMES clang++ clang++-18 clang++-17 clang++-16 clang++-15 clang++-14 DOC "clang++ used by cling-scan's tests")
if(BASH_PROGRAM)
    if(CLING_CLANG)
        add_test(NAME cling_scan
            COMMAND ${BASH_PROGRAM} ${CMAKE_SOURCE_DIR}/tests/fact-scan/run_scan_test.sh $<TARGET_FILE:cling> ${CLING_CLANG})
        # every scan (templates and --witness too) through the LibTooling program cling-scan-extract in WSL (skips itself until it is built)
        add_test(NAME cling_scan_libtooling
            COMMAND ${CMAKE_COMMAND} -E env SCAN_BACKEND=libtooling ${BASH_PROGRAM} ${CMAKE_SOURCE_DIR}/tests/fact-scan/run_scan_test.sh $<TARGET_FILE:cling> ${CLING_CLANG})
        # the same test with the scans of a class read through the libclang C API in WSL (skips itself without python3 + libclang there)
        add_test(NAME cling_scan_libclang
            COMMAND ${CMAKE_COMMAND} -E env SCAN_BACKEND=libclang ${BASH_PROGRAM} ${CMAKE_SOURCE_DIR}/tests/fact-scan/run_scan_test.sh $<TARGET_FILE:cling> ${CLING_CLANG})
        # is each scanned fact TRUE of the real library? (AddressSanitizer in WSL; skips itself without Eigen or WSL g++)
        add_test(NAME cling_scan_probe
            COMMAND ${BASH_PROGRAM} ${CMAKE_SOURCE_DIR}/tests/fact-scan/run_probe_test.sh $<TARGET_FILE:cling> ${CLING_CLANG})
    endif()
endif()
# `cling --facts pack`: a project teaches the compiler about its own C++ library (tests/facts/private_lib), and what a project pack may not do
if(BASH_PROGRAM)
    add_test(NAME cling_project_facts
        COMMAND ${BASH_PROGRAM} ${CMAKE_SOURCE_DIR}/tests/facts/run_project_facts_test.sh $<TARGET_FILE:cling>)
endif()
# facts/std-vector.clfacts and std-string.clfacts are generated: they must be exactly what gen_pack.py writes from the committed scans
set(PYTHON_FOR_FACTS ${PYTHON_PROGRAM})
if(PYTHON_FOR_FACTS)
    add_test(NAME cling_facts_generated
        COMMAND ${PYTHON_FOR_FACTS} -I ${CMAKE_SOURCE_DIR}/tools/std-scan-experiment/gen_pack.py --check
            ${CMAKE_SOURCE_DIR}/tools/std-scan-experiment ${CMAKE_SOURCE_DIR}/tools/std-scan-experiment/gcc ${CMAKE_SOURCE_DIR}/facts)
    # The gauge (tools/fact-gauge/gauge.py): seeded dangling programs for every scanned vector method, the usage corpus compiled with g++,
    # and the facts compared with the standard. False accepts must be 0. The two *_detects tests are negative controls: a gauge that cannot fail proves nothing.
    add_test(NAME cling_fact_gauge
        COMMAND ${PYTHON_FOR_FACTS} -I ${CMAKE_SOURCE_DIR}/tools/fact-gauge/gauge.py --cling $<TARGET_FILE:cling>)
    add_test(NAME cling_fact_gauge_detects_false_accept
        COMMAND ${PYTHON_FOR_FACTS} -I ${CMAKE_SOURCE_DIR}/tools/fact-gauge/gauge.py --cling $<TARGET_FILE:cling> --pretend-mutator size)
    add_test(NAME cling_fact_gauge_detects_wrong_fact
        COMMAND ${PYTHON_FOR_FACTS} -I ${CMAKE_SOURCE_DIR}/tools/fact-gauge/gauge.py --cling $<TARGET_FILE:cling> --pretend-nonmutator reserve)
    add_test(NAME cling_fact_gauge_detects_string_fact
        COMMAND ${PYTHON_FOR_FACTS} -I ${CMAKE_SOURCE_DIR}/tools/fact-gauge/gauge.py --cling $<TARGET_FILE:cling> --pretend-mutator compare)
    set_tests_properties(cling_fact_gauge_detects_false_accept cling_fact_gauge_detects_wrong_fact cling_fact_gauge_detects_string_fact PROPERTIES WILL_FAIL TRUE)
endif()

# `cmake --build build --target run_tests`
add_custom_target(run_tests
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    DEPENDS cling
    COMMENT "Running all tests"
)

install(TARGETS cling RUNTIME DESTINATION bin)

message(STATUS "CeeLing: build type ${CMAKE_BUILD_TYPE}, C++${CMAKE_CXX_STANDARD}")
