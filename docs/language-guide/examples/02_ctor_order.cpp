//@ cpp: compiles
//@ warns: some
// A member initializer list runs in DECLARATION order, whatever order it is written in. Here b is
// initialized first and reads a before a exists. g++ -Wall warns (-Wreorder, -Wmaybe-uninitialized);
// the program is not run by the guide (on the guide's toolchain it died with std::bad_alloc).
#include <iostream>
#include <string>

struct Pair {
    std::string b;
    std::string a;
    Pair() : a("x"), b(a + "y") {}
};

int main() {
    Pair p;
    std::cout << p.b << std::endl;
    return 0;
}
