# 12. Use-After-Move

**Coupled to auto-`std::move`:** because CeeLing inserts `std::move` for you, it is
*obligated* to catch the moved-from bug that move creates. This is a prerequisite of
the auto-move feature, not an optional extra.

## The problem in raw C++

`std::move` does not destroy the source — it leaves a **valid-but-unspecified empty
shell** that is still accessible. Touching it after the move is a latent bug:

```cpp
auto q = std::make_unique<Node>(1);
auto p = std::move(q);     // q is now empty (nullptr for unique_ptr)
q->foo();                  // ← null dereference — compiles fine, crashes at runtime

std::string s = "hello";
std::string t = std::move(s);
std::cout << s.size();     // ← s is unspecified; "works" today, UB-adjacent, brittle
```

Contrast Rust, where the source becomes **unusable at compile time**. C++ gives no
such protection — and auto-inserted moves would *multiply* these traps invisibly.

## How CeeLing solves it

The shared lifetime table (see [Chapter 5](05-how-analysis-works.md)) tracks a
**moved-from state** per variable:

- When a value is moved (whether the user wrote it or the compiler auto-inserted it),
  the source is marked **moved-from**.
- Any subsequent **use** of a moved-from variable is **rejected** at compile time.
- Re-assigning the variable clears the moved-from state (it is live again).

The move optimizer and the use-after-move checker read the *same* table, so the
compiler will never auto-move a value it will later use.

## Proof by example

### Rejected

```
Node q = new Node(1);
Node p = q;          // last use of q as a source → compiler emits std::move(q)
q.foo();             // ❌ REJECT: use of moved-from `q`
```
```
error[semantic/use-after-move]: `q` is used after it was moved into `p`
  --> prog.clg:3:1
help: `q` is empty after the move; assign a new value before using it
```

### Accepted — reassigned before use

```
Node q = new Node(1);
Node p = q;          // q moved-from here
q = new Node(2);     // q live again
q.foo();             // ✅ ACCEPT
```

### Accepted — auto-move at true last use

```
Node q = new Node(1);
consume(q);          // q's last use → auto std::move(q), never touched again → ✅
```

See fixtures `tests/move_local_ok`, `move_field_ok`, `move_shared_owner_ok` for the
accept cases the analyzer must keep green.
