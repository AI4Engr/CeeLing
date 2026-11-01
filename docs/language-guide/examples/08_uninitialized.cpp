//@ cpp: compiles
//@ warns: none
// n is declared and never given a value; reading it is undefined behaviour. g++ -Wall -Wextra
// stays silent when the read is behind a call and a branch, as it usually is in real code.
#include <iostream>

int pick(bool first) {
    int n;
    if (first) { n = 1; }
    return n;
}

int main() {
    std::cout << pick(false) << std::endl;
    return 0;
}
