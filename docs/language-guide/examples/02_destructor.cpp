//@ cpp: compiles
//@ warns: none
// Each node's destructor destroys the next node, which destroys the next: one stack frame per node.
// With a million nodes the program prints its line and then crashes while freeing the list
// (not run by the guide; on the guide's toolchain it exits with a stack overflow).
#include <iostream>
#include <memory>

struct Item {
    std::unique_ptr<Item> next;
    int v;
    explicit Item(int x) : v(x) {}
};

int main() {
    std::unique_ptr<Item> head = std::make_unique<Item>(0);
    for (int i = 1; i < 1000000; i = i + 1) {
        auto n = std::make_unique<Item>(i);
        n->next = std::move(head);
        head = std::move(n);
    }
    std::cout << "built " << head->v << std::endl;
    return 0;
}
