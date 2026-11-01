# Before & After: Smart Pointer Transformations

> **Merged into the Language Guide.** These examples now appear in
> [Language Guide Ch. 4 — Ownership](language-guide/04-ownership.md).
> This file is retained as an extended example gallery.

This document shows concrete examples of how CeeLing's two-pass smart pointer analyzer transforms CLG code into optimized C++.

## 🔄 Rule A: Parent-Child Relationships → `unique_ptr`

### Before (CLG Input)
```cpp
class TreeNode {
public:
    int data;
    TreeNode left;     // Raw class type
    TreeNode right;    // Raw class type
    
    TreeNode(int value) {
        data = value;
        left = nullptr;
        right = nullptr;
    }
    
    void insert(int value) {
        if (value < data) {
            if (!left) {
                left = new TreeNode(value);  // Raw new
            }
        } else {
            if (!right) {
                right = new TreeNode(value); // Raw new
            }
        }
    }
};
```

### After (Generated C++)
```cpp
class TreeNode {
public:
    int data;
    std::unique_ptr<TreeNode> left;     // ✅ Rule A: Parent-child
    std::unique_ptr<TreeNode> right;    // ✅ Rule A: Parent-child
    
    TreeNode(int value) {
        data = value;
        left = nullptr;
        right = nullptr;
    }
    
    void insert(int value) {
        if (value < data) {
            if (!left) {
                left = std::make_unique<TreeNode>(value);  // ✅ make_unique
            }
        } else {
            if (!right) {
                right = std::make_unique<TreeNode>(value); // ✅ make_unique
            }
        }
    }
};
```

### Analysis Output
```
PARENT-CHILD: TreeNode.left -> unique_ptr (Rule A)
PARENT-CHILD: TreeNode.right -> unique_ptr (Rule A)
```

**Benefits**: Exclusive ownership, automatic cleanup, zero overhead

---

## 🔄 Rule B: Shared Objects → `shared_ptr`

### Before (CLG Input)
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

class Material {
public:
    Texture diffuseTexture;    // Multiple materials share textures
    float shininess;
    
    Material(Texture tex, float shine) {
        diffuseTexture = tex;     // Raw assignment
        shininess = shine;
    }
};

int main() {
    Texture rockTexture = new Texture("rock.png");   // Raw new
    
    Material mat1 = new Material(rockTexture, 0.2);  // Sharing texture
    Material mat2 = new Material(rockTexture, 0.3);  // Sharing texture
    
    return 0;
}
```

### After (Generated C++)
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

class Material {
public:
    std::shared_ptr<Texture> diffuseTexture;    // ✅ Rule B: Shared ownership
    float shininess;
    
    Material(std::shared_ptr<Texture> tex, float shine) {
        diffuseTexture = tex;     // ✅ Safe sharing
        shininess = shine;
    }
};

int main() {
    auto rockTexture = std::make_shared<Texture>("rock.png");   // ✅ make_shared
    
    auto mat1 = std::make_unique<Material>(rockTexture, 0.2);   // ✅ Shares texture
    auto mat2 = std::make_unique<Material>(rockTexture, 0.3);   // ✅ Shares texture
    
    return 0;  // ✅ Texture deleted when last reference goes away
}
```

### Analysis Output
```
SHARED OWNERSHIP: Texture.diffuseTexture -> shared_ptr (Rule B)
```

**Benefits**: Safe sharing, reference counting, automatic cleanup when last owner releases

---

## 🔄 Rule C: Cycle Prevention → `shared_ptr` + `weak_ptr`

### Before (CLG Input)
```cpp
class Node {
public:
    int data;
    Node next;    // Forward reference
    Node prev;    // Back reference - creates cycle!
    
    Node(int value) {
        data = value;
        next = nullptr;
        prev = nullptr;
    }
};

class DoublyLinkedList {
public:
    Node head;
    
    void append(int value) {
        Node newNode = new Node(value);
        
        if (head) {
            Node tail = findTail();
            tail.next = newNode;      // Forward link
            newNode.prev = tail;      // Back link - CYCLE!
        } else {
            head = newNode;
        }
    }
};
```

### After (Generated C++)
```cpp
class Node {
public:
    int data;
    std::shared_ptr<Node> next;    // ✅ Rule C: Forward ownership
    std::weak_ptr<Node> prev;      // ✅ Rule C: Back reference (breaks cycle)
    
    Node(int value) {
        data = value;
        next = nullptr;
        // prev automatically initialized as empty weak_ptr
    }
};

class DoublyLinkedList {
public:
    std::shared_ptr<Node> head;    // ✅ Rule A: Exclusive ownership
    
    void append(int value) {
        auto newNode = std::make_shared<Node>(value);
        
        if (head) {
            auto tail = findTail();
            tail->next = newNode;        // ✅ Forward link (shared_ptr)
            newNode->prev = tail;        // ✅ Back link (weak_ptr) - no cycle!
        } else {
            head = newNode;
        }
    }
};
```

### Analysis Output
```
CYCLE DETECTED: Node.next -> shared_ptr (Rule C forward)
CYCLE DETECTED: Node.prev -> weak_ptr (Rule C back)
```

**Benefits**: Prevents memory leaks from cycles, safe backward navigation with `.lock()`

---

## 🔄 Rule D: Local/Temporary → `unique_ptr`

### Before (CLG Input)
```cpp
void processData(int value) {
    DataProcessor processor = new DataProcessor(1, "LocalProcessor");  // Raw new
    
    processor.process(value);
    int result = processor.calculate(value);
    
    // Manual cleanup required
}

int main() {
    ComplexGraph graph = new ComplexGraph();     // Raw new
    TreeNode tempNode = new TreeNode(42);        // Raw new
    
    // Use objects locally
    tempNode.process();
    
    // Manual cleanup required
    return 0;
}
```

### After (Generated C++)
```cpp
void processData(int value) {
    auto processor = std::make_unique<DataProcessor>(1, "LocalProcessor");  // ✅ make_unique
    
    processor->process(value);
    int result = processor->calculate(value);
    
    // ✅ Automatic cleanup when function exits
}

int main() {
    auto graph = std::make_unique<ComplexGraph>();     // ✅ Rule D: Local ownership
    auto tempNode = std::make_unique<TreeNode>(42);    // ✅ Rule D: Local ownership
    
    // Use objects locally
    tempNode->process();
    
    return 0;  // ✅ Automatic cleanup when scope ends
}
```

### Analysis Output
```
DEFAULT/LOCAL: DataProcessor.processor -> unique_ptr (Rule D)
DEFAULT/LOCAL: ComplexGraph.graph -> unique_ptr (Rule D)
DEFAULT/LOCAL: TreeNode.tempNode -> unique_ptr (Rule D)
```

**Benefits**: RAII, automatic cleanup, minimal overhead for local objects

---

## 🔀 Complex Mixed Example

### Before (CLG Input)
```cpp
class ComplexSystem {
public:
    TreeNode rootNode;      // Parent-child
    Node listHead;          // Part of cycle
    Texture sharedTexture;  // Shared resource
    
    ComplexSystem() {
        rootNode = new TreeNode(1);
        listHead = new Node(100);
        sharedTexture = new Texture("common.png");
        
        // Create relationships
        rootNode.left = new TreeNode(2);    // Parent-child
        listHead.next = new Node(200);      // Forward in cycle
        listHead.next.prev = listHead;      // Back in cycle - MEMORY LEAK!
    }
};
```

### After (Generated C++)
```cpp
class ComplexSystem {
public:
    std::unique_ptr<TreeNode> rootNode;      // ✅ Rule A: Parent-child
    std::shared_ptr<Node> listHead;          // ✅ Rule C: Part of cycle
    std::shared_ptr<Texture> sharedTexture;  // ✅ Rule B: Shared resource
    
    ComplexSystem() {
        rootNode = std::make_unique<TreeNode>(1);
        listHead = std::make_shared<Node>(100);
        sharedTexture = std::make_shared<Texture>("common.png");
        
        // Create relationships
        rootNode->left = std::make_unique<TreeNode>(2);    // ✅ Parent-child
        listHead->next = std::make_shared<Node>(200);      // ✅ Forward in cycle
        listHead->next->prev = listHead;                   // ✅ Back (weak_ptr) - no leak!
    }
};
```

### Analysis Output
```
=== FIRST PASS: Collecting Reference Relationships ===
Found 3 classes: TreeNode Node Texture
Found 6 class variable references

=== SECOND PASS: Determining Pointer Types ===
PARENT-CHILD: TreeNode.rootNode -> unique_ptr (Rule A)
PARENT-CHILD: TreeNode.left -> unique_ptr (Rule A)
CYCLE DETECTED: Node.listHead -> shared_ptr (Rule C forward)
CYCLE DETECTED: Node.next -> shared_ptr (Rule C forward)
CYCLE DETECTED: Node.prev -> weak_ptr (Rule C back)
SHARED OWNERSHIP: Texture.sharedTexture -> shared_ptr (Rule B)
```

---

## 📊 Summary of Transformations

| CLG Pattern | Generated C++ | Rule Applied | Benefit |
|-------------|---------------|--------------|---------|
| `TreeNode child;` | `std::unique_ptr<TreeNode> child;` | Rule A | Exclusive ownership, zero overhead |
| `Texture shared;` | `std::shared_ptr<Texture> shared;` | Rule B | Safe sharing, reference counting |
| `Node next;` (in cycle) | `std::shared_ptr<Node> next;` | Rule C | Forward ownership in cycle |
| `Node prev;` (in cycle) | `std::weak_ptr<Node> prev;` | Rule C | Breaks cycle, prevents leaks |
| `Object temp;` (local) | `std::unique_ptr<Object> temp;` | Rule D | RAII, automatic cleanup |
| `new ClassName()` | `std::make_unique<ClassName>()` or `std::make_shared<ClassName>()` | Context-dependent | Exception safety, no raw new |
| `obj.member` | `obj->member` | All rules | Consistent pointer access |

## 🎯 Key Advantages of This Approach

1. **🛡️ Memory Safety**: No leaks, even with complex cycles
2. **⚡ Performance**: Optimal pointer choice for each use case
3. **🤖 Automation**: Intelligent decisions without manual annotation
4. **📖 Clarity**: Ownership semantics explicit in generated code
5. **🔄 Compatibility**: Works with existing CLG syntax
6. **🧪 Testable**: Clear analysis output for verification

The two-pass analysis transforms simple CLG syntax into sophisticated, memory-safe C++ automatically! 🚀
