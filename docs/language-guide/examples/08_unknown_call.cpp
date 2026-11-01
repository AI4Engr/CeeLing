//@ cpp: compiles
//@ warns: none
// firstWord comes from "a library": all the compiler sees is its declaration. The result points
// into the string that was passed in, which here is a temporary, gone at the end of the
// statement. Nothing in the declaration says so, and the compiler does not ask. No warning;
// printing w.text is undefined behaviour.
#include <iostream>
#include <string>

struct View {
    const char* text;
};

View firstWord(const std::string& s) { return View{s.c_str()}; }

int main() {
    View w = firstWord(std::string("hello world, from a temporary string of some length"));
    std::cout << w.text << std::endl;
    return 0;
}
