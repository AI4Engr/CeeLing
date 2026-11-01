//@ cpp: compiles
//@ warns: none
#include <thread>

void start() {
    int hits = 0;
    std::thread t([&] { hits = hits + 1; });   // captures the local by reference
    t.detach();                                // start() returns, hits is gone, the thread may still write to it
}

int main() {
    start();
    return 0;
}
