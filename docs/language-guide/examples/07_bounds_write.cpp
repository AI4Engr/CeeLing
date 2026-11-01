//@ cpp: compiles
//@ warns: none
// Compiles with no warning. The store lands outside the vector's buffer and corrupts the heap:
// undefined behaviour, and the damage usually shows up later, somewhere else.
#include <iostream>
#include <vector>

int main() {
    std::vector<int> v;
    v.push_back(1);
    int i = 100;
    v[i] = 9;
    std::cout << "done" << std::endl;
    return 0;
}
