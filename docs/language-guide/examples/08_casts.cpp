//@ cpp: compiles
//@ warns: none
// Three ways to say "treat this as something else", all legal C++. The first reads a double's
// bits through an integer pointer (undefined behaviour by the aliasing rules), the second writes
// through a const object (undefined behaviour), the third is a C-style cast, which silently
// picks whichever C++ cast it needs, reinterpret_cast included.
#include <iostream>

struct Config {
    int level;
};

int main() {
    double d = 1.0;
    double* pd = &d;
    long long bits = *reinterpret_cast<long long*>(pd);

    const Config c{1};
    const_cast<Config&>(c).level = 2;

    const char* text = "abc";
    unsigned long long address = (unsigned long long)text;

    std::cout << bits << " " << c.level << " " << address << std::endl;
    return 0;
}
