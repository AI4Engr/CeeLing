//@ cpp: compiles
//@ warns: none
#include <iostream>
#include <memory>

struct Node {
    int v;
    explicit Node(int x) : v(x) {}
};

int main() {
    Node* raw = nullptr;
    {
        auto p = std::make_unique<Node>(1);
        raw = p.get();                     // a raw pointer: the smart pointer no longer knows about it
    }
    std::cout << raw->v << std::endl;      // undefined behaviour: the object died with p
    return 0;
}
