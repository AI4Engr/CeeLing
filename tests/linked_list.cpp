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

    // Function to add a new node at the end
    void append(int value) {
        std::shared_ptr<Node> newNode = std::make_shared<Node>(value);
        if (!head) {
            head = newNode;
        } else {
            std::shared_ptr<Node> temp = head;
            while (temp->next) {
                temp = temp->next;
            }
            temp->next = newNode;
        }
    }

    // Function to delete a node by value
    void deleteNode(int value) {
        if (!head) {
		    return;
	   }
	   
        if (head->data == value) {
            head = head->next;
            return;
        }

        std::shared_ptr<Node> temp = head;
        while (temp->next && temp->next->data != value) {
            temp = temp->next;
        }

        if (temp->next) {
            temp->next = temp->next->next;
        }
    }

    // Function to display the linked list
    void display() const {
        std::shared_ptr<Node> temp = head;
        while (temp) {
            std::cout << temp->data << " -> ";
            temp = temp->next;
        }
        std::cout << "nullptr\n";
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

    list->append(10);
    list->append(20);
    list->append(30);
    list->prepend(0);

    std::cout << "Linked List: ";
    list->display();

    list->deleteNode(20);
    std::cout << "After deleting 20: ";
    list->display();

    list->deleteNode(0);
    std::cout << "After deleting 0: ";
    list->display();

    return 0;

    return 0;
}
