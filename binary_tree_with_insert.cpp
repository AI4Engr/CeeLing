#include <memory>
#include <iostream>

class Node {
public:
    int data;
    std::shared_ptr<Node> left;
    std::shared_ptr<Node> right;

    Node(int value)
    {
      data = value;
      left = nullptr;
      right = nullptr;
    }

    void insert(int value) {
        if (value < data) {
            if (left) {
                left->insert(value);
            } else {
                left = std::make_shared<Node>(value);
            }
        } else {
            if (right) {
                right->insert(value);
            } else {
                right = std::make_shared<Node>(value);
            }
        }
    }

    void print() {
        if (left) {
            left->print();
        }
        std::cout << data << " ";
        if (right) {
            right->print();
        }
    }
};

int main()
{
    std::shared_ptr<Node> test = std::make_shared<Node>(42);

    //std::cout << test->find(35);
    return 0;
}