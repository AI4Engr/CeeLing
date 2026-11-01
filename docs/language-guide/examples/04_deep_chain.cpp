//@ cpp: compiles
//@ warns: none
// A list of one million nodes, each owning the next. Building it works. Destroying it does not:
// the destructor of a node destroys its `next`, which destroys its `next`, one stack frame each.
#include <iostream>
#include <memory>

struct Item {
    std::unique_ptr<Item> next;
    int v;
    explicit Item(int x) : v(x) {}
};

int main() {
    auto head = std::make_unique<Item>(0);
    for (int i = 1; i < 1000000; i++) {
        auto n = std::make_unique<Item>(i);
        n->next = std::move(head);
        head = std::move(n);
    }
    std::cout << "built " << head->v << std::endl;
    return 0;                               // head is destroyed here: stack overflow, the process dies
}
