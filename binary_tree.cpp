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

    bool find(int value) {
        if (value == data) {
            return true;
        } else if (value < data) {
            if(left) {
                return left->find(value);
            }
            else {
                return false;
            }
        } else {
            if(right) {
                return right->find(value);
            }
            else {
                return false;
            }
        }
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
    test->insert(50);
    test->insert(15);
    test->insert(7);
    test->print();

    std::cout << test->find(35);
    return 0;
}