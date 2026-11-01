//@ cpp: compiles
//@ warns: none
// Run it: undefined behaviour. The Holder keeps a reference to a vector that was a temporary.
#include <iostream>
#include <vector>

struct Holder {
    const std::vector<int>& data;
    explicit Holder(const std::vector<int>& d) : data(d) {}
};

std::vector<int> make() { return {1, 2, 3}; }

int main() {
    Holder h(make());                      // the temporary dies at the end of this statement
    std::cout << h.data[0] << std::endl;   // reads a destroyed vector
    return 0;
}
