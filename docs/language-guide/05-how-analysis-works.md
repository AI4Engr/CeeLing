# 5. How the Analysis Works

The dangling checker is **a small, C++-specific lifetime analyzer** — a *proper
subset* of a Rust borrow checker (see [Chapter 6](06-why-not-rust.md) for why it is a
subset, not a clone). It keeps the **liveness** half and drops the
**aliasing/mutability** half.

The "conservative reject" choice removes the need for precise control-flow merging
(the hardest part of a real borrow checker): at a branch merge, if an owner is dead
on *any* path, uses are rejected.

## Three components (A / B / C)

### A. Borrow summaries — per function/method

Records which parameters are borrowed, which are moved, and what the return value
borrows from.

- **CeeLing's own functions:** summaries are **auto-derived** by analyzing each body
  once. This is also the **cross-file mechanism** — only the summary crosses file
  boundaries, never the implementation (the same approach Rust uses with function
  signatures).
- **Third-party libraries (`std`/Eigen):** summaries are **hand-written** as an
  allow-list, growing incrementally from a minimal set. Not on the list ⇒ rejected
  (default-reject rule).

Example summaries:

```
std::vector<T>::push_back(self: &mut, x: T move) -> void      // invalidates iterators
std::vector<T>::operator[](self: &, i)            -> & self    // return aliases self
Eigen::MatrixXd::block(self: &, ...)              -> & self    // view aliases self
Eigen::operator+(a: &, b: &)                      -> aliases a, b  // expression template
```

### B. Provenance table

`b <= a` ("b references a"), propagated on assignment.

**Must distinguish the declared type of the LHS.** For expression-template libraries
like Eigen:

```cpp
auto e = A + B;      // e ALIASES A and B (dangling risk) — track provenance
MatrixXd e = A + B;  // e EVALUATES to an owned copy (safe) — break provenance
```

Same RHS, different provenance — the analyzer keys on whether the LHS is
`auto` / an expression type vs. a concrete value type.

### C. Death events + use-site check

An owner "dies" on:

- **move** into an owning sink,
- **reassignment**,
- **scope exit**,
- a **destructive call**.

At each use of a reference, look up its provenance; if the owner is dead, **reject**.

## Worked example

```
owned<Node> root = new Node(1);   // root: owner, live
borrowed<Node> n = borrow(root);  // A: return aliases param → B: n <= root
drop(root);                       // A: param is moved       → C: root = DEAD
std::cout << n.key << std::endl;                     // use n → n <= root, root DEAD → REJECT
```

## Eigen with no knowledge of Eigen internals

Eigen is handled by the *same* mechanism. The analyzer never looks inside Eigen — only
its borrow summaries matter. Any Eigen method without a summary is rejected, so the
safety contract holds even over opaque numeric code. The one Eigen-specific subtlety
is #6 (expression templates), handled by the `auto`-vs-concrete rule above.

## One analysis, three outputs

Dangling detection, automatic `std::move`, and smart-pointer type inference are **not
three separate features** — they are three consumers of the *same* provenance +
lifetime data. Keeping them on one shared base is a design requirement:

- **Dangling detection** — reject a use of a reference whose owner is dead.
- **Automatic `std::move`** — the "last use" of an owner is exactly a death event of
  kind *move-into-sink*. Never move a value that is still borrowed afterward — the
  same table guards this.
- **Smart-pointer type inference** — single owner ⇒ `unique_ptr`; multiple live owners
  of the same object ⇒ `shared_ptr`; back-reference / cycle edge ⇒ `weak_ptr`.

Because all three read the same table they stay mutually consistent: the analyzer will
never auto-`std::move` a value that dangling analysis knows is still borrowed, and
pointer-type choice reflects the same ownership graph used for lifetime checks.
