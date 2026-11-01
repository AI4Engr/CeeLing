//@ cpp: compiles
//@ warns: none
#include <iostream>
#include <vector>

int main() {
    std::vector<int> v;
    v.push_back(1);
    v.push_back(2);
    int i = 5;
    std::cout << v[i] << std::endl;        // reads memory past the vector: undefined behaviour, usually no crash
    return 0;
}
