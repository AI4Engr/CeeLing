//@ cpp: compiles
//@ warns: none
// A move on one path only: after the if, the object may or may not have been emptied.
#include <iostream>
#include <memory>

struct Node {
    int v;
    explicit Node(int x) : v(x) {}
};

void consume(std::unique_ptr<Node>) {}

int main(int argc, char**) {
    auto n = std::make_unique<Node>(1);
    if (argc > 0) {
        consume(std::move(n));                  // taken whenever the program has a name
    }
    std::cout << n->v << std::endl;             // null dereference on that path
    return 0;
}
