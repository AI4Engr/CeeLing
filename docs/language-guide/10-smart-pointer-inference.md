# 10. Smart-Pointer Inference

*(Merges `docs/SMART_POINTER_ANALYSIS.md` and `docs/before_after_examples.md`.)*

## The problem in raw C++

In C++ you must decide, **by hand**, how every object is owned, and get it right:

```cpp
// Which one is correct here? Get it wrong and you leak, double-free, or dangle.
Node* left;                       // raw — who deletes it? UB waiting to happen
std::unique_ptr<Node> left;       // exclusive owner
std::shared_ptr<Node> left;       // shared owner (overhead if unnecessary)
std::weak_ptr<Node> prev;         // must be weak or the cycle leaks forever
```

Two classic failures:

```cpp
// (a) Leak via reference cycle — both nodes keep each other alive forever.
struct Node { std::shared_ptr<Node> next, prev; };
auto a = std::make_shared<Node>();
auto b = std::make_shared<Node>();
a->next = b; b->prev = a;         // refcount never reaches 0 → LEAK

// (b) Wrong ownership — double free.
Node* shared = new Node();
Owner o1(shared), o2(shared);     // both think they own it → double delete
```

Choosing the pointer type is tedious, error-prone, and clutters the code with
ownership machinery.

## How CeeLing solves it

The user writes **plain object variables**. A **two-pass analysis** infers the
optimal smart-pointer type per variable from the ownership shape:

```
Source → Pass 1: collect references/assignments → Pass 2: determine types → C++
```

| Rule | Condition | Type | Why |
|------|-----------|------|-----|
| **A** | single, exclusive ownership | `unique_ptr` | zero overhead |
| **B** | multiple owners of one object | `shared_ptr` | safe sharing, refcount |
| **C** | back-reference / cycle edge (`prev`,`parent`,`back`,`owner`) | `weak_ptr` | breaks the cycle → no leak |
| **D** | local/temporary | `unique_ptr` | RAII, scoped cleanup |

All raw `new` is converted to `make_unique` / `make_shared`; member access `a.b`
becomes `a->b`. (The goal is to drive these decisions from the same
provenance/lifetime data used for dangling analysis — see
[Chapter 5](05-how-analysis-works.md) — rather than naming heuristics alone.)

## Proof by example

### Rule A — binary tree → `unique_ptr`

```cpp
// CeeLing                          // Generated C++
class TreeNode {                    class TreeNode {
    TreeNode left;                      std::unique_ptr<TreeNode> left;
    TreeNode right;                     std::unique_ptr<TreeNode> right;
};                                  };
// left = new TreeNode(v);          // left = std::make_unique<TreeNode>(v);
```
Analysis: `PARENT-CHILD: TreeNode.left -> unique_ptr (Rule A)`.

### Rule B — shared texture → `shared_ptr`

```cpp
// CeeLing
Texture rock = new Texture("rock.png");
Material mat1 = new Material(rock, 0.2);   // both materials
Material mat2 = new Material(rock, 0.3);   // share the same texture
```
```cpp
// Generated C++
auto rock = std::make_shared<Texture>("rock.png");
auto mat1 = std::make_unique<Material>(rock, 0.2);
auto mat2 = std::make_unique<Material>(rock, 0.3);
// class field: std::shared_ptr<Texture> diffuseTexture;
// → texture deleted only when the last material releases it
```
Analysis: `SHARED OWNERSHIP: Material.diffuseTexture -> shared_ptr (Rule B)`.

### Rule C — doubly linked list → `weak_ptr` back-edge (no leak)

```cpp
// CeeLing                          // Generated C++
class Node {                        class Node {
    Node next;   // forward             std::shared_ptr<Node> next;  // forward, owns
    Node prev;   // backward            std::weak_ptr<Node>   prev;  // back, no cycle
};                                  };
// newNode.prev = tail;             // newNode->prev = tail;   (weak — breaks cycle)
```
Analysis: `CYCLE DETECTED: Node.prev -> weak_ptr (Rule C back)`.
Backward navigation is via `.lock()`; the cycle can no longer leak.

### Mixed system

```cpp
// CeeLing                              // Generated C++
class ComplexSystem {                   class ComplexSystem {
    TreeNode rootNode;   // parent-child     std::unique_ptr<TreeNode> rootNode;
    Node listHead;       // in a cycle       std::shared_ptr<Node>     listHead;
    Texture sharedTex;   // shared           std::shared_ptr<Texture>  sharedTex;
};                                      };
```
```
PARENT-CHILD:   TreeNode.rootNode  -> unique_ptr (Rule A)
CYCLE DETECTED: Node.listHead      -> shared_ptr (Rule C forward)
CYCLE DETECTED: Node.prev          -> weak_ptr   (Rule C back)
SHARED OWNERSHIP: Texture.sharedTex-> shared_ptr (Rule B)
```

## Performance

| Type | Overhead | Use |
|------|----------|-----|
| `unique_ptr` | zero | single ownership (default) |
| `shared_ptr` | atomic refcount | shared ownership |
| `weak_ptr` | minimal | break cycles |

## Current limitations

- Back-reference detection still relies partly on naming conventions
  (`prev`/`parent`/...). The roadmap moves this onto the provenance/lifetime model.
- Inter-procedural sharing is summary-based (see [Chapter 5](05-how-analysis-works.md)).
