//@ cpp: compiles
//@ warns: none
// Run-time effect: the second line reads an object the assignment destroyed. The two references have the same type.
#include <iostream>
#include <memory>

struct Node {
    int v;
    explicit Node(int x) : v(x) {}
};

int main() {
    auto a = std::make_shared<Node>(1);
    Node& fine = *a;
    std::cout << fine.v << std::endl;       // fine: a still owns the object
    Node& stale = *a;
    a = std::make_shared<Node>(2);          // the object stale names is destroyed
    std::cout << stale.v << std::endl;      // undefined behaviour; same type as fine
    return 0;
}
