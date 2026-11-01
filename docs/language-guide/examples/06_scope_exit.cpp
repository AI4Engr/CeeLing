//@ cpp: compiles
//@ warns: none
// Run-time effect: the last line reads an object that died at the closing brace.
#include <iostream>
#include <memory>
#include <vector>
#include <string>

struct Node {
    int v;
    explicit Node(int x) : v(x) {}
};

int main() {
    Node* n = nullptr;
    {
        auto inner = std::make_unique<Node>(1);
        n = inner.get();
    }                                       // inner is destroyed here
    std::cout << n->v << std::endl;         // undefined behaviour
    return 0;
}
