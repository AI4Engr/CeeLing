# 9. Status & Roadmap

## Honest current state

Most safety features in this guide are still **specification**, but Phase 0 (harness),
Phase 1 (escape-hatch sealing), the new-syntax slice of Phase 2 (dangling analysis),
Phase 3 (use-after-move), and Phase 2.5 (AST-driven generator, now the default) are
real.

**Working today** (see [Chapter 18](18-implementation-notes.md)):
- **Use-after-move** — using a value after it was moved (into a function or another
  variable) is rejected, with a `.clg` line number.
- **Observer dangling** — a `ref` observer used after its owner is moved away,
  reassigned, or falls out of scope is rejected.
- **Observer-parameter ban** — `void sink(ref Node n)` is rejected at parse time
  (observers are local-only; this seals a whole class of escape bug by construction).
- **Every dangling scenario in the current syntax is caught.** Only the old,
  superseded `owned<>`/`borrowed<>` fixtures (kept around for regression, now
  correctly rejected as unparseable rather than silently accepted) are not covered by
  the modern dangling analysis — their intent is covered by the `tests/safety/*.clg`
  equivalents.
- **Escape hatches are sealed** (Phase 1) — `.get()` on a CeeLing object, raw pointer
  types, raw `delete`, casts, and raw `union` are all rejected with an explanatory
  diagnostic. See [Chapter 17](17-escape-hatches.md) for the mechanism and worked
  examples.
- **`AstEmitter` is the default generator** — it walks the validated AST, so a
  construct CeeLing cannot express structurally cannot appear in the output. This is
  what makes the Phase 1 guarantee real end-to-end (rejecting a construct in the AST
  is meaningless if the generator can still emit it from raw source text — see
  Chapter 18 for why this forced the generator switch). Coverage includes classes,
  functions, `if`/`while`, field access, `this->`, member-target assignment,
  auto-`std::move`, return-value move, `#include` passthrough, and native
  `std::cout << ...` stream output. The old regex `CppGenerator` remains available via
  `--legacy-cpp` for comparison/debugging only — it does not offer the Phase 1
  guarantee, since it builds output from raw source text rather than the AST.
- **Auto-derived return-provenance summaries** (CLAUDE.md component A) — CeeLing bans
  `ref` function parameters, so every object-typed parameter is unconditionally moved
  into the callee already; the only thing a summary needs to capture is whether a
  function's return value aliases one of its own parameters (`T f(T a){ return a; }`)
  or is always fresh. Computed once per function/method before the dangling check
  runs; a call result assigned to a variable consults it and registers the right
  provenance (alias → observer of the argument, same machinery as `ref`; fresh → new
  owner) instead of assuming every call returns something brand new. See
  `tests/safety/summary_return_aliases_param.clg` / `summary_return_alias_dangles.clg`.
- **Return-local-reference** (#2) — CeeLing has no `ref` return type, so a function's
  return is always owning; `return b;` is rejected whenever `b` is a `ref` observer
  declared in that function's own body (it can only be observing something local,
  since `ref` parameters are already banned). See
  `tests/safety/return_local_ref_rejected.clg` /
  `return_owner_not_observer_accepted.clg`.

**Not yet done:**
- The analyzer still also runs the older **smart-pointer type selection**
  (`unique_ptr`/`shared_ptr`/`weak_ptr`), not yet unified onto the new engine.
- **Default-reject unknown library calls** (the remaining Phase 1 item) — no
  library-summary allow-list exists yet; tracked under Phase 2.
- **Reference-stored-in-field escape** (#3) — storing a `ref` observer into a
  longer-lived field is not yet checked.
- **Library/std/Eigen summaries** — only CeeLing's own functions get an auto-derived
  summary today.
- Branch merges (`if`/`else` arms are checked independently, not merged), iterator
  invalidation, `string_view`/Eigen dangling — still open.

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
- ✅ **Legal-C++ output check** — `AstEmitter` is now the default generator and emits
  compilable C++ (verified under g++ via `cling_test_ast_return_object_compile` and
  the other `_compile` CTest targets); the custom `print` is gone in favor of native
  `std::cout << ...`. The legacy regex generator is available via `--legacy-cpp` for
  comparison only.

### Phase 1 — Seal escape hatches (simplest + foundational)  ✅ mostly done
Forbid raw `T*`, raw `new`/`delete`, `.get()`, `reinterpret_cast` / C-style casts,
`const_cast`, raw `union`; require definite initialization (locals). All done — see
Chapter 17 for the mechanism and worked diagnostic text. Still open: default-reject
unknown library calls (needs the Phase 2 library-summary allow-list first).

### Phase 2 — Dangling-reference analysis (core)  ✅ new-syntax fixtures + auto summaries + #2 done
Provenance table, death events, use-site check are implemented and cover all 5
new-syntax dangling fixtures (owner moved/reassigned/scope-exited, observer-parameter
ban), plus auto-derived return-provenance summaries (CLAUDE.md component A — scoped to
return-value aliasing, since parameters are always-moved by construction) and
return-local-reference (#2) (see "Working today" above). Still open: library
summaries, field escape (#3), iterator invalidation (#4), `string_view`/`span` (#5),
`auto`-vs-concrete provenance (#6), lambda `[&]` escape (#7), dangling temporaries
(#8), Eigen summaries, branch-merge precision. The legacy `borrow_*.clg`
(`owned<>/borrowed<>`) fixtures are superseded by the new syntax and kept only for
regression.

### Phase 3 — Use-after-move (coupled to auto-`std::move`)  ✅ done
Moved-from state tracking; reject use of a moved-from value. Implemented in
`dangling_checker.cpp` and reused by the AST-driven generator to justify inserting
`std::move` automatically (Phase 2.5, below).

### Phase 2.5 — AST-driven code generator (new architecture)  ✅ done, now default
Replaces text/regex substitution with a generator that walks the AST directly —
required because C++'s `.`/`->` choice, `this->` field access, and ownership-transfer
points cannot be determined from source text alone (see Chapter 18, Step 6 for the
concrete failures this caused). It is also what makes Phase 1 a real guarantee rather
than a checked-but-bypassable AST property (Chapter 18, Step 7): the legacy generator
builds output from raw source text, so it can still emit a construct the AST rejects.
`AstEmitter` is now the default (no flag needed); `--legacy-cpp` opts back into the
old generator for comparison/debugging. Working: classes/functions/main, if/else/
while, comparisons, `&&`/`||`, `ref`->reference, `this->`, chained member/method
access of arbitrary depth (`a.b.c.d`), member-target assignment (`a.b = x;`),
auto-move, return-value move (object return type lowered to a smart pointer, owning
local moved out), custom `#include` passthrough, and native `std::cout << ...` stream
output.

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
