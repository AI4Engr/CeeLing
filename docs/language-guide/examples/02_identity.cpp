//@ cpp: compiles
//@ warns: none
// With smart pointers, == on two handles compares ADDRESSES; comparing the objects needs a dereference
// and an operator== somebody wrote. Both spellings compile, and they look almost the same.
#include <iostream>
#include <memory>

struct Point {
    int x;
    explicit Point(int v) : x(v) {}
    bool operator==(const Point& o) const { return x == o.x; }
};

int main() {
    auto a = std::make_shared<Point>(1);
    auto b = std::make_shared<Point>(1);
    std::cout << (a == b) << std::endl;      // 0: different objects
    std::cout << (*a == *b) << std::endl;    // 1: equal values
    return 0;
}
