//@ cpp: compiles
//@ warns: none
// Run-time effect: push_back inside a range-for over the same vector invalidates the loop's own iterators.
#include <iostream>
#include <vector>

int main() {
    std::vector<int> v;
    v.push_back(1);
    v.push_back(2);
    for (int x : v) {
        v.push_back(x);                     // undefined behaviour once the vector reallocates
    }
    std::cout << v.size() << std::endl;
    return 0;
}
