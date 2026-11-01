//@ cpp: compiles
//@ warns: none
// A method that hands out `this`. Wrapping the raw pointer in a second shared_ptr starts a second
// reference count for the same object, so the object is deleted twice.
#include <iostream>
#include <memory>

struct Node {
    int v;
    explicit Node(int x) : v(x) {}
    std::shared_ptr<Node> self() { return std::shared_ptr<Node>(this); }   // wrong: should derive from enable_shared_from_this
};

int main() {
    auto a = std::make_shared<Node>(1);
    std::shared_ptr<Node> s = a->self();
    std::cout << s->v << std::endl;
    return 0;                               // a and s each delete the Node: double free
}
