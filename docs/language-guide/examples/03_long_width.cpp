//@ cpp: compiles
//@ warns: none
// Run it: the first number is 4 on 64-bit Windows and 8 on 64-bit Linux; the second is the same everywhere.
#include <iostream>

int main() {
    std::cout << sizeof(long) << " " << sizeof(long long) << std::endl;
    return 0;
}
