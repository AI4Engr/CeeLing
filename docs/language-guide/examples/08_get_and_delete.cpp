//@ cpp: compiles
//@ warns: none
// .get() hands out the raw pointer inside a smart pointer; delete on it frees the object a second
// time when p is destroyed. Both are ordinary, well-typed C++. Compiles with no warning; the
// program has undefined behaviour (a double free).
#include <memory>

struct Node {
    int v;
    explicit Node(int x) : v(x) {}
};

int main() {
    auto p = std::make_unique<Node>(1);
    Node* raw = p.get();
    delete raw;
    return 0;
}
