//@ cpp: compiles
//@ warns: none
// std::copy requires that the destination does not start inside the source range: this is undefined behaviour.
// g++ happens to print "1 2 1 2 0 0 0" here, another library or optimisation level may print something else.
#include <algorithm>
#include <iostream>
#include <vector>

int main() {
    std::vector<int> v = {1, 2, 0, 0, 0, 0, 0};
    std::copy(v.begin(), v.begin() + 5, v.begin() + 2);
    for (int x : v) std::cout << x << " ";
    std::cout << std::endl;
    return 0;
}
