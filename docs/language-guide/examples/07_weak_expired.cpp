//@ cpp: compiles
//@ warns: none
// A back link kept as weak_ptr does not keep its target alive. When the target is gone, lock()
// returns an empty pointer, and the -> on it is undefined behaviour. (With a raw Node* for the
// back link it would be a read of freed memory instead.)
#include <iostream>
#include <memory>

struct Node {
    int v;
    std::shared_ptr<Node> next;
    std::weak_ptr<Node> prev;
    explicit Node(int x) : v(x) {}
};

std::shared_ptr<Node> tailOf() {
    auto first = std::make_shared<Node>(1);
    first->next = std::make_shared<Node>(2);
    first->next->prev = first;
    return first->next;                // first is destroyed here: only its next survives
}

int main() {
    auto tail = tailOf();
    std::cout << tail->v << std::endl;
    std::cout << tail->prev.lock()->v << std::endl;
    return 0;
}
