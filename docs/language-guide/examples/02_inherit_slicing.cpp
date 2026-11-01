//@ cpp: compiles
//@ warns: none
// A base-class PARAMETER taken by value copies only the base part of the argument: the Dog is sliced
// into an Animal, and the call prints the base's answer. The program compiles without a warning and
// prints "..." for both lines.
#include <iostream>
#include <string>
#include <vector>

struct Animal {
    virtual ~Animal() = default;
    virtual std::string sound() const { return "..."; }
};

struct Dog : Animal {
    std::string sound() const override { return "woof"; }
};

void describe(Animal a) { std::cout << a.sound() << std::endl; }

int main() {
    Dog d;
    describe(d);                       // prints ...
    std::vector<Animal> zoo;
    zoo.push_back(d);                  // sliced again
    std::cout << zoo[0].sound() << std::endl;
    return 0;
}
