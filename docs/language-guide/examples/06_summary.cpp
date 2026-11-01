//@ cpp: compiles
//@ warns: none
// Run-time effect: r is a reference to the first parameter's object, which x gave up.
#include <iostream>
#include <memory>
#include <vector>
#include <string>

struct Node {
    int v;
    explicit Node(int x) : v(x) {}
};

Node& pick(Node& a, Node&) { return a; }

int main() {
    auto x = std::make_unique<Node>(1);
    auto y = std::make_unique<Node>(2);
    Node& r = pick(*x, *y);                 // r is x's object
    x = std::make_unique<Node>(3);          // x's first object is destroyed
    std::cout << r.v << std::endl;          // undefined behaviour
    return 0;
}
