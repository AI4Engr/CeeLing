//@ cpp: compiles
//@ warns: none
#include <iostream>
#include <memory>

struct Node {
    int key;
    explicit Node(int k) : key(k) {}
};

void sink(std::unique_ptr<Node>) {}      // takes the object and destroys it

int main() {
    auto root = std::make_unique<Node>(1);
    Node& n = *root;                       // n looks at the object
    sink(std::move(root));                 // the object is destroyed here
    std::cout << n.key << std::endl;       // undefined behaviour: reads a destroyed object
    return 0;
}
