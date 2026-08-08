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

These map exactly to `tests/safety/observer_dangles_after_reassign.clg`,
`observer_dangles_after_move.clg`, `observer_dangles_after_scope.clg`, and
`ref_field_rejected.clg` (case (D) is sealed by construction in CeeLing — see
Chapter 17 — rather than caught by the same provenance analysis as the other three,
since the grammar has no `ref`-qualified field production at all).

## How CeeLing solves it

Three components (full detail in [Chapter 5](05-how-analysis-works.md)):

- **Provenance table** — on assignment, record `b <= a` ("b references a").
- **Death events** — an owner dies on: move-into-sink, reassignment, scope exit,
  destructive call.
- **Use-site check** — at every use of a reference, if its owner is dead → **reject**
  at compile time. Conservative: at a branch merge, dead-on-any-path ⇒ reject.

## Proof by example

### Rejected — the canonical case (equivalent to what Rust rejects)

In CeeLing's actual surface syntax (see `tests/safety/observer_dangles_after_move.clg`):

```
Node root = new Node(1);   // root: owner, live
ref Node n = root;         // provenance: n observes root
sink(root);                // root moved into sink → death: root = DEAD
std::cout << n.key << std::endl;   // use n → root DEAD → ❌ REJECT
```

Actual diagnostic:

```
error[semantic/use-after-move]: use of dangling observer `n` (its owner `root` is gone)
  --> prog.clg:4:18
  help: `n` observes `root`, which no longer has a live owner
```

### Accepted — owner still alive (control, must NOT be rejected)

```
Node a = new Node(1);
ref Node b = a;
std::cout << b.v << std::endl;    // a still live → ✅ ACCEPT
return 0;
```

This positive control (`tests/safety/observer_alive_ok.clg`) guards against a checker
that is *too* aggressive — a safety analysis that rejects valid code is a bug too.

## Why this needs analysis, not just "use smart pointers"

Case (B)/(D) are especially instructive: if CeeLing naively made `b` a `shared_ptr`
copy of `a`, the object would *not* be freed and the program would "work" — but only
by **leaking to mask the dangle**, offering no real safety over plain C++. CeeLing
must *detect* the escape, not paper over it. See [Chapter 7](07-vs-rust.md) for why
"it didn't crash" is not the same as "it is safe."
