//@ cpp: compiles
//@ warns: none
#include <iostream>
#include <thread>
#include <vector>

int main() {
    std::vector<int> v;
    std::thread t([&v] {
        for (int i = 0; i < 1000; i++) v.push_back(i);
    });
    for (int i = 0; i < 1000; i++) v.push_back(i);   // two threads reallocating one vector: heap corruption is possible
    t.join();
    std::cout << v.size() << std::endl;
    return 0;
}
