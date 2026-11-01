//@ cpp: compiles
//@ warns: none
// C++ can throw any type, so a handler for one type does not see another. The int is not a
// std::string and not a std::exception: it passes through catch (const std::string&) and ends the
// program in std::terminate.
#include <iostream>
#include <string>

int main() {
    try {
        throw 42;
    } catch (const std::string& e) {
        std::cout << "caught " << e << std::endl;
    }
    return 0;
}
