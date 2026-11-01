//@ cpp: compiles
//@ warns: none
// Compiles with no warning. The int -1 is converted to size_t, a huge number, and operator[]
// computes buffer + that offset: in practice the 8 bytes before the buffer. Undefined behaviour.
#include <iostream>
#include <vector>

int main() {
    std::vector<int> v;
    v.push_back(1);
    int i = 0 - 1;
    std::cout << v[i] << std::endl;
    return 0;
}
