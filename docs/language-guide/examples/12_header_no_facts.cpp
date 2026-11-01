//@ cpp: compiles
//@ warns: none
// Two "files" in one: the declaration is what a header would show, the definition lives in another .cpp.
// (Not run by the checker: it reads freed memory.)
#include <iostream>
#include <vector>

struct Node { int v; };

void growAll(std::vector<Node>& v);          // header: nothing says this may reallocate v

void growAll(std::vector<Node>& v) {         // library .cpp
    for (int i = 0; i < 1000; i++) v.push_back(Node{i});
}

int main() {
    std::vector<Node> v;
    v.push_back(Node{1});
    Node& first = v[0];
    growAll(v);
    std::cout << first.v << std::endl;       // first points into the old buffer
    return 0;
}
