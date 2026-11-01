//@ cpp: compiles
//@ warns: none
#include <iostream>
#include <utility>
#include <vector>

void consume(std::vector<int> v) { (void)v; }

int main() {
    std::vector<int> data{1, 2, 3};
    consume(std::move(data));          // data was moved from
    std::cout << data.size() << std::endl;   // legal: a valid but unspecified "empty shell"; the compiler does not object
    return 0;
}
