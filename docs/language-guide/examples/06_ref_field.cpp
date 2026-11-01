//@ cpp: compiles
//@ warns: none
// Run-time effect: the Holder outlives the object its member refers to.
#include <iostream>
#include <memory>
#include <vector>
#include <string>

struct Node {
    int v;
    explicit Node(int x) : v(x) {}
};

struct Holder {
    Node& r;                                // a reference stored in a longer-lived object
    explicit Holder(Node& n) : r(n) {}
};

int main() {
    auto a = std::make_unique<Node>(1);
    Holder h(*a);
    a.reset();                              // the object is gone, the holder still names it
    std::cout << h.r.v << std::endl;        // undefined behaviour
    return 0;
}
