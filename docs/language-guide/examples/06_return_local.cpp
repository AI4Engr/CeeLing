//@ cpp: compiles
//@ warns: some
// Run-time effect: the caller receives a reference into a stack frame that has been popped.
#include <iostream>

struct Node {
    int v;
    explicit Node(int x) : v(x) {}
};

Node& makeDangling() {
    Node n(1);
    return n;                               // g++ -Wall warns about this direct form; it stays a warning
}

int main() {
    Node& r = makeDangling();
    std::cout << r.v << std::endl;          // undefined behaviour
    return 0;
}
