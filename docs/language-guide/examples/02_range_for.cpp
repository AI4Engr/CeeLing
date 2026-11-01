//@ cpp: compiles
//@ warns: none
// Forgetting the & in a range-for copies every element, and the loop body edits the copy.
// The program compiles without a warning and prints 1 2.
#include <iostream>
#include <vector>

int main() {
    std::vector<int> v{1, 2};
    for (auto x : v) { x = x + 10; }       // should have been auto&
    std::cout << v[0] << " " << v[1] << std::endl;
    return 0;
}
