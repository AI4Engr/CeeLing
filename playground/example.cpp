// Basic Function Example
#include <iostream>
#include <memory>

double b = 15.0;
int a = 1;

class Node {
public:
    int data;
    std::shared_ptr<Node> next;
    Node(int value) {
        data = value;
        next = nullptr;
    }
};

int main() {
    std::shared_ptr<Node> node = std::make_shared<Node>(10);
    return 0;
}