//@ cpp: compiles
//@ warns: none
// Two nodes of a doubly linked list. Both links are shared_ptr, so the pair keeps itself alive.
#include <iostream>
#include <memory>

int alive = 0;                              // how many Nodes exist

struct Node {
    int v;
    std::shared_ptr<Node> next;
    std::shared_ptr<Node> prev;             // a back link that owns: nothing breaks the cycle
    explicit Node(int x) : v(x) { alive++; }
    ~Node() { alive--; }
};

int main() {
    {
        auto a = std::make_shared<Node>(1);
        a->next = std::make_shared<Node>(2);
        a->next->prev = a;                  // a -> next -> prev -> a
    }                                       // both handles leave scope here
    std::cout << "alive after the scope: " << alive << std::endl;   // prints 2: both nodes leaked
    return 0;
}
