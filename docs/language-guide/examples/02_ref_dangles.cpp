//@ cpp: compiles
//@ warns: none
// A C++ reference to an object owned by a unique_ptr: replacing the owner destroys the object,
// and the reference still compiles, with no warning. Reading it is undefined behaviour (not run by the guide).
#include <iostream>
#include <memory>

struct Counter {
    int n = 0;
};

int main() {
    auto c = std::make_unique<Counter>();
    Counter& view = *c;
    c = std::make_unique<Counter>();       // the old object is destroyed here
    std::cout << view.n << std::endl;      // reads a destroyed object
    return 0;
}
