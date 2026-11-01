//@ cpp: compiles
//@ warns: none
// Run-time effect: after the second push_back the vector may have moved its elements; first points at the old storage.
#include <iostream>
#include <vector>

int main() {
    std::vector<int> v;
    v.push_back(1);
    int& first = v[0];
    v.push_back(2);                         // may reallocate
    std::cout << first << std::endl;        // undefined behaviour if it did
    return 0;
}
