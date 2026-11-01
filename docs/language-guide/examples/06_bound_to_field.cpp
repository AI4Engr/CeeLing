//@ cpp: compiles
//@ warns: none
// Run-time effect: c names the object the field held; clearing the field destroyed it.
#include <iostream>
#include <memory>
#include <vector>
#include <string>

struct Node {
    int v;
    explicit Node(int x) : v(x) {}
};

struct Item {
    int v;
    std::unique_ptr<Item> child;
    explicit Item(int x) : v(x) {}
};

int main() {
    Item parent(1);
    parent.child = std::make_unique<Item>(42);
    Item& c = *parent.child;
    parent.child = nullptr;                 // destroys the child
    std::cout << c.v << std::endl;          // undefined behaviour
    return 0;
}
