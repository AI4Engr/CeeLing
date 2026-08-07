# Safety Suite Baseline (Phase 0)

This snapshot records the **honest starting state** of the memory-safety contract, so
progress in later phases is measurable. Regenerate with:

```bash
bash tests/safety/run_safety_tests.sh
```

## Baseline snapshot

Captured against `build/cling.exe` with **no safety analysis implemented**
(`src/core/` has zero enforcement logic as of this baseline).

| Fixture | Expect | Baseline result |
|---------|--------|-----------------|
| `safety/observer_alive_ok.clg` | accept | **PASS** |
| `safety/move_then_use.clg` | reject | **XFAIL** (missed — use-after-move not implemented) |
| `safety/observer_dangles_after_move.clg` | reject | **XFAIL** (missed — dangling not implemented) |
| `safety/observer_dangles_after_reassign.clg` | reject | **XFAIL** |
| `safety/observer_dangles_after_scope.clg` | reject | **XFAIL** |
| `borrow_owner_alive_ok.clg` (legacy) | accept | **PASS** |
| `borrow_owner_moved_use.clg` (legacy) | reject | **XFAIL** |
| `borrow_owner_reassign_use.clg` (legacy) | reject | **XFAIL** |
| `borrow_owner_scope_end_use.clg` (legacy) | reject | **XFAIL** |
| `borrow_stored_in_field.clg` (legacy) | reject | **XFAIL** |
| `borrow_dangling_rust_example.clg` (legacy) | reject | **XFAIL** |

**Summary: PASS=2, XFAIL=9, FAIL=0.**

## What the numbers mean

- **PASS (2):** the positive controls compile, confirming the harness does not
  over-reject valid code.
- **XFAIL (9):** every unsafe fixture currently passes through the compiler — a
  false-negative against the safety contract. These are *known gaps*, tracked so the
  baseline stays green rather than pretending the work is done.
- **FAIL (0):** no valid program is wrongly rejected, and no unexpected error.

## How this evolves

- As **Phase 2+** lands, each XFAIL fixture starts getting correctly rejected and
  flips to real **PASS**. Run with `--strict` once the analysis exists so a missed
  reject counts as a hard failure.
- If a positive control ever flips to FAIL, the analysis has become **over-aggressive**
  (a false positive) — also a bug.

## Fixture conventions

- New-syntax fixtures live in `tests/safety/` and use the intended surface syntax:
  **plain move semantics** (`Node b = a;` transfers ownership) and **`ref` observers**
  (`ref Node n = ...;` is non-owning). No `owned<>`/`borrowed<>` annotations.
- The `tests/borrow_*.clg` files are **legacy** scaffolding (annotated syntax),
  retained for regression and tagged `//@ legacy:`.
- Each fixture declares its expectation with a `//@ expect: reject|accept` comment.

## Current status (Phase 1 landed)

This section is updated as phases land; the snapshot above stays frozen as the
Phase 0 historical starting point.

Phase 1 (escape-hatch sealing) added 4 fixtures and closed all remaining gaps in the
suite: PASS=16, XFAIL=0, FAIL=0.

- `get_on_object_rejected.clg` / `get_on_user_method_accepted.clg` — `.get()` is
  rejected only when the receiver is a compiler-generated smart pointer (a CeeLing
  object), never a user-defined method of the same name.
- `raw_pointer_type_rejected.clg` / `raw_delete_rejected.clg` — raw pointer types and
  raw `delete` are rejected (casts and raw `union` are covered too, without dedicated
  fixtures, since they hit the same mechanism as `delete`).
- `borrow_owner_alive_ok.clg` flipped from `accept` to `reject`: it was never valid
  CeeLing grammar (`owned<Node>`/`borrowed<Node>` are not real types) and only
  "passed" before because of a bug where a body statement that failed to parse was
  silently dropped instead of raising an error. Fixing that bug (a Phase 1
  prerequisite -- see Ch. 18) made this fixture's true state visible: rejected, like
  its five sibling `borrow_*.clg` fixtures. The real positive control for "a borrow
  used while its owner is alive" is `safety/observer_alive_ok.clg`, which still PASSes.

Phase 2's auto-derived function summaries (return-provenance) added 2 more fixtures:
PASS=18, XFAIL=0, FAIL=0.

- `summary_return_aliases_param.clg` — a function whose summary says its return value
  aliases a parameter (`Node pick(Node a, Node b) { return a; }`) is used safely at the
  call site; proves the summary mechanism doesn't over-reject the passthrough shape.
- `summary_return_alias_dangles.clg` — the same summary-derived alias, but the aliased
  owner is reassigned before the alias is used again: rejected, the same way an
  explicit `ref` observer would be. This is the new catch the summary mechanism
  enables -- without it, `Node r = pick(x, y);` registered no provenance at all, so
  this dangling use previously went undetected.

Phase 2's return-local-reference check (#2) added 2 more fixtures:
**PASS=20, XFAIL=0, FAIL=0.**

- `return_local_ref_rejected.clg` — a function declares a `ref` observer of its own
  local owner, then returns the observer. CeeLing has no `ref` return type, so this
  would hand the caller a reference into a stack frame that's already gone; rejected
  with a `.clg`-level diagnostic rather than left to surface as a confusing (and
  coincidental, not guaranteed) C++ compile error downstream.
- `return_owner_not_observer_accepted.clg` — the same function shape, but returning
  the *owner* (not the observer): proves the check only flags returning the observer
  by name, not merely having one declared somewhere in the body.
