# 3. The Java-like Default Experience

The guiding UX principle: **writing CeeLing should feel like Java, not C++.**

The user just declares objects and passes them around by reference. All the C++
memory machinery is inferred and generated automatically:

- **No `new`/`delete`, no `*`/`&`.** Object variables are plain references, as in
  Java. `Node n = new Node(1);` reads like Java; there is no manual pointer
  bookkeeping.
- **No smart-pointer types to choose.** The compiler picks `unique_ptr` /
  `shared_ptr` / `weak_ptr` under the hood from the ownership shape.
- **No manual `std::move`.** The compiler inserts `std::move` automatically at a
  value's last use.
- **No explicit lifetimes.** The safety analysis guarantees no dangling references,
  so the user does not reason about lifetimes in the common case.

## The two core semantic rules

Everything rests on two rules the user rarely has to think about:

1. **Plain assignment / argument passing transfers ownership (default move).**
   ```cpp
   Node b = a;      // ownership moves from a to b; a is now moved-from
   sink(a);         // passing a into a function moves it in
   ```
   This is what lets the compiler prefer `unique_ptr` (single owner) and only fall
   back to `shared_ptr` when it *proves* multiple live owners exist. **"Use
   `unique_ptr` where possible; `shared_ptr` only when necessary."**

2. **A non-owning observer is marked with the `ref` keyword.**
   ```cpp
   ref Node n = obj.getNode();   // n observes; it does NOT take ownership
   ```
   `ref` reads as "reference" — clearer than a bare `&` symbol, and without Rust's
   `borrow`/`&mut` baggage. (`ref` is not a C++ reserved word, so it lives only in
   CeeLing source; it lowers to a native C++ reference — `ref Node n = a;` →
   `Node& n = *a;`.) Observers are exactly what the dangling analysis watches: an
   observer used after its owner is moved / reassigned / goes out of scope is rejected
   (see [Chapter 11](11-dangling-references.md)).

There is **no** `owned<T>` / `borrowed<T>` in the surface language. Ownership is
implicit (move by default); observation is `ref`. That is the whole model.

### Why observation needs its own keyword

`Node b = a;` (move) and `ref Node b = a;` (observe) **must** be different syntax —
otherwise the compiler cannot tell whether you meant to hand ownership over or just
look. That distinction is what lets it prefer `unique_ptr` (single owner) and reach
for `shared_ptr` only when it proves multiple live owners. Java/C# avoid the keyword
only because they gave up ownership entirely and lean on a GC; CeeLing keeps
zero-overhead ownership, so it needs one small signal — `ref` — to mark "observe, do
not move." That is the whole tax, and it is smaller than Rust's `&`/`&mut`/lifetimes.

## Example (intended surface syntax)

```cpp
class Node {
public:
    int key;
    Node next;          // just a reference; compiler infers unique_ptr
    Node(int k) { key = k; }
};

int main() {
    Node root = new Node(1);   // reads like Java
    root.next = new Node(2);   // ownership inferred, std::move inserted as needed
    std::cout << root.next.key << std::endl;
    return 0;
}
```

## Explicit annotations are a *future, optional* feature

Explicit ownership annotations (`borrow<T>`, and similar) are an **advanced escape
hatch for expert control — never a requirement** for the default Java-like
experience.

The early `tests/borrow_*.clg` fixtures use such annotations only as *scaffolding*
while the inference is being built. They are not the intended surface syntax.
