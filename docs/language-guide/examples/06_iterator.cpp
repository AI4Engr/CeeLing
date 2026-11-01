//@ cpp: compiles
//@ warns: none
// Run-time effect: the iterator is used after push_back may have reallocated.
#include <iostream>
#include <vector>

int main() {
    std::vector<int> v;
    v.push_back(1);
    auto it = v.begin();
    v.push_back(2);                         // invalidates it if the vector grew
    std::cout << *it << std::endl;          // undefined behaviour
    return 0;
}
