//@ cpp: compiles
//@ warns: none
// A raw pointer remembers the address of a local that is about to die. The store goes through a
// function, so the compiler does not see a local and a global side by side; no warning. Reading
// keep->v afterwards is undefined behaviour.
#include <iostream>

struct Node {
    int v;
};

Node* keep = nullptr;

void remember(Node& n) { keep = &n; }

void setup() {
    Node local{42};
    remember(local);
}                                           // local is destroyed here

int main() {
    setup();
    std::cout << keep->v << std::endl;
    return 0;
}
