# 2. The Safety Contract

> **If a program compiles under CeeLing, it contains no dangling references.**
> *(Single-threaded. Concurrency is out of scope for v1 — see
> [Chapter 7](07-vs-rust.md).)*

This is a **falsifiable, testable promise**, not a slogan. It is backed by fixtures:
the `tests/borrow_*.clg` files are the first test suite for the contract.

## The fixtures

| Fixture | Scenario | Expected |
|---------|----------|----------|
| `borrow_owner_alive_ok` | borrow used while owner still alive | **accept** (control) |
| `borrow_owner_moved_use` | owner moved into a sink, then borrow used | **reject** |
| `borrow_owner_reassign_use` | owner reassigned to a fresh object, then borrow used | **reject** |
| `borrow_owner_scope_end_use` | owner from inner scope destroyed, borrow used after | **reject** |
| `borrow_stored_in_field` | borrow stored in a field, escapes its owner | **reject** |
| `borrow_dangling_rust_example` | canonical Rust dangling example, transliterated | **reject** |

Five negatives that MUST be rejected, plus positive controls that MUST keep
compiling (guarding against a check that is *too* aggressive).

## The canonical example

Equivalent to what Rust rejects (`owned`/`borrowed` here are the analyzer's internal
annotations, **not** the intended user-facing syntax, which stays plain Java-like):

```
owned<Node> root = new Node(1);   // root: owner, live
borrowed<Node> n = borrow(root);  // summary: return aliases param → provenance: n <= root
drop(root);                       // summary: param is moved       → death: root = DEAD
print(n.key);                     // use n → n <= root, root DEAD  → REJECT
```

## Why the contract is more than "use smart pointers"

**"No raw pointers + all smart pointers" is necessary but far from sufficient.**
Smart pointers solve *who calls `delete`* (leaks, double-free). They do **not** stop
dangling, null deref, use-after-move, out-of-bounds, or type confusion. The full set
of threats the contract must close is in [Chapter 4](04-threat-model.md).
