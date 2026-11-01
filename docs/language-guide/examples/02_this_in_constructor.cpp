//@ cpp: compiles
//@ warns: none
// Handing out a shared_ptr to "this" from inside the constructor compiles without a warning and throws
// std::bad_weak_ptr when it runs: no shared_ptr owns the object yet. (Not run by the guide.)
#include <memory>

struct Registry {
    void add(std::shared_ptr<struct Widget>) {}
};

struct Widget : std::enable_shared_from_this<Widget> {
    Widget() {
        Registry reg;
        reg.add(shared_from_this());
    }
};

int main() {
    auto w = std::make_shared<Widget>();
    return 0;
}
