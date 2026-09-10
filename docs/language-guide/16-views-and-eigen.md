# 16. Dangling Views: `string_view`, `span`, Eigen

Modern C++'s newest dangling hotspots are **non-owning views** — objects that *look
like values* but actually reference someone else's storage. Eigen's expression
templates are the same trap in numeric code.

## Implementation status (honest, read this before the rest of the chapter)

This chapter was written as a **design target** before any of it existed, and most
of it still is one: `string_view`/`span` dangling tracking is **not implemented**,
and CeeLing has **no `auto` keyword at all** (confirmed —
`tree-sitter-ceeling/grammar.js`'s `variable_declaration` requires an explicit
type; no type-inference production exists anywhere in the grammar). Every `auto e
= A + B;` example below is therefore describing a danger CeeLing **cannot express**,
not one it resolves at runtime — the entire "declared type flips the meaning"
mechanism below (`auto` vs. concrete type) is real for hand-written C++ and for
`cppfront`/Circle, but for CeeLing it is a non-issue by construction, not a caught
error. This is a *favorable* finding, not a gap: the single hardest part of this
chapter's original design (provenance through `auto`) turned out to be moot.

What **is real and shipped**, as a narrow first slice of this chapter's `.block()`/
`.row()` idea (not the full design below — see the honest scope limits at the end
of this note before relying on it):
- `Eigen::MatrixXd` participates in ordinary move/copy tracking exactly like any
  CeeLing `class` — `sink(a); use(a);` is a real, caught use-after-move; declaring
  `MatrixXd b = a;` is a real, accepted copy.
- Eight view-returning methods — `.block()`, `.row()`, `.col()`, `.transpose()`,
  `.segment()`, `.head()`, `.tail()`, `.diagonal()` — are recognized as borrowing
  their receiver: `MatrixXd b = a.block(...);` registers `b` as an observer of `a`,
  and the SAME dangling machinery Chapter 11 describes for `ref` (provenance +
  death events + use-site check) catches a later reassignment or move of `a` before
  `b` is used. See `tests/safety/`'s sibling directory `eigen-cling-spike/` for the
  four fixtures proving both directions (dangling caught, and the source usable
  again afterward when it's *not* reassigned/moved).
- This does **not** yet come from reading Eigen's own header source automatically
  (the "one summary line" idea below, `Eigen::MatrixXd::block(&self, ...) -> borrows
  self`, is accurate in spirit but is currently a small hand-maintained allow-list of
  8 method names, not a general per-library declaration mechanism or an
  auto-extracted one). Any Eigen view method NOT in that list of 8 (`topRows()`,
  `array()`, ...) is rejected by the ordinary default-reject-unknown-call rule —
  the safe failure mode, not a silent hole. Full design and exact implementation
  in `PLAN_eigen_views.md` (repo root) and `TODO.md`'s "Eigen view/borrow
  tracking" entry.
- **Chained views** (`a.block(...).transpose()`) specifically remain rejected,
  and deliberately so, even though chained method calls are now supported in
  general for CeeLing's own classes (Chapter 9's roadmap entry, Chapter 18
  Step 36). That fix is safe for CeeLing's own classes because every one of
  them is a compiler-managed smart pointer — a method's return value is
  always a live, owning copy. Eigen types keep plain C++ value/reference
  semantics on purpose (that is the whole point of this chapter's view
  tracking), so a chained view's receiver really is an unnamed temporary
  with real dangling risk if mishandled — closing this needs genuine
  transitive-borrow-provenance design (does a view's own borrow propagate
  through a SECOND view built on top of it?), not just wider call
  resolution. Tracked as a separate, harder follow-up in `TODO.md`'s Eigen
  entry; not attempted by the general chained-call fix.

Read the rest of this chapter as: **the target design**, with the two paragraphs
above telling you exactly which piece of it is real today.

## The problem in raw C++

### `string_view` / `span` over a temporary

```cpp
std::string_view sv = std::string("hello");   // temporary string dies at ; → sv dangles
std::cout << sv;                               // ← UB

std::span<int> s = makeVector();               // returned vector destroyed → s dangles
s[0];                                          // ← UB
```

### Eigen expression templates — the `auto` trap

```cpp
Eigen::MatrixXd A = ..., B = ...;
auto e = A + B;      // e is NOT a matrix — it is CwiseBinaryOp<...,const MatrixXd&,...>
                     // holding REFERENCES to A and B
A = somethingElse;   // A's buffer freed
Eigen::MatrixXd r = e;   // ← evaluates e, reads dead A → UB
```

The killer detail: **the declared type flips the meaning.**

```cpp
auto e     = A + B;  // borrows A, B  (lazy expression — dangling risk)
MatrixXd e = A + B;  // evaluates now → owned copy (safe)
```

## How CeeLing solves it

View/expression types carry **provenance to what they view**, and the analyzer keys
on the **declared type of the LHS**:

- A view/expression bound to a **temporary** → the temporary is a scope-exit death
  event; using the view afterward is rejected.
- **`auto e = A + B`** → provenance `e <= {A, B}` (keep tracking); if `A` or `B` dies
  before `e` is used → reject.
- **`MatrixXd e = A + B`** → concrete value type forces evaluation → **break
  provenance** (`e` is an owned copy, safe).

Eigen is handled with **no knowledge of Eigen internals** — only summaries:

```
Eigen::MatrixXd::block(self:&, ...) -> view borrowing self
Eigen::MatrixXd::row(self:&, i)     -> view borrowing self
Eigen::operator+(a:&, b:&)          -> expression borrowing a, b
```

## Proof by example

### Rejected — view of a temporary

```
StringView sv = makeString();   // temporary dies at end of statement
std::cout << sv << std::endl;                      // ❌ REJECT: `sv` views a destroyed temporary
```

### Rejected — Eigen expression outlives an operand

```
Matrix A = ...; Matrix B = ...;
auto e = A + B;       // provenance: e <= {A, B}
A = other;            // A DEAD
Matrix r = e;         // ❌ REJECT: `e` references `A`, which was overwritten
```

### Accepted — force evaluation with a concrete type

```
Matrix A = ...; Matrix B = ...;
Matrix e = A + B;     // concrete type → owned copy, provenance broken
A = other;            // fine
Matrix r = e;         // ✅ ACCEPT
```

### Rejected — block view outlives a resize

```
Matrix m = ...;
auto blk = m.block(0, 0, 2, 2);   // view borrows m
m = bigger;                        // m's storage replaced → blk DEAD
std::cout << blk << std::endl;                        // ❌ REJECT
```

The lesson: CeeLing treats "looks like a value but is actually a reference" uniformly
— `string_view`, `span`, and Eigen expressions all flow through the same provenance
rule, and the `auto`-vs-concrete distinction is what makes the safe idiom safe.

## Why CeeLing *can* check Eigen (when Circle and Rust cannot)

This is the most counter-intuitive result in the whole design: Eigen is a huge,
template-heavy library, yet CeeLing can catch its dangling bugs — and it does so
**without understanding one line of Eigen's internals.** The reason comes down to a
single sentence:

> **Dangling analysis does not need to understand Eigen. It only needs one fact per
> method — the method's *borrow behavior* — and that fact is independent of how Eigen
> is implemented.**

Three steps make this true:

1. **Dangling is a *liveness* question, not a *type* question.** A dangle is always
   "a reference used after the object it points to has died." To decide that, the
   analyzer needs only: who references whom (provenance), when an object dies (death
   event), and where a reference is used (use site). **None of those needs to know
   whether the referenced object is a `MatrixXd` or a hand-written `Node`.** To the
   analyzer, an Eigen matrix and a user class are the same thing: a value with a
   lifetime.

2. **Eigen's only "special" behavior — do its methods return borrows? — is captured
   by one signature line.** Eigen is dangerous because `.block()`/`.row()` return
   *reference views* and `A + B` returns an *expression template that borrows A and B*.
   The analyzer does not need to know *how* those are implemented — only a declaration:
   ```
   Eigen::MatrixXd::block(&self, ...) -> borrows self
   Eigen::operator+(&a, &b)           -> borrows a, b
   ```
   With that, provenance propagation automatically marks `auto blk = m.block(...)` as
   "borrows m," and a later death of `m` triggers the reject. **Eigen stays a black box;
   the analyzer only reads this summary table.**

3. **CeeLing can supply that table; Circle and Rust cannot use it.** This is the
   decisive asymmetry:
   - **CeeLing** hand-writes Eigen's borrow summaries directly. Eigen's implementation
     is never touched — it compiles into the emitted C++ unchanged.
   - **Circle / Rust** would first have to admit Eigen into their *aliasing* model.
     Circle requires **exclusivity** (mutable-xor-shared); Eigen's reference views
     *inherently* alias, so they violate exclusivity and are pushed into `unsafe`, where
     the borrow checker is off (see [Chapter 8](08-landscape.md)). Rust has no Eigen at
     all.

**The punchline:**

> **CeeLing can check Eigen not because it asks for *more* than Circle/Rust, but
> because it asks for *less*.** It checks only *liveness*, never *aliasing*. Eigen can
> satisfy "liveness is analyzable" (via summaries) but cannot satisfy "aliasing is
> exclusive" (its views alias by nature). **Giving up aliasing checks is precisely what
> lets Eigen live inside CeeLing's safety boundary** rather than outside it.

## The honest boundary

This power is bounded by one thing: **CeeLing's Eigen coverage equals how many borrow
summaries have been written for Eigen.** A method with no summary is rejected by the
default-reject rule — safe, but temporarily unusable. So the guarantee is *safe first,
usable as the summary table grows* (see [Chapter 5](05-how-analysis-works.md) and the
roadmap in [Chapter 9](09-status-and-roadmap.md)). Eigen is not magically fully covered
— it is covered exactly as far as its allow-list reaches, and no further.

Concretely, today that allow-list has exactly 8 entries (see the implementation-status
note at the top of this chapter) — real tracking, verified end-to-end against real
Eigen headers, but a small, hand-maintained slice of Eigen's full view-returning
surface, not the fully general "any borrow summary" mechanism this chapter otherwise
describes.
