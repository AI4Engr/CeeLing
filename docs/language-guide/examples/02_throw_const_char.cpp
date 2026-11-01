//@ cpp: compiles
//@ warns: none
// throw "text" throws a const char*, not a std::string, so catch (std::string) never matches and the
// program ends in std::terminate. No warning. (Not run by the guide.)
#include <iostream>
#include <string>

int main() {
    try {
        throw "bad thing";
    } catch (std::string e) {
        std::cout << "caught " << e << std::endl;
    }
    return 0;
}
