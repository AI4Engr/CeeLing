//@ cpp: compiles
//@ warns: none
// Run it: prints 1 and the Button is never destroyed (run it under a leak checker). The callback owns the button, the button owns the callback.
#include <functional>
#include <iostream>
#include <memory>

struct Button {
    int clicks = 0;
    std::function<void()> onClick;
    ~Button() { std::cout << "destroyed" << std::endl; }
};

int main() {
    auto b = std::make_shared<Button>();
    b->onClick = [b] { b->clicks = b->clicks + 1; };   // captures a copy of the shared_ptr: a cycle
    b->onClick();
    std::cout << b->clicks << std::endl;
    return 0;                                          // "destroyed" is never printed
}
