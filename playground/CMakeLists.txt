cmake_minimum_required(VERSION 3.16)
project(CeeLingPlayground)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Enable debug information and source mapping
set(CMAKE_BUILD_TYPE Debug)
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")

# Define paths
set(CLING_EXECUTABLE "${CMAKE_CURRENT_SOURCE_DIR}/../dist/cling.exe")
set(SOURCE_FILE "${CMAKE_CURRENT_SOURCE_DIR}/example.clg")
set(GENERATED_CPP "${CMAKE_CURRENT_BINARY_DIR}/example.cpp")
set(SOURCE_MAP "${CMAKE_CURRENT_BINARY_DIR}/example.cpp.map")
set(EXECUTABLE_NAME "example")

# Find Python for source map generation
find_program(PYTHON_EXECUTABLE python3 python)
if(NOT PYTHON_EXECUTABLE)
    message(WARNING "Python not found. Source map generation will be skipped.")
endif()

# Custom command to convert .clg to .cpp using CeeLing
add_custom_command(
    OUTPUT ${GENERATED_CPP}
    COMMAND ${CLING_EXECUTABLE} ${SOURCE_FILE} --cpp ${GENERATED_CPP}
    DEPENDS ${SOURCE_FILE}
    COMMENT "Converting ${SOURCE_FILE} to ${GENERATED_CPP} using CeeLing"
    VERBATIM
)

# Custom command to generate source map
if(PYTHON_EXECUTABLE)
    add_custom_command(
        OUTPUT ${SOURCE_MAP}
        COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/generate_source_map.py
                ${SOURCE_FILE} ${GENERATED_CPP} ${SOURCE_MAP}
        DEPENDS ${GENERATED_CPP} ${CMAKE_CURRENT_SOURCE_DIR}/generate_source_map.py
        COMMENT "Generating source map for debugging"
        VERBATIM
    )
    set(SOURCE_MAP_DEPENDENCY ${SOURCE_MAP})
else()
    set(SOURCE_MAP_DEPENDENCY "")
endif()

# Create executable from generated C++ code
add_executable(${EXECUTABLE_NAME} ${GENERATED_CPP})

# Add compile options for debugging
target_compile_options(${EXECUTABLE_NAME} PRIVATE
    -g          # Generate debug information
    -O0         # No optimization for debugging
    -std=c++17  # C++17 standard for smart pointers
)

# Ensure source map is generated before building
if(SOURCE_MAP_DEPENDENCY)
    add_custom_target(generate_source_map DEPENDS ${SOURCE_MAP_DEPENDENCY})
    add_dependencies(${EXECUTABLE_NAME} generate_source_map)
endif()

# Add custom target to run the executable
add_custom_target(run
    COMMAND ${EXECUTABLE_NAME}
    DEPENDS ${EXECUTABLE_NAME}
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Running ${EXECUTABLE_NAME}"
)

# Add custom target to debug with GDB
add_custom_target(debug
    COMMAND gdb -x ${CMAKE_CURRENT_SOURCE_DIR}/.gdbinit ${EXECUTABLE_NAME}
    DEPENDS ${EXECUTABLE_NAME}
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Debugging ${EXECUTABLE_NAME} with GDB"
)

# Add custom target to clean and rebuild
add_custom_target(rebuild
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_CURRENT_BINARY_DIR} --target clean
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_CURRENT_BINARY_DIR}
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Cleaning and rebuilding project"
)

# Print configuration info
message(STATUS "CeeLing executable: ${CLING_EXECUTABLE}")
message(STATUS "Source file: ${SOURCE_FILE}")
message(STATUS "Generated C++ file: ${GENERATED_CPP}")
message(STATUS "Source map file: ${SOURCE_MAP}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Python executable: ${PYTHON_EXECUTABLE}")