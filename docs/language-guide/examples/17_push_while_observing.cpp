//@ cpp: compiles
//@ warns: none
// Run-time effect: undefined behaviour. The first element's address is kept across a push_back that may reallocate.
#include <iostream>
#include <vector>

int main() {
    std::vector<int> scores;
    scores.push_back(10);
    int& best = scores[0];             // a reference into the vector's storage
    scores.push_back(20);              // may move the storage
    std::cout << best << std::endl;    // reads freed memory if it did
    return 0;
}
