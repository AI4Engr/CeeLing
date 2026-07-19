# 11. Dangling References

This is the **core guarantee** — the reason CeeLing exists.

## The problem in raw C++

Smart pointers decide *who deletes*, but they do **not** stop a non-owning reference
from outliving the object it points at. All four of these compile cleanly in C++ and
are undefined behavior at runtime:

```cpp
// (A) owner reassigned — old object freed, reference dangles
auto a = std::make_shared<Node>(1);
Node& b = *a;
a = std::make_shared<Node>(2);   // old Node(1) freed
use(b);                          // ← dangles

// (B) owner moved into a sink, then reference used
auto a = std::make_shared<Node>(1);
Node& b = *a;
sink(std::move(a));              // a's object consumed
use(b);                          // ← dangles

// (C) owner from an inner scope, used after the scope ends
Node* b;
{ auto inner = std::make_shared<Node>(1); b = inner.get(); }
use(*b);                         // ← inner destroyed, dangles

// (D) reference stored in a longer-lived field (escape)
struct Holder { Node* ref; };
Holder h; h.ref = a.get();       // borrow escapes into h
eat(std::move(a));               // a consumed
use(h.ref);                      // ← dangles
```

These map exactly to `tests/borrow_owner_reassign_use`, `_moved_use`,
`_scope_end_use`, and `borrow_stored_in_field`.

## How CeeLing solves it

Three components (full detail in [Chapter 5](05-how-analysis-works.md)):

- **Provenance table** — on assignment, record `b <= a` ("b references a").
- **Death events** — an owner dies on: move-into-sink, reassignment, scope exit,
  destructive call.
- **Use-site check** — at every use of a reference, if its owner is dead → **reject**
  at compile time. Conservative: at a branch merge, dead-on-any-path ⇒ reject.

## Proof by example

### Rejected — the canonical case (equivalent to what Rust rejects)

```
owned<Node> root = new Node(1);   // root: owner, live
borrowed<Node> n = borrow(root);  // provenance: n <= root
drop(root);                       // death: root = DEAD
print(n.key);                     // use n → root DEAD → ❌ REJECT
```

Error (target form):

```
error[semantic/dangling]: `n` is used after its owner `root` was moved away
  --> prog.clg:30:11
   |
29 |     drop(root);      // root moved here
30 |     print(n.key);    // n still references root
   |           ^ `n` references `root`, which no longer has a live owner
help: use `n` before `root` is moved, or copy the value out first
```

### Accepted — owner still alive (control, must NOT be rejected)

```
owned<Node> a = new Node(1);
borrowed<Node> b = a;
std::cout << b.v;                 // a still live → ✅ ACCEPT
return 0;
```

This positive control (`borrow_owner_alive_ok`) guards against a checker that is *too*
aggressive — a safety analysis that rejects valid code is a bug too.

## Why this needs analysis, not just "use smart pointers"

Case (B)/(D) are especially instructive: if CeeLing naively made `b` a `shared_ptr`
copy of `a`, the object would *not* be freed and the program would "work" — but only
by **leaking to mask the dangle**, offering no real safety over plain C++. CeeLing
must *detect* the escape, not paper over it. See [Chapter 7](07-vs-rust.md) for why
"it didn't crash" is not the same as "it is safe."
