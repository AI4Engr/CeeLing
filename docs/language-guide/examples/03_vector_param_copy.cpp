//@ cpp: compiles
//@ warns: none
// Run it: prints 0. The parameter is a whole copy of the caller's vector, made on every call.
#include <iostream>
#include <vector>

bool sharesStorage(std::vector<int> param, const std::vector<int>& caller) {
    return param.data() == caller.data();
}

int main() {
    std::vector<int> v = {1, 2, 3};
    std::cout << sharesStorage(v, v) << std::endl;
    return 0;
}
