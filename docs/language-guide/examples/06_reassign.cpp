//@ cpp: compiles
//@ warns: none
// Run-time effect: the last line reads an object that the assignment above it destroyed.
#include <iostream>
#include <memory>
#include <vector>
#include <string>

struct Node {
    int v;
    explicit Node(int x) : v(x) {}
};

int main() {
    auto a = std::make_shared<Node>(1);
    Node& n = *a;                           // n looks at the first object
    a = std::make_shared<Node>(2);          // the first object is destroyed here
    std::cout << n.v << std::endl;          // undefined behaviour
    return 0;
}
