# Source with `-x cling.gdb` (or `source cling.gdb`) when debugging a program compiled from CeeLing with `cling -g`:
# `step` then goes into your own methods and over the library code the generated C++ calls (smart-pointer operators, the bounds and null checks).
set pagination off
set print pretty on
skip -rfu ^std::
skip -rfu ^__gnu_cxx::
skip -rfu ^cling::
