//@ cpp: compiles
//@ warns: none
// Run it: undefined behaviour. The lambda captured `this`, and the Widget is gone when the lambda runs.
#include <functional>
#include <iostream>
#include <memory>

struct Widget {
    int id = 7;
    std::function<void()> callback() {
        return [this] { std::cout << id << std::endl; };
    }
};

int main() {
    std::function<void()> cb;
    {
        auto w = std::make_unique<Widget>();
        cb = w->callback();
    }
    cb();
    return 0;
}
