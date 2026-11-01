//@ cpp: compiles
//@ warns: some
// Deleting a Dog through an Animal* when Animal's destructor is not virtual is undefined behaviour:
// Dog's destructor (and its members) may never run. g++ -Wall happens to warn about the delete here;
// with a smart pointer built from a raw base pointer the same mistake compiles in silence.
#include <iostream>
#include <string>

struct Animal {
    virtual void speak() { std::cout << "..." << std::endl; }
};

struct Dog : Animal {
    std::string toy = "ball";
    void speak() override { std::cout << "woof" << std::endl; }
};

int main() {
    Animal* a = new Dog();
    a->speak();
    delete a;
    return 0;
}
