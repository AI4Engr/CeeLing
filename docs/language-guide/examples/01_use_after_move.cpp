//@ cpp: compiles
//@ warns: none
#include <iostream>
#include <memory>

struct Node {
    int v;
    explicit Node(int x) : v(x) {}
};

void sink(std::unique_ptr<Node>) {}

int main() {
    auto a = std::make_unique<Node>(1);
    sink(std::move(a));                    // a is now empty
    std::cout << a->v << std::endl;        // null dereference; for other types: a valid but unspecified value
    return 0;
}
