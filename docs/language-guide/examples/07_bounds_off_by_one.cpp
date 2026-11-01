//@ cpp: compiles
//@ warns: none
// Compiles with no warning. At run time v[v.size()] reads one element past the end: undefined
// behaviour, and usually no crash, just a stale or garbage number.
#include <iostream>
#include <vector>

int main() {
    std::vector<int> v;
    v.push_back(10);
    v.push_back(20);
    v.push_back(30);
    std::cout << v[v.size()] << std::endl;
    return 0;
}
