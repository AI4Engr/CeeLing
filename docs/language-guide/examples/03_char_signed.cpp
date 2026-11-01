//@ cpp: compiles
//@ warns: none
// Run it: on this toolchain char is signed, so idx is -56 and the store lands 56 ints before the array.
#include <iostream>
#include <vector>

int main() {
    std::vector<char> data;
    data.push_back(static_cast<char>(200));   // a byte that came from a file or a socket
    int counts[256] = {0};
    int idx = data[0];                        // char is signed here: idx is -56, not 200
    std::cout << idx << std::endl;
    counts[idx] = 1;                          // writes before the start of the array
    std::cout << counts[0] << std::endl;
    return 0;
}
