//@ cpp: compiles
//@ warns: none
#include <iostream>
#include <memory>
#include <thread>

struct Counter {
    int n = 0;
};

int main() {
    auto c = std::make_shared<Counter>();
    std::thread t([c] {                        // the lambda holds its own copy of the shared_ptr: that copy is safe
        for (int i = 0; i < 100000; i++) c->n = c->n + 1;
    });
    for (int i = 0; i < 100000; i++) c->n = c->n + 1;   // both threads write the one Counter: a data race
    t.join();
    std::cout << c->n << std::endl;            // not 200000 in general; the standard gives no answer at all
    return 0;
}
