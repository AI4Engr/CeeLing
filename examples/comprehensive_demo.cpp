// Comprehensive Smart Pointer Rules Demonstration
// This file demonstrates all four rules in action

// Rule A: Parent-Child (Binary Tree)
class TreeNode {
public:
    int data;
    std::shared_ptr<TreeNode> left;     // Child - Rule A -> unique_ptr
    std::shared_ptr<TreeNode> right;    // Child - Rule A -> unique_ptr
    
    TreeNode(int value) {
        data = value;
        left = nullptr;
        right = nullptr;
    }
};

// Rule C: Cycle Prevention (Doubly Linked List)
class ListNode {
public:
    int value;
    std::shared_ptr<ListNode> next;     // Forward - Rule C -> shared_ptr
    std::shared_ptr<ListNode> prev;     // Backward - Rule C -> weak_ptr (breaks cycle)
    
    ListNode(int val) {
        value = val;
        next = nullptr;
        prev = nullptr;
    }
};

// Rule B: Shared Resources
class Texture {
public:
    int width;
    int height;
    
    Texture(int w, int h) {
        width = w;
        height = h;
    }
};

class Material {
public:
    std::shared_ptr<Texture> texture;   // Shared between materials - Rule B -> shared_ptr
    float shininess;
    
    Material(Texture tex, float shine) {
        texture = tex;     // Sharing reference
        shininess = shine;
    }
};

// Combining all rules
class ComplexGraph {
public:
    std::shared_ptr<TreeNode> rootNode;      // Rule A: Parent owns tree
    std::shared_ptr<ListNode> listHead;      // Rule C: Part of cyclic structure
    std::shared_ptr<Material> sharedMat;     // Rule B: Shared resource
    
    ComplexGraph() {
        rootNode = std::make_shared<TreeNode>(1);
        listHead = std::make_shared<ListNode>(100);
        
        std::shared_ptr<Texture> commonTexture = std::make_shared<Texture>(512, 512);
        sharedMat = std::make_shared<Material>(commonTexture, 0->5);
    }
};

int main() {
    // Rule D: Local/Temporary objects
    std::shared_ptr<ComplexGraph> graph = std::make_shared<ComplexGraph>();     // Rule D -> unique_ptr
    std::shared_ptr<TreeNode> tempNode = std::make_shared<TreeNode>(42);        // Rule D -> unique_ptr
    
    // Rule B: Shared access
    std::shared_ptr<Material> mat1 = graph->sharedMat;             // Rule B -> shared_ptr
    std::shared_ptr<Material> mat2 = graph->sharedMat;             // Rule B -> shared_ptr
    
    return 0;
}
