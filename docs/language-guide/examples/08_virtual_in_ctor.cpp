//@ cpp: compiles
//@ warns: none
// While Animal's constructor runs, the object is an Animal, not yet a Dog. speak() therefore
// prints "generic" even though a Dog is being built. Legal, silent, and not what the code says.
#include <iostream>

struct Animal {
    Animal() { speak(); }
    virtual ~Animal() {}
    virtual void speak() { std::cout << "generic" << std::endl; }
};

struct Dog : Animal {
    void speak() override { std::cout << "woof" << std::endl; }
};

int main() {
    Dog d;
    return 0;
}
