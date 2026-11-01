//@ cpp: compiles
//@ warns: none
// A weak_ptr back link whose target is gone. lock() returns an empty pointer, and the next line uses it.
#include <iostream>
#include <memory>

struct Emitter {
    int count = 7;
};

struct Listener {
    std::weak_ptr<Emitter> owner;
};

int main() {
    Listener l;
    {
        auto e = std::make_shared<Emitter>();
        l.owner = e;
    }                                       // the Emitter dies here
    std::cout << l.owner.lock()->count << std::endl;   // lock() is empty: undefined behaviour
    return 0;
}
