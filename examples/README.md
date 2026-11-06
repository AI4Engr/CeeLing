# Examples

`account.clg` is a small program used by the debugging section of the Language Guide ([chapter 11](../docs/language-guide/11-talking-to-cpp.md)): compile it with `cling -g account.clg --cpp account.cpp`, build the C++ with `g++ -g -O0`, and step through the `.clg` source in `gdb` (`tools/clgdebug.sh account.clg` does all of it).

The guide itself is the collection of examples: every program in [docs/language-guide](../docs/language-guide/README.md) is compiled and run by CTest, and `tests/safety/` holds several hundred small programs that must be accepted or rejected.
