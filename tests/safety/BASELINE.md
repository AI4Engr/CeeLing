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
