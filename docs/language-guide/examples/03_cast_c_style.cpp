//@ cpp: compiles
//@ warns: none
#include <iostream>

struct Node {
    int v;
    explicit Node(int x) : v(x) {}
};

int main() {
    double d = 3.7;
    int i = (int)d;                      // a numeric conversion ...
    Node n(1);
    Node* p = &n;
    long long address = (long long)p;    // ... and the same syntax reinterprets a pointer as a number
    Node* q = (Node*)address;            // ... and back
    std::cout << i << " " << q->v << std::endl;
    return 0;
}
