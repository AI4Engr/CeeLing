//@ cpp: compiles
//@ warns: none
// A move inside a loop: the first round takes the string, every later round sees an empty one.
#include <iostream>
#include <string>
#include <vector>

int main() {
    std::string line = "hello";
    std::vector<std::string> lines;
    for (int i = 0; i < 3; i++) {
        lines.push_back(std::move(line));       // round 1 moves "hello" out; rounds 2 and 3 push ""
    }
    for (const auto& s : lines) std::cout << "[" << s << "]";
    std::cout << std::endl;                     // [hello][][] with libstdc++
    return 0;
}
