# Smart Pointer Rules Examples

This directory contains examples demonstrating how CeeLing's two-pass smart pointer analyzer converts CLG code to optimized C++ with appropriate smart pointers.

## 📁 Example Files

### Individual Rule Examples

1. **`rule_a_binary_tree.clg`** - Demonstrates **Rule A: Parent-Child Relationships**
   - Binary tree with exclusive parent-child ownership
   - Results in `unique_ptr` for `left` and `right` children

2. **`rule_b_shared_resources.clg`** - Demonstrates **Rule B: Shared Objects**
   - Textures and materials shared between multiple objects
   - Results in `shared_ptr` for shared resources

3. **`rule_c_doubly_linked_list.clg`** - Demonstrates **Rule C: Cycle Prevention**
   - Doubly linked list with forward and backward references
   - Results in `shared_ptr` for forward links, `weak_ptr` for backward links

4. **`rule_d_local_temporary.clg`** - Demonstrates **Rule D: Local/Temporary Ownership**
   - Objects that exist only within local scope
   - Results in `unique_ptr` for local objects

### Comprehensive Example

5. **`comprehensive_smart_pointer_demo.clg`** - Demonstrates **All Rules Together**
   - Complex example showing all four rules in a single file
   - Shows how the analyzer handles mixed ownership patterns

## 🚀 Running the Examples

To test any example with the smart pointer analyzer:

```bash
# Build the CeeLing compiler (if not already built)
cd build
ninja
cd ..

# Run individual examples
.\build\cling.exe --cpp output.cpp examples/rule_a_binary_tree.clg
.\build\cling.exe --cpp output.cpp examples/rule_b_shared_resources.clg
.\build\cling.exe --cpp output.cpp examples/rule_c_doubly_linked_list.clg
.\build\cling.exe --cpp output.cpp examples/rule_d_local_temporary.clg

# Run comprehensive example
.\build\cling.exe --cpp output.cpp examples/comprehensive_smart_pointer_demo.clg
```

## 📊 Expected Analysis Output

When you run the comprehensive example, you should see output like:

```
=== PERFORMING TWO-PASS SMART POINTER ANALYSIS ===
=== FIRST PASS: Collecting Reference Relationships ===
Found 5 classes: ComplexGraph ListNode Material Texture TreeNode
Found 13 class variable references

=== SECOND PASS: Determining Pointer Types ===
PARENT-CHILD: TreeNode.left -> unique_ptr (Rule A)
PARENT-CHILD: TreeNode.right -> unique_ptr (Rule A)
CYCLE DETECTED: ListNode.next -> shared_ptr (Rule C forward)
CYCLE DETECTED: ListNode.prev -> weak_ptr (Rule C back)
SHARED OWNERSHIP: Material.texture -> shared_ptr (Rule B)
DEFAULT/LOCAL: ComplexGraph.graph -> unique_ptr (Rule D)
```

## 🔍 What to Look For

### Rule A Examples (Parent-Child)
- **Input**: `TreeNode left;`
- **Output**: `std::unique_ptr<TreeNode> left;`
- **Reason**: Exclusive parent-child ownership

### Rule B Examples (Shared Objects)
- **Input**: `Texture texture;` (used by multiple materials)
- **Output**: `std::shared_ptr<Texture> texture;`
- **Reason**: Multiple objects share the same resource

### Rule C Examples (Cycle Prevention)
- **Input**: `ListNode next;` and `ListNode prev;`
- **Output**: `std::shared_ptr<ListNode> next;` and `std::weak_ptr<ListNode> prev;`
- **Reason**: Prevents reference cycles in doubly-linked structures

### Rule D Examples (Local/Temporary)
- **Input**: `ComplexGraph graph = new ComplexGraph();` (in main function)
- **Output**: `std::unique_ptr<ComplexGraph> graph = std::make_unique<ComplexGraph>();`
- **Reason**: Local scope ownership

## 🎯 Key Benefits Demonstrated

1. **Memory Safety**: No memory leaks, even with cycles
2. **Performance**: `unique_ptr` used when possible (zero overhead)
3. **Clarity**: Ownership semantics are explicit in the generated code
4. **Automation**: Intelligent decisions made automatically by the analyzer

## 🔧 Customizing the Examples

Feel free to modify these examples to test different scenarios:

- Add more complex inheritance hierarchies
- Create more intricate sharing patterns
- Test deeply nested structures
- Experiment with different cycle patterns

The analyzer will adapt and make appropriate smart pointer decisions for your specific use cases!

## 📚 Further Reading

See `../docs/smart_pointer_rules.md` for complete documentation of all rules and decision algorithms.
