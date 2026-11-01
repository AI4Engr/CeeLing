//@ cpp: compiles
//@ warns: none
// A reference to the object a field owns. Clearing the field destroys the object; the reference
// does not know. Compiles with no warning; reading c.v is undefined behaviour.
#include <iostream>
#include <memory>

struct Item {
    int v;
    std::unique_ptr<Item> child;
    explicit Item(int x) : v(x) {}
};

int main() {
    Item parent(1);
    parent.child = std::make_unique<Item>(42);
    Item& c = *parent.child;
    parent.child = nullptr;              // the object c refers to is destroyed
    std::cout << c.v << std::endl;
    return 0;
}
