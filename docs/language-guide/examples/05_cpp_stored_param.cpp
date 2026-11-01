//@ cpp: compiles
//@ warns: none
// Storing a by-value parameter into a field without std::move copies it a second time.
#include <iostream>
#include <vector>

struct Holder {
    std::vector<int> data;
    void take(std::vector<int> bytes) { data = bytes; }              // copy: bytes is a local, and this is its last use
    void takeMoved(std::vector<int> bytes) { data = std::move(bytes); }
};

int main() {
    Holder h;
    h.take(std::vector<int>(1000000, 1));
    h.takeMoved(std::vector<int>(1000000, 2));
    std::cout << h.data.size() << std::endl;
    return 0;
}
