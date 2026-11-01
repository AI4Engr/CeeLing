//@ cpp: compiles
//@ warns: none
// Write one member, read another. In C++ this is undefined behaviour (C allows it; C++ does not),
// and every compiler accepts it without a word.
#include <iostream>

union Bits {
    int i;
    float f;
};

int main() {
    Bits b;
    b.i = 1;
    std::cout << b.f << std::endl;
    return 0;
}
