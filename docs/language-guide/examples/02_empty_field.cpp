//@ cpp: compiles
//@ warns: none
// A pointer field nobody initialised holds whatever was in that memory. Testing it against nullptr
// then reads an indeterminate value (undefined behaviour); the program below is not run by the guide.
#include <iostream>

struct Item {
    int v;
    Item* next;                    // the constructor forgets it
    explicit Item(int x) : v(x) {}
};

int main() {
    Item* a = new Item(1);
    if (a->next == nullptr) {      // may be true, false, or a crash
        std::cout << "last" << std::endl;
    }
    delete a;
    return 0;
}
