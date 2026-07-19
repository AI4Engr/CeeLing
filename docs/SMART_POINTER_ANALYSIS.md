# Smart Pointer Analysis - Two-Pass Algorithm

> **Merged into the Language Guide.** This content now lives, in the
> "problem → solution → proof" format, in
> [Language Guide Ch. 10 — Smart-Pointer Inference](language-guide/10-smart-pointer-inference.md).
> This file is retained as the detailed algorithm reference.

## Overview

CeeLing uses a sophisticated two-pass analysis algorithm to automatically determine the optimal smart pointer type (`unique_ptr`, `shared_ptr`, or `weak_ptr`) for each class variable. This ensures memory safety while maximizing performance.

## Algorithm Architecture

```
Source Code → Pass 1: Collect References → Pass 2: Determine Types → C++ Generation
```

## Pass 1: Reference Collection

**Purpose:** Build a complete graph of variable relationships and ownership patterns.

**Steps:**
1. Collect all class names from declarations
2. Identify all class-type variables (member variables and local variables)
3. Track assignment relationships:
   - `Node temp = head;` → records that `temp` is assigned from `head`
   - `left = new Node(5);` → records new object creation
4. Build class relationship graph for cycle detection

**Data Structures:**
```cpp
struct VariableReference {
    string className;              // Type (e.g., "Node")
    string variableName;           // Name (e.g., "left")
    string containingClass;        // Where declared (e.g., "Node" or "main")
    bool isAssignedFrom;           // Assigned from another variable?
    bool isAssignedTo;             // Assigned to another variable?
    bool isBackReference;          // Backward reference (prev, parent)?
    set<string> assignedFromVars;  // Source variables
    set<string> assignedToVars;    // Target variables
};
```

## Pass 2: Pointer Type Determination

**Purpose:** Apply ownership rules to select optimal pointer type.

**Decision Tree:**
```
For each variable:
  ├─ Is it a back-reference? (prev, parent, back, owner)
  │  └─ YES → weak_ptr (Rule C)
  │
  ├─ Is it shared? (multiple assignments from/to)
  │  └─ YES → shared_ptr (Rule B)
  │
  └─ Default → unique_ptr (Rule A)
```

## Smart Pointer Selection Rules

### Rule A: Single Ownership (unique_ptr)

**Condition:** Variable has exclusive ownership with no sharing.

**Examples:**
- Binary tree children: `left`, `right`
- Singly linked list: `next`
- Parent-child relationships

**Benefits:**
- Zero runtime overhead
- Move semantics
- Clear ownership

```cpp
// CeeLing
class Node {
    Node left;
    Node right;
};

// Generated C++
class Node {
    std::unique_ptr<Node> left;
    std::unique_ptr<Node> right;
};
```

### Rule B: Shared Ownership (shared_ptr)

**Condition:** Multiple variables reference the same object.

**Detection:**
- Variable assigned to multiple targets: `assignedToVars.size() > 1`
- Variable assigned from multiple sources: `assignedFromVars.size() > 1`
- Both assigned from and to: `isAssignedFrom && isAssignedTo`

**Examples:**
- Multiple references to same node
- Shared resources
- Cache entries

```cpp
// CeeLing
Node temp = head;      // temp shares ownership with head
Node current = temp;   // current shares with temp

// Generated C++
std::shared_ptr<Node> temp = head;
std::shared_ptr<Node> current = temp;
```

### Rule C: Back-Reference (weak_ptr)

**Condition:** Backward pointer in bidirectional structure.

**Detection:**
1. Class has multiple members of same type (e.g., `next` and `prev`)
2. Variable name matches pattern: `prev`, `previous`, `parent`, `back`, `owner`

**Purpose:** Break reference cycles to prevent memory leaks.

**Examples:**
- Doubly linked list: `prev`
- Tree with parent pointers: `parent`
- Graph back-edges: `back`

```cpp
// CeeLing
class Node {
    Node next;    // Forward reference
    Node prev;    // Backward reference
};

// Generated C++
class Node {
    std::unique_ptr<Node> next;  // Owns next node
    std::weak_ptr<Node> prev;    // Non-owning back-reference
};
```

## Implementation Details

### New-Expression Conversion Coverage

Code generation now converts raw `new` across multiple paths, not only variable declarations:

- `Class x = new Class(...)` -> smart pointer declaration + `make_*`
- `x = new Class(...)` and `obj.member = new Class(...)` -> assignment with `make_*`
- `return new Class(...)` -> `return std::make_shared<Class>(...)`
- Remaining expression contexts (for example function arguments) -> `new Class(...)` to `std::make_shared<Class>(...)`

To keep signatures compatible with pointer-based codegen, class-typed function/method parameters and return types are normalized to `std::shared_ptr<Class>`.

### Back-Reference Detection Algorithm

```cpp
void detectBackReferences() {
    // Group variables by class and type
    map<string, vector<string>> classMembersByType;

    for (auto& [key, ref] : variableReferences) {
        string groupKey = ref.containingClass + ":" + ref.className;
        classMembersByType[groupKey].push_back(ref.variableName);
    }

    // Check for back-reference patterns
    for (auto& [groupKey, members] : classMembersByType) {
        if (members.size() >= 2) {  // Multiple same-type members
            for (auto& memberName : members) {
                if (memberName == "prev" || memberName == "parent" ||
                    memberName == "back" || memberName == "owner") {
                    markAsBackReference(memberName);
                }
            }
        }
    }
}
```

### Shared Ownership Detection

```cpp
bool isSharedOwnership(const string& className, const string& variableName) {
    auto& ref = variableReferences[makeKey(className, variableName)];

    return (ref.assignedToVars.size() > 1) ||      // Assigned to multiple
           (ref.assignedFromVars.size() > 1) ||    // Assigned from multiple
           (ref.isAssignedFrom && ref.isAssignedTo); // Both directions
}
```

## Complete Example: Doubly Linked List

### Input (CeeLing)
```cpp
class Node {
    int data;
    Node next;
    Node prev;
};
```

### Pass 1 Output
```
Found class: Node
Variables:
  - Node.next (containingClass=Node)
  - Node.prev (containingClass=Node)
```

### Pass 2 Output
```
Detected back-reference: Node.prev
Node.next -> unique_ptr (Rule A: single ownership)
Node.prev -> weak_ptr (Rule C: back-reference)
```

### Generated C++
```cpp
class Node {
    int data;
    std::unique_ptr<Node> next;  // Forward ownership
    std::weak_ptr<Node> prev;    // Backward non-owning
};
```

## Performance Characteristics

| Pointer Type | Overhead | Use Case |
|--------------|----------|----------|
| `unique_ptr` | Zero | Single ownership (default) |
| `shared_ptr` | Reference counting | Shared ownership |
| `weak_ptr` | Minimal | Break cycles |

## Limitations and Future Work

**Current Limitations:**
- Back-reference detection relies on naming conventions
- No inter-procedural analysis
- Local variable analysis is scope-limited

**Future Enhancements:**
- Data flow analysis for more accurate sharing detection
- User annotations for explicit control
- Profile-guided optimization hints

## Testing

See example files:
- `tests/binary_tree.clg` - Rule A (unique_ptr)
- `examples/rule_b_shared_resources.clg` - Rule B (shared_ptr)
- `examples/rule_c_doubly_linked_list.clg` - Rule C (weak_ptr)
- `tests/new_expression_paths.clg` - `new` conversion coverage (`member = new`, `return new`, argument `new`)

Run tests:
```bash
ctest --test-dir build --output-on-failure
```

CTest now includes transpile + compile checks for generated C++ on supported fixtures.
