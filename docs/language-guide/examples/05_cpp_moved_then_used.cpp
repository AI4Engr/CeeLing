//@ cpp: compiles
//@ warns: none
// Moving too early: the source is still a name you can use. For std::vector and std::string it is
// usually empty afterwards, but the standard only promises "valid but unspecified".
#include <iostream>
#include <string>
#include <vector>

void consume(std::vector<std::string> v) { std::cout << "consumed " << v.size() << std::endl; }

int main() {
    std::vector<std::string> names;
    names.push_back("a");
    names.push_back("b");
    consume(std::move(names));
    std::cout << names.size() << std::endl;     // compiles; prints 0 with libstdc++, but nothing requires it
    names.push_back("c");                       // and the object is still usable, which hides the bug
    return 0;
}
