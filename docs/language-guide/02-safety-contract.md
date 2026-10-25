# 2. The Safety Contract

> **If a program compiles under CeeLing, it contains no dangling references.**
> *(Single-threaded. Concurrency is out of scope for v1 — see
> [Chapter 7](07-vs-rust.md) — except that threads can be started with only values and
> moved-in objects crossing the boundary: [Chapter 20](20-threads.md).)*

This is a **falsifiable, testable promise**, not a slogan. It is backed by fixtures:
the `tests/safety/*.clg` files are the test suite for the contract, run via
`tests/safety/run_safety_tests.sh` and the `cling_safety_contract_baseline` CTest
target.

## The fixtures

| Fixture | Scenario | Expected |
|---------|----------|----------|
| `observer_alive_ok` | observer used while owner still alive | **accept** (control) |
| `move_then_use` / `observer_dangles_after_move` | owner moved into a sink, then used/observed | **reject** |
| `observer_dangles_after_reassign` | owner reassigned to a fresh object, then observer used | **reject** |
| `observer_dangles_after_scope` | owner from inner scope destroyed, observer used after | **reject** |
| `ref_field_rejected` / `field_stores_owner_accepted` | an observer stored in a field escapes its owner (sealed by construction — no `ref`-qualified field production exists) | **reject** / **accept** (control) |
| `return_local_ref_rejected` / `return_owner_not_observer_accepted` | returning an observer of a function-local owner | **reject** / **accept** (control) |
| `branch_merge_no_false_positive_accepted` / `branch_merge_ambiguous_rejected` | observer state across `if`/`else` branches | **accept** (control) / **reject** |
| `summary_return_aliases_param` / `summary_return_alias_dangles` | a function's return value aliases one of its own parameters | **accept** (control) / **reject** |

Negatives that MUST be rejected, plus positive controls that MUST keep compiling
(guarding against a check that is *too* aggressive). See `tests/safety/BASELINE.md`
for the full, current pass count.

## The canonical example

In CeeLing's actual surface syntax — plain Java-like ownership, with `ref` for a
non-owning observer (no `owned<T>`/`borrowed<T>` annotations; see
[Chapter 3](03-java-like-experience.md)):

```
Node root = new Node(1);   // root: owner, live
ref Node n = root;         // provenance: n observes root
sink(root);                // root moved into sink → death: root = DEAD
std::cout << n.key << std::endl;  // use n → n observes root, root DEAD → REJECT
```

(Internally, the analyzer's provenance/death-event model is described using
`owned<T>`/`borrowed<T>` annotation notation in [Chapter 5](05-how-analysis-works.md)
— that notation is scaffolding for explaining the mechanism, not something a CeeLing
program ever writes.)

## Why the contract is more than "use smart pointers"

**"No raw pointers + all smart pointers" is necessary but far from sufficient.**
Smart pointers solve *who calls `delete`* (leaks, double-free). They do **not** stop
dangling, null deref, use-after-move, out-of-bounds, or type confusion. The full set
of threats the contract must close is in [Chapter 4](04-threat-model.md).
