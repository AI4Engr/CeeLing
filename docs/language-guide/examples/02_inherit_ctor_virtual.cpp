//@ cpp: compiles
//@ warns: none
// While Animal's constructor runs, the object is only an Animal: the virtual call does not reach Dog.
// It prints "generic", although the object being built is a Dog. No warning.
#include <iostream>

struct Animal {
    Animal() { speak(); }
    virtual ~Animal() = default;
    virtual void speak() { std::cout << "generic" << std::endl; }
};

struct Dog : Animal {
    void speak() override { std::cout << "woof" << std::endl; }
};

int main() {
    Dog d;
    return 0;
}
