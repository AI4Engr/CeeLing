//@ cpp: compiles
//@ warns: none
// shared_from_this() needs the object to be owned by a shared_ptr already; inside the
// constructor it never is yet, so it throws std::bad_weak_ptr. The compiler cannot see that.
#include <memory>
#include <vector>

struct Node;

struct Registry {
    std::vector<std::shared_ptr<Node>> all;
};

struct Node : std::enable_shared_from_this<Node> {
    explicit Node(Registry& r) { r.all.push_back(shared_from_this()); }
};

int main() {
    Registry reg;
    auto n = std::make_shared<Node>(reg);      // throws std::bad_weak_ptr
    return 0;
}
