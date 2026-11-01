//@ cpp: compiles
//@ warns: none
// Run it: the output is garbage or a crash. The lambda keeps a reference to `clicks`, which died when setup() returned.
#include <functional>
#include <iostream>

struct Button {
    std::function<void()> onClick;
};

void setup(Button& b) {
    int clicks = 0;
    b.onClick = [&clicks] { clicks = clicks + 1; std::cout << clicks << std::endl; };
}

int main() {
    Button b;
    setup(b);
    b.onClick();
    return 0;
}
