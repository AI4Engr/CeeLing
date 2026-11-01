//@ cpp: compiles
//@ warns: none
// Run it: prints "no pears" and then 2. The read of "pear" inserted an entry.
#include <iostream>
#include <map>
#include <string>

int main() {
    std::map<std::string, int> stock;
    stock["apple"] = 3;
    if (stock["pear"] == 0) {
        std::cout << "no pears" << std::endl;
    }
    std::cout << stock.size() << std::endl;
    return 0;
}
