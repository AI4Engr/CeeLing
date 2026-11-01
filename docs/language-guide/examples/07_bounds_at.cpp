//@ cpp: compiles
//@ warns: none
// C++ does have a checked access: .at() throws std::out_of_range. It is opt-in, spelled
// differently from the subscript everybody writes, and being an exception it can be swallowed.
#include <iostream>
#include <vector>

int main() {
    std::vector<int> v;
    v.push_back(1);
    try {
        std::cout << v.at(5) << std::endl;
    } catch (...) {
        std::cout << "ignored" << std::endl;
    }
    std::cout << "carried on" << std::endl;
    return 0;
}
