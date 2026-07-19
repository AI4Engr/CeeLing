# 9. Status & Roadmap

## Honest current state

Most safety features in this guide are still **specification**, but Phase 0 (harness)
and the first slice of Phase 2 (dangling analysis) are now real.

**Working today** (see [Chapter 18](18-implementation-notes.md)):
- **Use-after-move** — using a value after it was moved (into a function or another
  variable) is rejected, with a `.clg` line number.
- **Observer dangling** — a `ref` observer used after its owner is moved away is
  rejected.
- **Observer-parameter ban** — `void sink(ref Node n)` is rejected at parse time
  (observers are local-only).
- **Reassignment kills observers** — after `a = new Node(2);`, an observer of the old
  object is rejected on use.
- **Scope-exit kills observers** — an owner declared inside a `{ }` block dies when the
  block ends; an observer of it is rejected on use afterward.
- Safety suite: **PASS=7, XFAIL=5, FAIL=0**. **All new-syntax dangling fixtures now
  pass**; the 5 remaining XFAIL are the legacy `owned<>/borrowed<>` fixtures, kept for
  regression only.

**Not yet done:**
- The analyzer still also runs the older **smart-pointer type selection**
  (`unique_ptr`/`shared_ptr`/`weak_ptr`), not yet unified onto the new engine.
- No escape-hatch bans yet (`.get()`, casts, raw pointers) — Phase 1.
- Remaining dangling cases (scope-exit, reassignment, branch merges, library
  summaries) are still XFAIL.
- Generated C++ still leaks `print` and does not yet lower `ref` locals to
  `Node& n = *a;`.

This chapter is the plan; only the "working today" list above is done.

## Implementation order

Order matters: **seal the escape hatches first** — a single hole voids the whole
contract — then dangling analysis, then use-after-move, then runtime backstops.

### Phase 0 — Test harness & baseline  ✅ DONE (harness)
Without a red/green baseline none of the later phases can be verified.
- ✅ **Expect-reject / expect-accept runner** — `tests/safety/run_safety_tests.sh`
  scans `//@ expect: reject|accept` tags and asserts exit code per fixture. Missed
  rejects are **XFAIL** (analysis not yet implemented); over-rejecting a valid program
  is a hard **FAIL**. `--strict` flips XFAIL to FAIL once the analysis lands.
- ✅ **New-syntax fixtures** — `tests/safety/*.clg` use the intended surface syntax:
  **plain move** (`Node b = a;`) and **`ref` observers** (`ref Node n = ...;`), no
  `owned<>`/`borrowed<>`. Legacy annotated fixtures are tagged `//@ legacy:`.
- ✅ **Baseline snapshot** — `tests/safety/BASELINE.md` (PASS=2, XFAIL=9, FAIL=0).
- ✅ **CTest integration** — `cling_safety_contract_baseline`.
- ⏳ **Legal-C++ output check** (remaining) — generated C++ still leaks `print` and
  mishandles `&` locals; gate output on "compiles under a real C++ compiler." Folded
  into Phase 1/2 where that codegen is fixed.

### Phase 1 — Seal escape hatches (simplest + foundational)
Forbid raw `T*`, raw `new`/`delete`, `.get()`, `reinterpret_cast` / C-style casts,
`const_cast`, raw `union`; require definite initialization; default-reject unknown
library calls.

### Phase 2 — Dangling-reference analysis (core)
Provenance table, death events, use-site check, auto + library summaries, plus:
return-local-reference (#2), field escape (#3), iterator invalidation (#4),
`string_view`/`span` (#5), `auto`-vs-concrete provenance (#6), lambda `[&]` escape
(#7), dangling temporaries (#8), Eigen summaries, error mapping. Verify all
`borrow_*.clg` fixtures go green.

### Phase 3 — Use-after-move (coupled to auto-`std::move`)
Moved-from state tracking; reject use of a moved-from value.

### Phase 4 — Runtime backstops
Compiler-inserted bounds checks (#11) and null checks (#9) that panic/throw.

### Phase 5 — Leak prevention
`shared_ptr` cycle detection (#12) → `weak_ptr`.

## Out of scope for v1

Data-race / concurrency safety (#19). See [Chapter 7](07-vs-rust.md).

## What "done enough" means

The minimum for the single-threaded 99.99% contract:

> **dangling (#1–#8) + null (#9) + use-after-move (#10) + bounds (#11) + sealed
> escape hatches (#13–#18).**

Language expressiveness (generics, lambdas, range-for, operator overloading,
inheritance) is tracked separately in `TODO.md` — safety is the foundation,
expressiveness is the house built on top.
