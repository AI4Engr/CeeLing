# Smart Pointer Rules for CeeLing-to-C++ Conversion

## Overview

CeeLing uses a **two-pass analysis** to intelligently convert class types to appropriate C++ smart pointers when transpiling `.clg` files to `.cpp` files. This ensures memory safety, prevents leaks, and optimizes performance.

## 🔍 Two-Pass Analysis Strategy

### **Pass 1: Collect Reference Relationships**
- Identifies all class types and variables
- Maps ownership relationships between classes
- Records assignment patterns and dependencies
- Builds a complete reference graph

### **Pass 2: Apply Smart Pointer Rules**
- Analyzes ownership patterns
- Detects potential cycles
- Applies rules in priority order: C → B → A → D
- Generates appropriate smart pointer types

---

## 📋 Smart Pointer Rules

### **Rule A: Parent-Child Relationship (Tree Structure)**

**Condition:** A child object is owned by only **one parent**.

**Pointer:** `unique_ptr`

**CLG Input:**
```cpp
class Node {
public:
    int data;
    Node left;     // Child owned exclusively by parent
    Node right;    // Child owned exclusively by parent
    
    Node(int value) {
        data = value;
        left = nullptr;
        right = nullptr;
    }
};
```

**Generated C++:**
```cpp
class Node {
public:
    int data;
    std::unique_ptr<Node> left;     // ✅ Rule A: Parent-child
    std::unique_ptr<Node> right;    // ✅ Rule A: Parent-child
    
    Node(int value) {
        data = value;
        left = nullptr;
        right = nullptr;
    }
    
    void insert(int value) {
        if (value < data) {
            if (!left) {
                left = std::make_unique<Node>(value);  // ✅ make_unique
            } else {
                left->insert(value);
            }
        } else {
            if (!right) {
                right = std::make_unique<Node>(value); // ✅ make_unique
            } else {
                right->insert(value);
            }
        }
    }
};
```

**Analysis Output:**
```
PARENT-CHILD: Node.left -> unique_ptr (Rule A)
PARENT-CHILD: Node.right -> unique_ptr (Rule A)
```

**Explanation:**
- Each child node has exactly one parent
- Parent has exclusive ownership
- Automatic deletion when parent is destroyed
- Optimal performance (no reference counting)

---

### **Rule B: Shared Objects (Multiple References)**

**Condition:** An object can be referenced by **multiple owners**.

**Pointer:** `shared_ptr`

**CLG Input:**
```cpp
class Texture {
public:
    string filename;
    int width, height;
    
    Texture(string file) {
        filename = file;
        width = 1024;
        height = 768;
    }
};

class Sprite {
public:
    Texture texture;   // Multiple sprites can share same texture
    int x, y;
    
    Sprite(Texture tex, int posX, int posY) {
        texture = tex;     // Sharing reference
        x = posX;
        y = posY;
    }
};

int main() {
    Texture globalTexture = new Texture("player.png");
    
    Sprite player1 = new Sprite(globalTexture, 100, 200);  // Shares texture
    Sprite player2 = new Sprite(globalTexture, 300, 400);  // Shares texture
    
    return 0;
}
```

**Generated C++:**
```cpp
class Texture {
public:
    std::string filename;
    int width, height;
    
    Texture(std::string file) {
        filename = file;
        width = 1024;
        height = 768;
    }
};

class Sprite {
public:
    std::shared_ptr<Texture> texture;   // ✅ Rule B: Shared ownership
    int x, y;
    
    Sprite(std::shared_ptr<Texture> tex, int posX, int posY) {
        texture = tex;     // Safe sharing
        x = posX;
        y = posY;
    }
};

int main() {
    auto globalTexture = std::make_shared<Texture>("player.png");
    
    auto player1 = std::make_unique<Sprite>(globalTexture, 100, 200);  // Shares texture
    auto player2 = std::make_unique<Sprite>(globalTexture, 300, 400);  // Shares texture
    
    return 0;  // Texture deleted when last reference goes out of scope
}
```

**Analysis Output:**
```
SHARED OWNERSHIP: Texture.texture -> shared_ptr (Rule B)
```

**Explanation:**
- Multiple objects reference the same texture
- Reference counting tracks ownership
- Object deleted when last owner releases it
- Safe sharing between multiple owners

---

### **Rule C: Avoiding Cycles (Graphs or Doubly Linked Lists)**

**Condition:** There is a potential for **cyclic references**.

**Pointer:** Use `shared_ptr` for the "owning" side, and `weak_ptr` for the "back" side.

**CLG Input:**
```cpp
class Node {
public:
    int data;
    Node next;    // Forward reference
    Node prev;    // Back reference - potential cycle!
    
    Node(int value) {
        data = value;
        next = nullptr;
        prev = nullptr;
    }
};

class DoublyLinkedList {
private:
    Node head;
    Node tail;
    
public:
    void addNode(int value) {
        Node newNode = new Node(value);
        
        if (!head) {
            head = newNode;
            tail = newNode;
        } else {
            tail.next = newNode;    // Forward link
            newNode.prev = tail;    // Back link - creates potential cycle!
            tail = newNode;
        }
    }
};
```

**Generated C++:**
```cpp
class Node {
public:
    int data;
    std::shared_ptr<Node> next;    // ✅ Rule C: Forward ownership
    std::weak_ptr<Node> prev;      // ✅ Rule C: Back reference (breaks cycle)
    
    Node(int value) {
        data = value;
        next = nullptr;
        // prev is automatically initialized as empty weak_ptr
    }
};

class DoublyLinkedList {
private:
    std::shared_ptr<Node> head;    // ✅ Rule A: Exclusive ownership of head
    std::weak_ptr<Node> tail;      // ✅ Rule C: Non-owning reference to tail
    
public:
    void addNode(int value) {
        auto newNode = std::make_shared<Node>(value);
        
        if (!head) {
            head = newNode;
            tail = newNode;  // weak_ptr assignment
        } else {
            if (auto tailPtr = tail.lock()) {  // Safe access to tail
                tailPtr->next = newNode;        // Forward link (shared_ptr)
                newNode->prev = tailPtr;        // Back link (weak_ptr) - no cycle!
                tail = newNode;
            }
        }
    }
};
```

**Analysis Output:**
```
CYCLE DETECTED: Node.next -> shared_ptr (Rule C forward)
CYCLE DETECTED: Node.prev -> weak_ptr (Rule C back)
```

**Explanation:**
- `shared_ptr` for forward/owning direction
- `weak_ptr` for backward/non-owning direction
- Prevents reference cycles and memory leaks
- `weak_ptr` must be checked with `.lock()` before use

---

### **Rule D: Temporary or Local Ownership**

**Condition:** Object is **temporary** or only exists within a local scope.

**Pointer:** `unique_ptr` or even a **stack object** (no smart pointer needed).

**CLG Input:**
```cpp
void processData() {
    Node tempNode = new Node(42);    // Temporary, local scope only
    tempNode.process();
    
    // tempNode should be automatically cleaned up
}

class DataProcessor {
public:
    void process() {
        Node workingNode = new Node(100);  // Local working object
        workingNode.transform();
        workingNode.validate();
        // workingNode only used in this method
    }
};
```

**Generated C++:**
```cpp
void processData() {
    auto tempNode = std::make_unique<Node>(42);    // ✅ Rule D: Local ownership
    tempNode->process();
    
    // tempNode automatically cleaned up when function exits
}

class DataProcessor {
public:
    void process() {
        auto workingNode = std::make_unique<Node>(100);  // ✅ Rule D: Local ownership
        workingNode->transform();
        workingNode->validate();
        // workingNode automatically cleaned up when method exits
    }
};
```

**Analysis Output:**
```
DEFAULT/LOCAL: Node.tempNode -> unique_ptr (Rule D)
DEFAULT/LOCAL: Node.workingNode -> unique_ptr (Rule D)
```

**Explanation:**
- Local scope = exclusive ownership
- `unique_ptr` provides RAII with minimal overhead
- Automatic cleanup when scope ends
- No sharing needed = prefer `unique_ptr` over `shared_ptr`

---

## 🔄 Rule Priority and Decision Tree

The analyzer applies rules in this priority order:

```
1. Rule C (Cycle Prevention) - HIGHEST PRIORITY
   ↓ If no cycles detected...
   
2. Rule B (Shared Ownership)
   ↓ If not shared...
   
3. Rule A (Parent-Child)
   ↓ If not clear parent-child...
   
4. Rule D (Default/Local) - LOWEST PRIORITY
```

### Decision Algorithm:
```cpp
PointerType determinePointerType(const Variable& var) {
    // Rule C: Check for cycles first (highest priority)
    if (isPartOfCycle(var)) {
        return isBackReference(var) ? WEAK : SHARED;
    }
    
    // Rule B: Check for shared ownership
    if (isSharedOwnership(var)) {
        return SHARED;
    }
    
    // Rule A: Check for parent-child relationship  
    if (isParentChildRelationship(var)) {
        return UNIQUE;
    }
    
    // Rule D: Default to local ownership
    return UNIQUE;
}
```

---

## 📊 Complete Example: Complex Data Structure

**CLG Input:**
```cpp
class TreeNode {
public:
    int value;
    TreeNode left;       // Child - Rule A
    TreeNode right;      // Child - Rule A  
    TreeNode parent;     // Back reference - Rule C
};

class Graph {
public:
    TreeNode nodeA;      // Shared node - Rule B
    TreeNode nodeB;      // Shared node - Rule B
    
    Graph() {
        nodeA = new TreeNode(1);
        nodeB = new TreeNode(2);
        
        // Create cycle: A points to B, B points to A
        nodeA.right = nodeB;    // Forward reference
        nodeB.parent = nodeA;   // Back reference - cycle!
    }
};

int main() {
    Graph graph = new Graph();     // Local object - Rule D
    TreeNode temp = graph.nodeA;   // Temporary reference - Rule B
    return 0;
}
```

**Generated C++:**
```cpp
class TreeNode {
public:
    int value;
    std::unique_ptr<TreeNode> left;       // ✅ Rule A: Parent-child
    std::unique_ptr<TreeNode> right;      // ✅ Rule A: Parent-child
    std::weak_ptr<TreeNode> parent;       // ✅ Rule C: Back reference
};

class Graph {
public:
    std::shared_ptr<TreeNode> nodeA;      // ✅ Rule B: Shared ownership
    std::shared_ptr<TreeNode> nodeB;      // ✅ Rule B: Shared ownership
    
    Graph() {
        nodeA = std::make_shared<TreeNode>(1);
        nodeB = std::make_shared<TreeNode>(2);
        
        // Create cycle-safe references
        nodeA->right = std::unique_ptr<TreeNode>(nodeB.get());  // Forward
        nodeB->parent = nodeA;                                   // Back (weak)
    }
};

int main() {
    auto graph = std::make_unique<Graph>();         // ✅ Rule D: Local ownership
    std::shared_ptr<TreeNode> temp = graph->nodeA; // ✅ Rule B: Shared reference
    return 0;
}
```

**Analysis Output:**
```
=== FIRST PASS: Collecting Reference Relationships ===
Found 3 classes: TreeNode, Graph, main
Found 6 class variable references

=== SECOND PASS: Determining Pointer Types ===
PARENT-CHILD: TreeNode.left -> unique_ptr (Rule A)
PARENT-CHILD: TreeNode.right -> unique_ptr (Rule A)  
CYCLE DETECTED: TreeNode.parent -> weak_ptr (Rule C)
SHARED OWNERSHIP: Graph.nodeA -> shared_ptr (Rule B)
SHARED OWNERSHIP: Graph.nodeB -> shared_ptr (Rule B)
DEFAULT/LOCAL: Graph.graph -> unique_ptr (Rule D)
```

---

## 🚀 Benefits of This Approach

### **Memory Safety**
- ✅ Automatic cleanup with RAII
- ✅ No memory leaks from cycles
- ✅ No dangling pointers
- ✅ Exception safety

### **Performance Optimization**
- ✅ `unique_ptr` when possible (zero overhead)
- ✅ `shared_ptr` only when needed (minimal overhead)
- ✅ `weak_ptr` for cycle breaking (prevents leaks)

### **Code Clarity**
- ✅ Ownership semantics are explicit
- ✅ Relationships are clear from types
- ✅ Intent is preserved from CLG to C++

### **Compatibility**
- ✅ Works with existing CLG syntax
- ✅ Gradual migration possible
- ✅ Standard C++ smart pointers

---

## 🔧 Usage

To use the smart pointer analyzer:

```bash
# Compile CLG file with smart pointer analysis
cling --cpp output.cpp input.clg

# The analyzer will automatically:
# 1. Perform two-pass analysis
# 2. Apply appropriate rules
# 3. Generate optimized C++ code
# 4. Show analysis decisions in output
```

The resulting C++ code will have optimal smart pointer usage for memory safety and performance! 🎉
