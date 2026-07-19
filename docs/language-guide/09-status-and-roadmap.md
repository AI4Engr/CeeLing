# 9. Status & Roadmap

## Honest current state

The safety features described in this guide are **specification, not yet
implemented** — with the exception of the Phase 0 test harness, which is done.

- The analyzer currently performs **only smart-pointer type selection**
  (`unique_ptr` / `shared_ptr` / `weak_ptr`). It does **no** lifetime / dangling
  checking.
- Every dangling / use-after-move fixture currently **passes through** the compiler —
  false negatives against the safety contract, tracked as XFAIL in the Phase 0 suite.
- `src/core/` contains **zero safety-enforcement logic**: no raw-pointer ban, no
  `.get()` / cast enforcement, no dangling analysis.
- Generated C++ still leaks `print` tokens and mishandles `ref` observer locals (the
  `ref` keyword is passed through instead of lowering to `Node& n = *a;`) — not valid
  C++ yet.

This chapter is the plan; do not read it as "already works."

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
