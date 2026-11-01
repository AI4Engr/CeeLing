//@ cpp: compiles
//@ warns: none
#include <memory>
#include <thread>

struct Counter {
    int n = 0;
};

int main() {
    auto c = std::make_shared<Counter>();
    std::thread t([c] { c->n = c->n + 1; });   // the copy of the shared_ptr is safe, the object it points to is not
    c->n = c->n + 1;                           // data race: two threads, one object, no lock
    t.join();
    return 0;
}
