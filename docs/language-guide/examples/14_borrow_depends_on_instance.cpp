//@ cpp: compiles
//@ warns: none
// One template, two instantiations: with the first, call returns a reference into its argument; with the second, a copy.
// A signature that says which has to be written per instantiation; one annotation on the template cannot.
// Run-time effect: none as written; keeping the first result past the life of the argument would be undefined behaviour.
#include <iostream>

template <class F>
decltype(auto) call(F f, int& x) { return f(x); }

int main() {
    int a = 1;
    auto byRef = [](int& x) -> int& { return x; };
    auto byValue = [](int& x) { return x; };
    int& r1 = call(byRef, a);               // aliases a
    int r2 = call(byValue, a);              // independent of a
    a = 5;
    std::cout << r1 << " " << r2 << std::endl;
    return 0;
}
