//@ cpp: compiles
//@ warns: some
// Run it: prints 1. C++ reads \x41BC as ONE character (and g++ warns that it is out of range), not as A, B, C.
#include <iostream>
#include <string>

int main() {
    std::string s = "\x41BC";
    std::cout << s.size() << std::endl;
    return 0;
}
