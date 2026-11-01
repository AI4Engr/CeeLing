//@ cpp: compiles
//@ warns: none
#include <iostream>
#include <memory>

struct Node {
    std::unique_ptr<Node> next;
    int v;
    explicit Node(int x) : v(x) {}
};

int main() {
    auto n = std::make_unique<Node>(1);
    std::cout << n->v << std::endl;
    std::cout << n->next->v << std::endl;  // next is empty: undefined behaviour
    return 0;
}
