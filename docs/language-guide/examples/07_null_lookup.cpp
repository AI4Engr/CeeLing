//@ cpp: compiles
//@ warns: none
// find() returns an empty shared_ptr when nothing matches. The type of the result is the same
// whether it is empty or not, so nothing makes the caller test it. hit->v on an empty pointer is
// undefined behaviour.
#include <iostream>
#include <memory>

struct Node {
    int v;
    explicit Node(int x) : v(x) {}
};

std::shared_ptr<Node> find(std::shared_ptr<Node> n, int key) {
    if (n->v == key) { return n; }
    return nullptr;
}

int main() {
    auto a = std::make_shared<Node>(5);
    auto hit = find(a, 9);
    std::cout << hit->v << std::endl;
    return 0;
}
