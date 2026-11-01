//@ cpp: compiles
//@ warns: none
#include <functional>
#include <iostream>

std::function<int(int)> adder(int base) {
    return [&](int x) { return base + x; };    // captures base by reference: it lives in adder's frame
}

int main() {
    auto add1000 = adder(1000);
    std::cout << add1000(1) << std::endl;      // undefined behaviour: base is gone
    return 0;
}
