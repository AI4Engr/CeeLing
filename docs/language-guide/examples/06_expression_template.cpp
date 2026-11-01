//@ cpp: compiles
//@ warns: none
// A miniature of what Eigen's expression templates do: operator+ does not add, it returns an object that refers to both operands.
// Run-time effect: sum holds references to the temporaries, which died at the end of the statement that created them.
#include <iostream>
#include <vector>

struct Vec {
    std::vector<int> data;
};

struct Sum {                                // the lazy expression "a + b"
    const Vec& a;
    const Vec& b;
    int at(size_t i) const { return a.data[i] + b.data[i]; }
};

Sum operator+(const Vec& a, const Vec& b) { return Sum{a, b}; }

Vec make(int x) { return Vec{{x, x}}; }

int main() {
    auto sum = make(1) + make(2);           // auto keeps the lazy Sum; the two Vec temporaries are destroyed at the semicolon
    std::cout << sum.at(0) << std::endl;    // undefined behaviour
    return 0;
}
