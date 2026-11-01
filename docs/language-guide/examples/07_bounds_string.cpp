//@ cpp: compiles
//@ warns: none
// s[3] on "abc" is legal C++ (it is the terminating '\0'), s[10] is not: undefined behaviour.
// Neither draws a warning.
#include <iostream>
#include <string>

int main() {
    std::string s = "abc";
    int i = 10;
    std::cout << int(s[3]) << std::endl;
    std::cout << s[i] << std::endl;
    return 0;
}
