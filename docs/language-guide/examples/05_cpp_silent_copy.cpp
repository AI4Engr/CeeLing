//@ cpp: compiles
//@ warns: none
// Forgetting std::move: the vector is copied, one million ints, on every call. Nothing says so.
#include <iostream>
#include <vector>

long long consume(std::vector<int> data) {      // by value: the caller's argument is copied unless it is moved in
    return (long long)data.size();
}

int main() {
    std::vector<int> big(1000000, 7);
    long long n = consume(big);                 // a copy; write std::move(big) to hand it over
    std::cout << n << " " << big.size() << std::endl;
    return 0;
}
