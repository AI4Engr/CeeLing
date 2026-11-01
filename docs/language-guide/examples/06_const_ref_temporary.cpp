//@ cpp: compiles
//@ warns: some
// Run-time effect: binding a const reference to a temporary extends its life, but only when it is bound directly.
// Here the reference comes back out of a function, so the temporaries it points into die at the semicolon.
#include <iostream>
#include <string>

const std::string& longer(const std::string& a, const std::string& b) {
    return a.size() >= b.size() ? a : b;
}

int main() {
    const std::string& s = longer(std::string("short"), std::string("longer one"));
    std::cout << s << std::endl;            // undefined behaviour
    return 0;
}
