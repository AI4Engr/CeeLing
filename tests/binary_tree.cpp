#include <iostream>
#include <memory>

// Node class
class Node {
public:
    int data;
    std::shared_ptr<Node> next;
    Node(int value) {
        data = value;
        next = nullptr;
	}
};

// LinkedList class
class LinkedList {
private:
    std::shared_ptr<Node> head;

public:
    LinkedList() {
	    head = nullptr;
	}

    // Function to add a new node at the beginning
    void prepend(int value) {
        std::shared_ptr<Node> newNode = std::make_shared<Node>(value);
        newNode->next = head;
        head = newNode;
    }
};

int main() {
    std::shared_ptr<LinkedList> list = std::make_shared<LinkedList>();

    list->prepend(0);

    return 0;
}
