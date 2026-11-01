//@ cpp: compiles
//@ warns: none
// Run-time effect: sink destroys the object; the last line reads it.
#include <iostream>
#include <memory>
#include <vector>
#include <string>

struct Node {
    int v;
    explicit Node(int x) : v(x) {}
};

void sink(std::unique_ptr<Node>) {}

int main() {
    auto root = std::make_unique<Node>(1);
    Node& n = *root;
    sink(std::move(root));                  // the object is destroyed inside sink
    std::cout << n.v << std::endl;          // undefined behaviour
    return 0;
}
