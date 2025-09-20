# CeeLing Source Mapping for GDB
# This script helps map generated C++ code back to original .clg source

# Disable all interactive features that can cause stdin issues
set pagination off
set height 0
set width 0
set confirm off
set verbose off
set complaints 0

# Disable history to avoid file I/O issues
set history save off

# Define source mapping function
define ceeling-map-source
    # Map generated C++ lines back to .clg source
    set substitute-path example.cpp ../example.clg
    directory ..
end

# Auto-load source mapping
ceeling-map-source

# Set up pretty printing for smart pointers (simplified to avoid issues)
set print pretty on

# Demo debugging capabilities (for batch mode)
define demo-debug
    echo === CeeLing Debug Demo ===\n
    echo Setting breakpoint at main...\n
    break main
    echo Starting program...\n
    run
    echo Current location:\n
    list
    echo Stepping into next line...\n
    next
    echo Current variables:\n
    info locals
    echo Continuing execution...\n
    continue
    echo Program completed.\n
    quit
end

# Auto-run demo if in batch mode
demo-debug