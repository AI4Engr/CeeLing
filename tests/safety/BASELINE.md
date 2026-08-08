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
| `borrow_owner_alive_ok.clg` (legacy, since removed) | accept | **PASS** |
| `borrow_owner_moved_use.clg` (legacy, since removed) | reject | **XFAIL** |
| `borrow_owner_reassign_use.clg` (legacy, since removed) | reject | **XFAIL** |
| `borrow_owner_scope_end_use.clg` (legacy, since removed) | reject | **XFAIL** |
| `borrow_stored_in_field.clg` (legacy, since removed) | reject | **XFAIL** |
| `borrow_dangling_rust_example.clg` (legacy, since removed) | reject | **XFAIL** |

**Summary: PASS=2, XFAIL=9, FAIL=0.** (Frozen historical snapshot — the six legacy
`owned<>`/`borrowed<>` fixtures listed above were removed once every scenario they
covered had a modern-syntax equivalent in `tests/safety/*.clg` passing for real; see
"Current status" below.)

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

- Fixtures live in `tests/safety/` and use the intended surface syntax: **plain move
  semantics** (`Node b = a;` transfers ownership) and **`ref` observers** (`ref Node n
  = ...;` is non-owning). No `owned<>`/`borrowed<>` annotations anywhere in the suite
  — the legacy `tests/borrow_*.clg` scaffolding fixtures (annotated syntax) were
  removed once the modern suite covered every scenario they demonstrated for real
  (see "Current status" below); `owned<>`/`borrowed<>` now appears only as prose
  notation in the Language Guide (Chapters 2, 5) for explaining the analyzer's
  internal model, never in an actual `.clg` file.
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
PASS=20, XFAIL=0, FAIL=0.

- `return_local_ref_rejected.clg` — a function declares a `ref` observer of its own
  local owner, then returns the observer. CeeLing has no `ref` return type, so this
  would hand the caller a reference into a stack frame that's already gone; rejected
  with a `.clg`-level diagnostic rather than left to surface as a confusing (and
  coincidental, not guaranteed) C++ compile error downstream.
- `return_owner_not_observer_accepted.clg` — the same function shape, but returning
  the *owner* (not the observer): proves the check only flags returning the observer
  by name, not merely having one declared somewhere in the body.

Phase 2's reference-stored-in-field escape check (#3) added 2 more fixtures:
PASS=22, XFAIL=0, FAIL=0.

- `ref_field_rejected.clg` — a class field declared `ref` (`ref Node r;`). Sealed by
  construction, the same mechanism as raw pointers/casts (Ch. 17): the grammar has no
  `ref`-qualified field production, so this is a parse error, not a semantic check.
- `field_stores_owner_accepted.clg` — a method stores a moved-in parameter into a
  plain (non-`ref`) field: ordinary ownership transfer, not a borrow, and must still
  be accepted.

Phase 2's branch-merge precision fix added 2 more fixtures:
PASS=24, XFAIL=0, FAIL=0.

- `branch_merge_no_false_positive_accepted.clg` — an observer is rebound inside the
  `if` branch only; the `else` branch uses it observing its original, still-alive
  owner. Before the fix, observer-tracking state leaked across sibling branches (only
  `movedFrom_` was snapshotted/restored per branch, not the observer maps), so this
  was wrongly rejected using state left over from the `if` branch.
- `branch_merge_ambiguous_rejected.clg` — the two branches rebind the same observer to
  *different* owners. Positive control proving the fix doesn't over-correct: this case
  is genuinely ambiguous (a later use can't know which branch ran) and must stay
  rejected, per CLAUDE.md's conservative-reject rule.

The `ref` observer `T*` lowering fix (Ch. 18, Step 12) added 1 more fixture:
**PASS=25, XFAIL=0, FAIL=0.**

- `observer_nullptr_rebind_accepted.clg` — a `ref` observer starts null, then is
  rebound to a live owner before use. Proves the raw-pointer lowering handles both
  `nullptr` and rebinding correctly; also the first accept-case fixture to exercise
  `ref ... = nullptr;` through to compiled C++ (previously only reject-case fixtures
  used that pattern, so a latent null-dereference bug in the old `T&` lowering never
  actually got compiled).

**Note on compiled-output verification**: every `//@ expect: accept` fixture's
generated C++ has been spot-checked to compile clean under g++
(`-std=c++17 -fsyntax-only`) as of the `ref`-lowering fix, including
`branch_merge_no_false_positive_accepted.clg`, which previously only asserted
`cling.exe`'s own exit code (the rebind it exercises had no valid C++ lowering until
Step 12 landed).

Phase 1's default-reject-unknown-library-calls item (the last unchecked Phase 1 box)
added 2 more fixtures: **PASS=27, XFAIL=0, FAIL=0** (at the time it landed).

- `unknown_call_rejected.clg` — a call to a function CeeLing did not compile itself
  (`mysteryFunction(a, b)`), with the result assigned to an object variable. Rejected
  outright: CeeLing has no borrow/ownership summary for anything outside the program
  (`#include` lines are passed through as opaque text, never parsed for
  declarations), so treating an unknown call's return value as "a fresh, safe owner"
  by default would silently defeat the auto-derived-summary mechanism the moment a
  real external call existed. `std::cout << ...` stream output is exempt by
  construction (a distinct AST shape, never a function call).
- `known_call_accepted.clg` — positive control: a free function and a bare-name
  (implicit-`this`) method call, both CeeLing-defined, are not flagged.

**Then the 6 legacy `owned<>`/`borrowed<>` fixtures were removed** (`tests/borrow_*.clg`
— `borrow_owner_alive_ok.clg`, `_moved_use.clg`, `_reassign_use.clg`,
`_scope_end_use.clg`, `borrow_stored_in_field.clg`, `borrow_dangling_rust_example.clg`),
bringing the count to PASS=21. Every scenario they demonstrated has a modern-syntax
fixture in `tests/safety/` that actually exercises the real checker (the legacy files
only "passed" their `//@ expect: reject` tag by failing to parse at all — see the
`borrow_owner_alive_ok.clg` note above for why that was never a meaningful test of the
scenario itself). Language Guide Chapters 2 and 11, which previously cited these files
by name as worked examples, were rewritten to use the actual modern syntax and cite
the real `tests/safety/*.clg` fixtures instead.

Phase 2.6 slice 1 (`std::vector<T>` minimal usable set — Ch. 18, Step 13) added 4 more
fixtures: **PASS=25, XFAIL=0, FAIL=0.**

- `vector_push_back_and_for_accepted.clg` — declare, `push_back`, range-`for` iterate
  with field access on the loop variable. The end-to-end proof this slice is usable;
  compiles clean under g++.
- `vector_push_back_shares_when_already_shared_accepted.clg` — an already-shared
  source pushed into a vector stays usable afterward (the "shares" case).
- `vector_push_back_moves_unique_source_rejected.clg` — positive control: a plain
  (never-shared) source is consumed by `push_back`, same as any other call argument.
  Proves `push_back` isn't unconditionally non-consuming.
- `vector_moved_into_call_rejected.clg` — a container variable passed to an ordinary
  function participates in the same move-tracking a class object would. Also the
  fixture that surfaced a genuine pre-existing bug (not container-specific): a
  method call's receiver (`v` in `v.size()`) was never checked for moved-from status,
  only the call's arguments — fixed alongside this fixture, see Ch. 18.
