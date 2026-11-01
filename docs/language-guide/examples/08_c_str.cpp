//@ cpp: compiles
//@ warns: none
// c_str() of a temporary string: the string is destroyed at the end of the statement, the
// pointer lives on. Compiles with no warning; reading *p is undefined behaviour.
#include <iostream>
#include <string>

std::string name() { return "a string long enough to live on the heap, not in the object"; }

int main() {
    const char* p = name().c_str();
    std::cout << p << std::endl;
    return 0;
}
