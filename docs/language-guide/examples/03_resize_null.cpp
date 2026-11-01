//@ cpp: compiles
//@ warns: none
// Run it: crashes. resize(3) fills the new slots with empty unique_ptrs, and nothing in the type says so.
#include <iostream>
#include <memory>
#include <vector>

struct Node {
    int v;
    explicit Node(int x) : v(x) {}
};

int main() {
    std::vector<std::unique_ptr<Node>> nodes;
    nodes.resize(3);
    std::cout << nodes[0]->v << std::endl;
    return 0;
}
