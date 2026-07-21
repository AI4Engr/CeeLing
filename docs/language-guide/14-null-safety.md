# 14. Null Dereference

"All smart pointers" does **not** mean "never null." A `shared_ptr` can be empty, and
`weak_ptr::lock()` can fail. This is as high-frequency as bounds checking and cannot
be skipped.

## The problem in raw C++

```cpp
std::shared_ptr<Node> p;         // default-constructed → empty
p->foo();                        // ← null dereference, UB

std::weak_ptr<Node> w = prev;
w.lock()->foo();                 // ← if the object is gone, lock() is empty → UB
```

The type `shared_ptr<Node>` says nothing about whether it currently holds an object,
so the deref is unguarded.

## How CeeLing solves it

**Non-null typing where provable, inserted runtime check otherwise.**

- Where the analysis can prove a pointer is non-empty at the use site (e.g. freshly
  constructed and not moved-from), the deref is direct — no check.
- Where it cannot, the generator inserts a **null check** that panics/throws on a null
  deref, converting UB into a defined failure.
- `weak_ptr` back-references (Rule C) are **required** to go through a checked
  `.lock()`: a failed lock is handled, never blindly dereferenced.

## Proof by example

### No check needed — provably non-null

```
Node n = new Node(1);    // freshly constructed, non-null
std::cout << n.key << std::endl;            // ✅ direct access, no runtime check
```

### Runtime-guarded — possibly empty

```
Node n = maybeGet();     // may return empty
std::cout << n.key << std::endl;            // ✅ compiles; null at runtime → panic, not UB
```

### Back-reference must be checked

```
// prev is a weak_ptr (Rule C). Access forces a checked lock:
Node p = node.prev;      // lowers to: auto p = node->prev.lock();
std::cout << p.key << std::endl;            // guarded — if the parent is gone, panic instead of UB
```

The guarantee: a null dereference is caught statically or turned into a clean panic —
never silent memory corruption.
