//@ cpp: compiles
//@ warns: none
// The last node of a list has no next. n->next->v dereferences an empty unique_ptr: undefined
// behaviour, and no warning.
#include <iostream>
#include <memory>

struct Node {
    std::unique_ptr<Node> next;
    int v;
    explicit Node(int x) : v(x) {}
};

int main() {
    auto n = std::make_unique<Node>(1);
    std::cout << n->next->v << std::endl;
    return 0;
}
