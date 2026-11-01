//@ cpp: compiles
//@ warns: none
// Run-time effect: the view points into the temporary string, which died at the semicolon; the output is garbage.
#include <iostream>
#include <string>
#include <string_view>

std::string make() {
    return "a string long enough to live on the heap, not in the string object";
}

int main() {
    std::string_view sv = make();           // the temporary is destroyed at the end of this statement
    std::cout << sv << std::endl;           // undefined behaviour
    return 0;
}
