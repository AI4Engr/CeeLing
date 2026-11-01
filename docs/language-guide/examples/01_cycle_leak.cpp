//@ cpp: compiles
//@ warns: none
#include <iostream>
#include <memory>

struct Node {
    int v;
    std::shared_ptr<Node> next;
    std::shared_ptr<Node> prev;            // should have been weak_ptr; nothing says so
    explicit Node(int x) : v(x) {}
};

int main() {
    auto a = std::make_shared<Node>(1);
    a->next = std::make_shared<Node>(2);
    a->next->prev = a;                     // a and its next now keep each other alive forever
    std::cout << a->next->v << std::endl;
    return 0;                              // neither node is ever destroyed: a leak
}
