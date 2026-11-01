//@ cpp: compiles
//@ warns: none
#include <iostream>
#include <vector>

struct Node {
    int v;
    explicit Node(int x) : v(x) {}
};

int main() {
    std::vector<Node> v;
    v.push_back(Node(1));
    Node& n = v[0];
    v.push_back(Node(2));                  // reallocates: n now points at freed memory
    std::cout << n.v << std::endl;         // undefined behaviour
    return 0;
}
