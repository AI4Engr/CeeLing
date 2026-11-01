//@ cpp: compiles
//@ warns: none
// Three ways to hold "a Node" in C++, and nothing in the type says which one a given line means.
// The raw-pointer version below never calls delete: it compiles without a warning and leaks.
#include <iostream>

struct Node {
    int v;
    explicit Node(int x) : v(x) {}
};

int main() {
    Node a(1);
    Node b = a;                 // a copy: changing b does not change a
    b.v = 5;
    std::cout << a.v << b.v << std::endl;

    Node* p = new Node(1);      // a pointer: spelled differently, never freed here
    Node* q = p;                // an alias of the same object
    q->v = 5;
    std::cout << p->v << std::endl;
    return 0;
}
