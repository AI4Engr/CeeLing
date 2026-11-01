//@ cpp: compiles
//@ warns: none
// Run-time effect: the object n looks at was destroyed by the assignment inside the try block.
#include <iostream>
#include <memory>
#include <vector>
#include <string>

struct Node {
    int v;
    explicit Node(int x) : v(x) {}
};

int main() {
    auto a = std::make_shared<Node>(1);
    Node& n = *a;
    try {
        a = std::make_shared<Node>(2);      // destroys the first object
        throw std::string("failed");
    } catch (const std::string& e) {
        std::cout << e << std::endl;
    }
    std::cout << n.v << std::endl;          // undefined behaviour
    return 0;
}
