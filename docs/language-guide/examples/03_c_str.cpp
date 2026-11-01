//@ cpp: compiles
//@ warns: none
// Run it: prints garbage. The string inside label() is destroyed when label() returns.
#include <iostream>
#include <string>

const char* label(int n) {
    std::string s = "item " + std::to_string(n);
    return s.c_str();
}

int main() {
    const char* p = label(7);
    std::cout << p << std::endl;
    return 0;
}
