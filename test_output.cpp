// Test file for two-pass smart pointer analysis

class Node {
public:
    int data;
    std::shared_ptr<Node> next;     // Should be unique_ptr (parent-child, Rule A)
    std::shared_ptr<Node> prev;     // Should be weak_ptr (back reference, Rule C)
    
    Node(int value) {
        data = value;
        next = nullptr;
        prev = nullptr;
    }
};

class LinkedList {
private:
    std::shared_ptr<Node> head;     // Should be unique_ptr (exclusive ownership, Rule A)
    
public:
    LinkedList() {
        head = nullptr;
    }
    
    void append(int value) {
        std::shared_ptr<Node> newNode = std::make_shared<Node>(value);  // Should be unique_ptr
        if (!head) {
            head = newNode;
        } else {
            std::shared_ptr<Node> temp = head;            // Should be shared_ptr (shared access, Rule B)
            while (temp->next) {
                temp = temp->next;        // Sharing access pattern
            }
            temp->next = newNode;
        }
    }
};

class Graph {
public:
    std::shared_ptr<Node> nodeA;    // Should be shared_ptr (potential sharing, Rule B)
    std::shared_ptr<Node> nodeB;    // Should be shared_ptr (potential sharing, Rule B)
    
    Graph() {
        nodeA = std::make_shared<Node>(1);
        nodeB = std::make_shared<Node>(2);
        nodeA->next = nodeB;  // A references B
        nodeB->next = nodeA;  // B references A (cycle!)
    }
};

int main() {
    std::shared_ptr<LinkedList> list = std::make_shared<LinkedList>();  // Should be unique_ptr (local ownership, Rule D)
    list->append(10);
    
    Graph graph = new Graph();           // Should be unique_ptr (local ownership, Rule D)
    
    return 0;
}
