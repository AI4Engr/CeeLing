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
- **Every dangling scenario in the current syntax is caught**, verified by
  `tests/safety/*.clg`. The old `owned<>`/`borrowed<>`-annotated fixtures that once
  demonstrated the same scenarios in a superseded syntax have been removed — every
  scenario they covered now has a real, passing modern-syntax equivalent.
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
- **Reference-stored-in-field escape** (#3) — sealed by construction like the Chapter
  17 escape hatches, not by semantic analysis: the grammar has no `ref`-qualified
  field production, so `ref Node r;` inside a class body is not well-formed CeeLing.
  Combined with the existing ban on `ref` function parameters, there is no remaining
  syntactic path to get an actual borrow stored in a field. See
  `tests/safety/ref_field_rejected.clg` / `field_stores_owner_accepted.clg`.
- **Branch-merge precision** — `if`/`else` arms (and a `while` body vs. skipping it)
  are each analyzed from the same snapshot, then merged: an effect in either branch
  holds afterward, and if the branches disagree about what an observer points at, it's
  conservatively marked dangling rather than guessed at. This closed a real false
  positive where a rebind inside one branch leaked into the sibling branch's view. See
  `tests/safety/branch_merge_no_false_positive_accepted.clg` /
  `branch_merge_ambiguous_rejected.clg`.
- **Default-reject unknown library calls** — a call to anything CeeLing did not
  itself compile (no free function/method declaration in the program) is rejected
  outright, closing the gap where an unknown call's return value was silently
  trusted as "a fresh, safe owner." `std::cout << ...` stream output is exempt by
  construction (never a `FunctionCall` node). See
  `tests/safety/unknown_call_rejected.clg` / `known_call_accepted.clg`.
- **`std::vector<T>` (Phase 2.6, slice 1)** — declaration (with or without an
  initializer — the one definite-init exception, a container's empty state is
  always safe), `push_back`/`size`/`begin`/`end` (the first real library-summary
  allow-list entry), and range-`for` iteration. A vector's element pointer kind
  (`shared_ptr` or `unique_ptr`, for class-typed elements) is decided once across
  every `push_back` call site targeting it, and container variables participate in
  the same move/observer tracking as class objects. `operator[]`, `map`/`string`,
  and iterator-invalidation analysis are deferred to the next slice. See
  `tests/safety/vector_push_back_and_for_accepted.clg` and siblings, and Chapter
  18's implementation notes for the full design.

**Not yet done:**
- The analyzer still also runs the older **smart-pointer type selection**
  (`unique_ptr`/`shared_ptr`/`weak_ptr`), not yet unified onto the new engine.
- **Library/std/Eigen summaries** — only `std::vector<T>`'s four methods are on the
  allow-list; every other external call is still rejected unconditionally (the
  correct conservative default).
- `operator[]`, `std::map`/`std::string`, iterator invalidation, container-held
  observers, `string_view`/Eigen dangling — next Phase 2.6 slices.

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
  `owned<>`/`borrowed<>`. The legacy annotated `borrow_*.clg` fixtures have since been
  removed (see Phase 2's "Migrate/retire" item in `TODO.md`).
- ✅ **Baseline snapshot** — `tests/safety/BASELINE.md` (Phase 0 frozen at PASS=2,
  XFAIL=9, FAIL=0; see the file for the full current count as later phases landed).
- ✅ **CTest integration** — `cling_safety_contract_baseline`.
- ✅ **Legal-C++ output check** — `AstEmitter` is now the default generator and emits
  compilable C++ (verified under g++ via `cling_test_ast_return_object_compile` and
  the other `_compile` CTest targets); the custom `print` is gone in favor of native
  `std::cout << ...`. The legacy regex generator is available via `--legacy-cpp` for
  comparison only.

### Phase 1 — Seal escape hatches (simplest + foundational)  ✅ done
Forbid raw `T*`, raw `new`/`delete`, `.get()`, `reinterpret_cast` / C-style casts,
`const_cast`, raw `union`; require definite initialization (locals); default-reject
any call to a function/method CeeLing did not itself compile. All done — see Chapter
17 for the mechanism and worked diagnostic text. The default-reject-unknown-calls
rule rejects every external call today (there is no library-summary allow-list yet,
so nothing outside the program has a known-safe contract) — a real allow-list letting
specific std/Eigen calls through is tracked under Phase 2.

### Phase 2 — Dangling-reference analysis (core)  ✅ done except library summaries
Provenance table, death events, use-site check are implemented and cover all 5
new-syntax dangling fixtures (owner moved/reassigned/scope-exited, observer-parameter
ban), plus auto-derived return-provenance summaries (CLAUDE.md component A — scoped to
return-value aliasing, since parameters are always-moved by construction),
return-local-reference (#2), reference-stored-in-field escape (#3, sealed by
construction — no `ref`-qualified field production exists), and branch-merge
precision (see "Working today" above). Still open: library summaries, iterator
invalidation (#4), `string_view`/`span` (#5), `auto`-vs-concrete provenance (#6),
lambda `[&]` escape (#7), dangling temporaries (#8), Eigen summaries — all gated on
language features (containers, `auto`, lambdas) or a library-summary allow-list that
don't exist yet (Phase 2.6+). The legacy `borrow_*.clg` (`owned<>/borrowed<>`)
fixtures are superseded by the new syntax and kept only for regression.

Found but not fixed during this phase (a separate, pre-existing emitter gap, not a
dangling-analysis one): rebinding a `ref` observer (`obs = b;`) has no valid C++
lowering — a native `T&` can't be rebound after initialization, so `AstEmitter`'s
current assign-through lowering is wrong for this specific case. The checker itself
tracks CeeLing's intended rebind semantics correctly.

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
