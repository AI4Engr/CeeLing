//@ cpp: compiles
//@ warns: none
// A file-scope variable belongs to no object and no function, so every function and every thread
// can reach it. Two threads add to it with no lock: a data race (undefined behaviour). The
// compiler says nothing.
#include <iostream>
#include <thread>

int counter = 0;

void work() {
    for (int i = 0; i < 100000; i++) { counter = counter + 1; }
}

int main() {
    std::thread t(work);
    work();
    t.join();
    std::cout << counter << std::endl;
    return 0;
}
