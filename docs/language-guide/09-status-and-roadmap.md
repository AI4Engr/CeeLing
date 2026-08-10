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
- **Auto-derived return-provenance summaries** (CLAUDE.md component A) — the only
  thing a return-provenance summary needs to capture is whether a function's return
  value aliases one of its own parameters (`T f(T a){ return a; }`) or is always
  fresh. Computed once per function/method before the dangling check runs; a call
  result assigned to a variable consults it and registers the right provenance
  (alias → observer of the argument, same machinery as `ref`; fresh → new owner)
  instead of assuming every call returns something brand new. See
  `tests/safety/summary_return_aliases_param.clg` / `summary_return_alias_dangles.clg`.
- **Parameter borrowed/moved summaries** (CLAUDE.md component A's other half) —
  CeeLing bans `ref` function parameters, but that alone does NOT mean every
  object-typed parameter must be consumed: a method/constructor that stores its own
  parameter into an already-shared field (`link = other;`) does not consume that
  parameter, the same way `a.link = b;` at a call site already didn't. Closing this
  gap required `SmartPointerAnalyzer` to infer `shared_ptr` for the parameter (a
  `unique_ptr` genuinely cannot be copied, so "doesn't consume" is only true once the
  parameter's own C++ type actually supports it) AND propagate that inference to
  every call-site argument bound to it — a real three-module change (checker +
  pointer-type inference + emitter), not just a checker-side relaxation. See
  `tests/safety/param_stored_in_field_accepted.clg` (`a.attach(b); b.attach(a);`,
  the single most common object-graph construction pattern) and siblings, and
  Chapter 18's implementation notes for the full design.
- **Single inheritance + `virtual`/`override`** — `class D : public B` (exactly one
  base, `public` explicit), with real vtable dispatch through a base-typed owner
  (`Animal a = new Dog(3); a.speak();` runs Dog's override). The core mechanism is
  **member flattening**: each analysis module's collection pre-pass now visits
  classes base-before-derived and copies a base's members down into each derived
  class, so the ~40 exact-match "does class X have member Y" lookups across four
  modules keep working unchanged. Two things the compiler generates that users
  cannot write — and therefore cannot get wrong — close real holes by construction:
  `virtual ~Base() = default;` for every base (destroying a Derived through a
  `unique_ptr<Base>` is undefined behavior without it), and a `protected` no-arg
  base constructor (CeeLing has no base-initializer-list syntax; a derived
  constructor just assigns inherited fields in its own body). A virtual call from a
  constructor is rejected (CLAUDE.md Phase 2.7's threat — it silently does not
  dispatch to the override). See `tests/safety/inherit_virtual_dispatch_accepted.clg`
  and siblings, and Chapter 18's implementation notes.
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
- **`v[i]` subscript access + iterator/reference invalidation, threat #4**
  (Phase 2.6, slice 2) — new `subscript_expression` grammar (same tight-binding
  operand-restriction technique as `member_access`), and `ref Node n = v[i];` is now
  a real, trackable observer of the **container** (not a specific element — element
  identity doesn't exist independently, and invalidation is a whole-container
  reallocation event). `push_back` is the sole invalidating mutator modeled so far
  (`library_summary.h`'s `vectorMethodInvalidatesReferences`); an invalidating call
  kills every existing observer of its container, a third death-event source
  alongside reassignment and scope-exit, reusing the same `danglingObservers_` set
  and getting branch-merge conservatism for free. See
  `tests/safety/vector_element_observer_dangles_after_push_back_rejected.clg` and
  siblings, and Chapter 18's implementation notes.
- **Container-held observers forbidden by construction** — `std::vector<ref Node>`
  turned out to already be grammatically unparseable (`generic_type`'s type
  argument is `$.type`, and `observer_qualifier` never appears there), but the AST
  *builder* silently swallowed the resulting ERROR node (nested inside
  `generic_type`, not a direct sibling) by capturing the whole subtree's text
  verbatim — a real "misparse, not rejection" gap, not a documentation-only item.
  Fixed with `ts_node_has_error` (a full-subtree check, first use in this codebase)
  plus a targeted diagnostic. See
  `tests/safety/container_held_observer_rejected.clg`.
- **`std::string` as a usable type** — a plain value type, never move/observer
  tracked (unlike `std::vector`/`std::map`). Lowers to `std::string`; a small
  read-only method allow-list deliberately excludes `.c_str()` (extracts a raw
  `const char*` — the same escape-hatch shape as the `.get()` ban, #14). See
  `tests/safety/string_basic_usage_accepted.clg`.
- **`std::map<K,V>` as a usable type** — `generic_type` extended to an optional
  second type argument; `m[key] = value;` (insert-or-update) works via a new
  `subscript_expression`-as-assignment-target grammar path. Keys/values restricted
  to primitive types this slice (sidesteps the element-ownership question vector
  slice 1 needed to solve); the map variable itself participates in move-tracking
  exactly like `std::vector`. See
  `tests/safety/map_declare_and_subscript_accepted.clg` and siblings.
- **`throw`/`catch`, string-only** — `throw` needed no new dangling-analysis
  machinery at all (block-exit was already unconditional, not return-triggered);
  `try`/`catch` reuses the existing branch-merge primitives for a new two-stage
  conservative merge (try-may-throw-anywhere, then try-succeeded-vs-caught).
  Restricted to `string` payloads/catch-types (no exception-class hierarchy without
  inheritance). Caught and fixed a real runtime bug while building the fixture: a
  thrown string literal is C++ `const char*`, which `catch (const std::string&)`
  does NOT match (exact-type matching, no implicit conversion) — would have
  compiled clean but `std::terminate`'d at runtime. See
  `tests/safety/throw_catch_basic_accepted.clg` and siblings.
- **`enum` (unscoped)** — needed zero dangling-analysis changes: an enum type name
  matches none of the "is this an object type" checks, so it's already treated
  like `int`. See `tests/safety/enum_basic_accepted.clg`.
- **`namespace` (desugared to name-qualification)** — `namespace X { class C {...}
  }` is flattened at parse time into a top-level `ClassDeclaration` tagged
  `namespaceName = "X"`; every analysis module dual-registers it under both its
  bare name and its qualified name (`"Node"` and `"YAML::Node"`), the same
  collection-time-flattening instinct Step 20's inheritance work established.
  Nested namespaces are rejected, not supported. See
  `tests/safety/namespace_class_accepted.clg` and siblings.
- **Return-type-generic methods with specializations (`.as<T>()`)** — narrowly
  scoped to the shape yaml-cpp needs: a primary generic `<T> T method() {...}`
  plus concrete specializations `ConcreteType method<ConcreteType>() {...}`,
  called as `n.as<int>()`. Not general C++ templates (see TODO.md's "light
  generics" item for the larger, unscoped feature). The only real risk was a
  grammar-level ambiguity between `x.as<int>()` and an ordinary comparison chain
  at the same expression position, resolved with a `conflicts:` entry; the
  checker/emitter mechanism reuses every module's existing per-method lookup maps
  under a synthetic key (`specializationKey`) instead of adding anything new. See
  `tests/safety/template_as_basic_accepted.clg` and siblings, and Chapter 18 Step
  21 for the full design.
- **Qualified free-function calls** (`std::stoi`, `std::stod`) — a new
  `library_summary.h` allow-list entry for qualified FREE FUNCTIONS
  (distinct from the existing method allow-lists); needed zero
  `DanglingChecker`/`AstEmitter` changes since neither function has any
  object-typed argument or return value. See
  `tests/safety/qualified_call_known_accepted.clg`.
- **`operator[]`/`operator=` overloading** — a class may declare and call
  its own `operator[]`/`operator=`. `n[key] = value;` (assignment THROUGH a
  subscript) is still rejected outright, since that specific shape needs a
  real C++ reference return from `operator[]`, which CeeLing does not
  attempt to support in general — but the aliasing NEED that motivated it
  (yaml-cpp's chained `node["a"]["b"] = "c"`) is covered by the next bullet,
  a narrower, sufficient solution. See
  `tests/safety/operator_subscript_read_accepted.clg`,
  `operator_subscript_two_overloads_accepted.clg` (multiple overloads on one
  class, e.g. `operator[](string)` + `operator[](int)`), and Chapter 18 Step
  22.
- **Alias-returning methods, INFERRED** — a method whose body only ever
  returns `this`, a same-class object-typed field, or a call to another
  already-classified alias-returning method (never `new T(...)`) is
  automatically classified to return `shared_ptr<T>` (aliasing the object
  graph) instead of the default `unique_ptr<T>` (ownership transfer) — no
  annotation, purely structural inference over the method body, matching
  CLAUDE.md's "ownership is inferred, not annotated" rule. This is what
  actually closes the reference-return need above: yaml-cpp's
  `Node::operator[]` returns an alias into the tree by construction (every
  return in its body matches the rule), so `node["a"]` mutating the tree
  falls out for free without a `ref` return type ever existing. `return
  this;` from such a method emits `shared_from_this()` (needs
  `std::enable_shared_from_this<T>`, auto-added as a compiler-generated
  base — never a raw `shared_ptr<T>(this)`, which would create a second,
  independent control block and double-free). See
  `tests/safety/shared_return_alias_accepted.clg`,
  `shared_return_this_accepted.clg`,
  `shared_return_namespaced_class_accepted.clg`, and Chapter 18 Step 23. The
  real `yaml-cling/src-clg/yaml_node.clg` now compiles AND runs correctly
  end-to-end with zero ownership annotations — see
  `yaml-cling/tests/run_node_smoke_rust_backend.sh`.
- **`char` as a value type** — a pure primitive, exactly like `int`/`bool`;
  `string[i]` returns `char`. `char*`/`char[]` are grammatically
  inexpressible by construction (no pointer/array production exists at all),
  closing the classic "char is secretly a raw C-string pointer" door before
  it could open. See `tests/safety/char_basic_accepted.clg` and siblings.
- **A real YAML text parser (`YAML::Load`) for yaml-cling** — a hand-rolled,
  block-style-only tokenizer + recursive-descent parser (no flow style, no
  anchors/aliases, no multi-document, no tags), verified
  **byte-identical against actual upstream yaml-cpp** built and run
  directly on the same representative document. This closes yaml-cling's
  last remaining exclusion ("no parser this round," see the templates/
  operator-overloading bullets above) — real `.yaml` text now parses into a
  `Node` tree, not just hand-built trees. See
  `yaml-cling/src-clg/yaml_scanner.clg`, `yaml_parser.clg`, and
  `yaml-cling/tests/run_parser_smoke_rust_backend.sh`, and Chapter 18 Step
  24 for the four AST-builder bugs and two Parser bugs this surfaced (none
  related to `char` itself — multi-parameter constructors silently dropping
  parameters, parenthesized expressions not parsing at all, call-result
  member access having no valid parse, and a compact-sequence-of-maps
  indent-tracking bug only the yaml-cpp cross-check caught).
- **Real multi-file compilation: `.h` header generation + `.clh` source-level
  includes** — `cling --cpp foo.h foo.clg` emits a declaration-only header
  (signatures, no bodies; templates/specializations excepted — they must stay
  fully defined/`inline` at every instantiation point) instead of a full
  translation unit. `.clg` source spells a local dependency as
  `#include "foo.clh"` (a source-level-only convention — no `.clh` file is
  ever read or written); resolving one parses `foo.clg` itself and merges its
  declarations into the including file's semantic analysis (so `Node`/
  `Scanner`/etc. are no longer "unknown calls"), without merging their
  DEFINITIONS — each `.clg` still transpiles to its own independent `.cpp`,
  compilable with an ordinary `g++ -c` and linkable with the others. This is
  what `yaml-cling`'s `yaml_node.clg`/`yaml_scanner.clg`/`yaml_parser.clg`/
  `main.clg` now do for real: four separately transpiled, separately
  compiled, separately linked translation units — not the source-concatenation
  workaround every prior end-to-end proof (including the parser above) had
  relied on. See `yaml-cling/transpiled/` and Chapter 18 Step 25 for the
  header/out-of-line-definition/ODR mechanics this forced.

**Not yet done:**
- The analyzer still also runs the older **smart-pointer type selection**
  (`unique_ptr`/`shared_ptr`/`weak_ptr`), not yet unified onto the new engine.
- **Library/std/Eigen summaries** — only `std::vector<T>`/`std::string`'s methods
  are on the allow-list; every other external call is still rejected
  unconditionally (the correct conservative default).
- `insert`/`erase`/`resize` as additional invalidating mutators, class-typed map
  keys/values, container-held observers precise tracking (currently just banned),
  map iteration (needs C++ structured-binding support, unshipped),
  `string_view`/Eigen dangling — next slices.

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
construction — no `ref`-qualified field production exists), branch-merge
precision, and `std::vector<T>` iterator/reference invalidation for `push_back`
(#4, Phase 2.6 slice 2 — see "Working today" above). Still open: broader library
summaries, `insert`/`erase`/`resize` invalidation, `string_view`/`span` (#5),
`auto`-vs-concrete provenance (#6), lambda `[&]` escape (#7), dangling temporaries
(#8), Eigen summaries — all gated on language features (`map`/`string`, `auto`,
lambdas) or a library-summary allow-list that don't exist yet (Phase 2.6+). The
legacy `borrow_*.clg` (`owned<>/borrowed<>`) fixtures are superseded by the new
syntax and kept only for regression.

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

Language expressiveness (light/general generics, lambdas, range-for, general
operator overloading, multiple inheritance) is tracked separately in `TODO.md`
— safety is the foundation, expressiveness is the house built on top. `enum`,
`namespace`, the narrow return-type-generic-method slice of templates,
qualified free-function calls, `operator[]`/`operator=` overloading, and
INFERRED alias-returning methods (the actual solution to the
reference-returning-method need — no `ref` return type was built or is
needed) are done (see "Working today" above).
