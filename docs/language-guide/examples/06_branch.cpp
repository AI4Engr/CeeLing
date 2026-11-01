//@ cpp: compiles
//@ warns: none
// Run-time effect: when cond is true, obs names b, which was destroyed at the end of the if block.
#include <iostream>
#include <memory>
#include <vector>
#include <string>

struct Node {
    int v;
    explicit Node(int x) : v(x) {}
};

int main() {
    Node a(1);
    Node* obs = &a;
    bool cond = true;
    if (cond) {
        Node b(2);
        obs = &b;
    }                                       // b is destroyed here
    std::cout << obs->v << std::endl;       // right when cond is false, undefined behaviour when it is true
    return 0;
}
