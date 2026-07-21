# 9. Status & Roadmap

## Honest current state

Most safety features in this guide are still **specification**, but Phase 0 (harness),
the new-syntax slice of Phase 2 (dangling analysis), and Phase 3 (use-after-move) are
now real — plus a new AST-driven code generator running alongside the old one.

**Working today** (see [Chapter 18](18-implementation-notes.md)):
- **Use-after-move** — using a value after it was moved (into a function or another
  variable) is rejected, with a `.clg` line number.
- **Observer dangling** — a `ref` observer used after its owner is moved away,
  reassigned, or falls out of scope is rejected.
- **Observer-parameter ban** — `void sink(ref Node n)` is rejected at parse time
  (observers are local-only; this seals a whole class of escape bug by construction).
- **Every dangling scenario in the current syntax is caught.** Only the old,
  superseded `owned<>`/`borrowed<>` fixtures (kept around for regression) are not yet
  covered.
- **A new code generator** (opt-in today) produces correct C++ for cases the old one
  couldn't handle reliably — field access inside methods, chained access like
  `a.b.c()`, assigning into a field (`a.b = x;`), and automatic `std::move`. See
  Chapter 18, Step 6.

**Not yet done:**
- The analyzer still also runs the older **smart-pointer type selection**
  (`unique_ptr`/`shared_ptr`/`weak_ptr`), not yet unified onto the new engine.
- No escape-hatch bans yet (`.get()`, casts, raw pointers) — Phase 1.
- Branch merges (`if`/`else` arms are checked independently, not merged), library
  summaries, iterator invalidation, `string_view`/Eigen dangling — still open.
- `AstEmitter` is not yet the default generator: custom `#include` passthrough and
  native `std::cout <<` stream input are still missing. (Returning an object from a
  function now works — the return type is lowered to a smart pointer and the value is
  moved out.) The regex `CppGenerator` remains the default until parity is reached.

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

### Phase 2 — Dangling-reference analysis (core)  ✅ new-syntax fixtures done
Provenance table, death events, use-site check are implemented and cover all 5
new-syntax dangling fixtures (owner moved/reassigned/scope-exited, observer-parameter
ban). Still open: auto + library summaries, return-local-reference (#2), field escape
(#3), iterator invalidation (#4), `string_view`/`span` (#5), `auto`-vs-concrete
provenance (#6), lambda `[&]` escape (#7), dangling temporaries (#8), Eigen summaries,
branch-merge precision. The legacy `borrow_*.clg` (`owned<>/borrowed<>`) fixtures are
superseded by the new syntax and kept only for regression.

### Phase 3 — Use-after-move (coupled to auto-`std::move`)  ✅ done
Moved-from state tracking; reject use of a moved-from value. Implemented in
`dangling_checker.cpp` and reused by the AST-driven generator to justify inserting
`std::move` automatically (Phase 2.5, below).

### Phase 2.5 — AST-driven code generator (new architecture)
Replaces text/regex substitution with a generator that walks the AST directly —
required because C++'s `.`/`->` choice, `this->` field access, and ownership-transfer
points cannot be determined from source text alone (see Chapter 18, Step 6 for the
concrete failures this caused). Opt-in via `--emit-ast`; coexists with the regex
generator until coverage matches. Working: classes/functions/main, if/else,
comparisons, `ref`->reference, `this->`, chained member/method access,
member-target assignment (`a.b = x;`), auto-move, return-value move (object return
type lowered to a smart pointer, owning local moved out). Open: custom includes,
native stream output.

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
