# Examples

* `hello.clg`: the first program in the README (a vector, an object and a loop). `cling hello.clg --cpp hello.cpp && g++ -std=c++17 hello.cpp -o hello`.
* `account.clg`: a small program to step through in the debugger: `tools/clgdebug.sh examples/account.clg` (see [guide chapter 11](../docs/language-guide/11-talking-to-cpp.md)).

The guide is the larger collection of examples: every program in [docs/language-guide](../docs/language-guide/README.md) is compiled and run by CTest, and `tests/safety/` holds several hundred small programs that must be accepted or rejected.
