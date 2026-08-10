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

Phase 2.6 slice 2 (`v[i]` subscript + iterator/reference invalidation, threat #4 —
Ch. 18, Step 14) added 4 more fixtures: **PASS=29, XFAIL=0, FAIL=0.**

- `vector_subscript_read_accepted.clg` — plain `v[i]` read, no `ref` observer
  involved. Proves the new subscript syntax parses, resolves to the right receiver,
  and lowers `.` to `->` when the element is class-typed; compiles clean under g++.
- `vector_element_observer_alive_ok.clg` — `ref Node n = v[0];` used while `v` is
  never mutated afterward. Positive control: an element observer must NOT be
  rejected just because it exists — only actual invalidation should trigger reject.
- `vector_element_observer_dangles_after_push_back_rejected.clg` — the canonical
  invalidation case: `ref Node n = v[0];` then `v.push_back(...)` then use `n`.
  `push_back` may reallocate the vector's backing store, so `n` may be dangling.
- `vector_element_observer_branch_merge_rejected.clg` — the invalidating
  `push_back` happens inside only one `if` branch; a later use must still reject
  (dangling-in-either-branch ⇒ dangling after merge), proving invalidation events
  get the same branch-merge conservatism as every other death event, with no new
  `BranchState` machinery — reuses the existing `danglingObservers_` union.

The container-held-observer ban (Ch. 18, Step 15) added 1 more fixture:
**PASS=30, XFAIL=0, FAIL=0.**

- `container_held_observer_rejected.clg` — `std::vector<ref Node> v;` is tagged
  `//@ expect: reject` for a *parse-level* rejection, not a semantic one: the
  grammar already made this unparseable (`generic_type`'s type argument is
  `$.type`, and `ref` is never reachable from there), but the AST builder was
  silently swallowing the resulting nested ERROR node via a raw `getNodeText`
  capture. Fixed with `ts_node_has_error` (first use in this codebase — a
  full-subtree check, needed because the ERROR node here is nested inside
  `generic_type`, not a direct sibling the way every prior ERROR-detection site
  assumed).

`std::string` support (Ch. 18, Step 16) added 2 more fixtures:
**PASS=32, XFAIL=0, FAIL=0.**

- `string_basic_usage_accepted.clg` — `string` locals, a class field, a
  parameter, concatenation with `+`, and a known-method call (`.length()`), all
  lowering to `std::string` and compiling clean under g++. Also the fixture that
  confirmed a real pre-existing gap: every non-vector field/return-type emission
  site wrote the bare CeeLing type name verbatim instead of routing through
  `cppTypeFor`, so `string` fields never actually compiled before this fix (two
  `examples/*.clg` files had been silently broken this way).
- `string_c_str_rejected.clg` — `.c_str()` is deliberately off the string method
  allow-list (extracts a raw `const char*`, the same escape-hatch shape as the
  `.get()` ban, #14) and hits the default-reject-unknown-calls path.

`std::map<K,V>` support (Ch. 18, Step 17) added 3 more fixtures:
**PASS=35, XFAIL=0, FAIL=0.**

- `map_declare_and_subscript_accepted.clg` — declare `std::map<int,string>`,
  insert via `m[key] = value;`, read back via `m[key]`, compiles clean under g++.
  Also proves `AssignmentStatement::target` (already a general `ExpressionPtr`,
  not `MemberAccess`-specific) needed zero `DanglingChecker` changes for
  subscript-target semantics — only `AstEmitter` needed a new `subscriptTarget`
  case.
- `map_class_value_rejected.clg` — `std::map<int, Node>` (Node a class) — positive
  control proving the primitive-only key/value restriction is enforced, not just
  documented.
- `map_moved_into_call_rejected.clg` — a map variable participates in
  move-tracking exactly like a vector variable (confirmed design decision:
  `isObjectType` checks `isMapType` too), mirroring
  `vector_moved_into_call_rejected.clg`.

`throw`/`catch` support, string-only (Ch. 18, Step 18) added 4 more fixtures:
**PASS=39, XFAIL=0, FAIL=0.**

- `throw_catch_basic_accepted.clg` — the basic usable case. This fixture is
  actually **run**, not just compiled with `-fsyntax-only`, because the bug it
  caught (see below) only manifests at runtime.
- `throw_non_string_rejected.clg` — `throw 42;` — positive control proving the
  string-only restriction is enforced.
- `try_catch_scope_exit_dangles_rejected.clg` — an observer's owner is reassigned
  partway through a try body that then throws; a use after the `try`/`catch` must
  still reject, proving the new two-stage conservative merge (try-may-throw-
  anywhere, then try-succeeded-vs-caught) works, built entirely from existing
  `mergeBranchStates`/`endBlockScope` primitives with no new `BranchState` fields.
- `try_catch_no_throw_path_accepted.clg` — positive control: the try body runs
  normally, catch never executes, code after is fine.

**Runtime bug caught and fixed while building `throw_catch_basic_accepted.clg`**:
a thrown string literal is C++ `const char*`, not `std::string`; `catch (const
std::string&)` does not match a thrown `const char*` (exact-type exception
matching, no implicit conversion) — the naive emission compiled clean and then
`std::terminate()`'d at runtime. `AstEmitter` now force-wraps thrown string
literals in `std::string(...)`. This is why the accept fixture is run, not just
compiled — `-fsyntax-only` cannot catch a matching failure that only manifests
when the exception is actually thrown.

Parameter borrowed/moved summaries — `a.attach(b)` shouldn't consume `b` (Ch. 18,
Step 19) added 5 more fixtures: **PASS=44, XFAIL=0, FAIL=0.**

- `param_stored_in_field_accepted.clg` — the end-to-end proof:
  `a.attach(b); b.attach(a);` compiles AND is actually **run** (not just
  `-fsyntax-only`-checked), printing `2` with no crash. Confirms all three
  modules (`DanglingChecker`, `SmartPointerAnalyzer`, `AstEmitter`) agree at
  runtime, not just that each one's isolated logic looks correct in isolation —
  the same discipline the `throw`/`catch` runtime bug (Step 18) established.
- `param_stored_in_field_self_body_accepted.clg` — the prerequisite-bug fixture
  in its narrowest form: inside `attach`'s own body, using `other` right after
  `link = other;` must NOT be rejected. Before the fix, a bare-identifier
  assignment target with implicit `this` was treated as an ordinary local
  reassignment (no `classFields_`-equivalent existed to recognize it as a
  field-store), so `other` was marked moved-from immediately — with no caller or
  summary logic involved at all.
- `param_stored_in_field_constructor_accepted.clg` — constructor bodies get the
  same parameter-storage promotion method bodies do
  (`SmartPointerAnalyzer::promoteParamsStoredInSharedFields` walks both). Also
  the fixture that surfaced a genuine, pre-existing, UNRELATED bug (confirmed via
  `git stash` with none of this step's changes applied): a constructor with more
  than one parameter silently drops every parameter after the first from the
  emitted C++ signature. Worked around by using a single-parameter constructor
  rather than fixing that separate bug.
- `param_not_stored_still_moves_rejected.clg` — positive control: a method that
  does NOT store its parameter (ordinary use-and-discard) still consumes its
  argument normally. Proves the promotion doesn't over-correct into "nothing ever
  moves."
- `param_conditional_storage_still_moves_rejected.clg` — storage behind a
  conditional (`if (x) { link = other; }`) is deliberately NOT detected this
  slice (`promoteParamsStoredInSharedFields` only walks a body's TOP-LEVEL
  statements) — `other` stays `unique_ptr` and still moves, the existing safe
  default, documented honestly as a real boundary rather than silently.

**A real regression caught and fixed during this step's own verification sweep**
(not anticipated in the plan): generalizing "a SHARED argument never moves" to
every call, not just `push_back`, initially broke
`vector_moved_into_call_rejected.clg` (PASS → XFAIL) — `SmartPointerAnalyzer::
getPointerType`'s not-found fallback defaults to SHARED, and a container type
like `std::vector<Node>` has no `pointerDecisions` entry at all, so the new rule
incorrectly treated every vector argument as already-shared. Fixed by restricting
the blanket rule to arguments whose declared type is a genuine registered class
name. Full fixture sweep re-run afterward confirmed PASS=44, XFAIL=0 restored.

**CTest note**: `cling_test_bidirectional_analysis_report`'s NEEDLE3 assertion
changed from `std::move(second)` to `std::shared_ptr<Node> tail = second` — the
old assertion was pinning down a real, if minor, pre-existing bug (`second` is a
SHARED local; unnecessarily `std::move`-ing it into `tail` nulled out `second`
even though nothing downstream needed it moved). The new behavior is more
correct, not a regression.

Single inheritance + `virtual`/`override` (Ch. 18, Step 20) added 6 more fixtures:
**PASS=50, XFAIL=0, FAIL=0.**

- `inherit_base_field_and_method_accepted.clg` — a derived class uses an inherited
  field and calls an inherited method. Proves the base-before-derived member
  FLATTENING works in both the safe-failing case (`EscapeHatchChecker::
  isKnownCallable` would otherwise reject a valid inherited call) and the
  silently-wrong one (`AstEmitter::fieldType` would otherwise return "" for an
  inherited field, emitting `.` instead of `->` on a smart pointer). Run; prints 7.
- `inherit_virtual_dispatch_accepted.clg` — `Animal a = new Dog(3); a.speak();`
  **runs** and prints `woof`, not `generic`. Dispatch is a runtime property:
  compile-only checking would happily accept code that calls the base version,
  which is precisely the bug this feature exists to prevent. Also exercises
  `factoryFor`'s declared-vs-constructed type split (`unique_ptr<Animal>` holding a
  `make_unique<Dog>`).
- `inherit_virtual_destructor_accepted.clg` — a derived object owned through a
  base-typed variable. Both **run** (prints 16) and grepped for
  `virtual ~Shape() = default;` in the emitted output, since the safety claim is
  about what was generated. Also exercises the auto-generated protected no-arg base
  constructor (CeeLing has no base-initializer-list syntax).
- `inherit_unknown_base_rejected.clg` — inheriting from an undeclared class.
  Targeted parse-time diagnostic, not a generic parse error.
- `inherit_cycle_rejected.clg` — `A : B`, `B : A`. A correctness prerequisite, not
  politeness: a cycle has no valid base-before-derived order for the flattening
  pass to use.
- `inherit_virtual_call_in_constructor_rejected.clg` — CLAUDE.md Phase 2.7's
  virtual-call-from-constructor threat, landed on day one of inheritance support as
  that entry prescribed. Only constructors are checked because CeeLing has no
  destructor syntax, so the other half of the classic trap cannot be written.

**Verified additive**: a non-inheriting program's emitted C++ contains zero
occurrences of `virtual` or `: public` — existing single-class programs are
completely unaffected by this step.

## Compile gate (what PASS actually means now)

Until recently, `PASS` on an accept fixture meant only **"`cling` exited 0"** — i.e.
the *analysis* accepted the program. It said nothing about whether the C++ the
emitter produced would even compile. Those are different claims, and the gap between
them is where the `throw "literal"` bug lived (it compiled cleanly and then
`std::terminate`'d at runtime).

`run_safety_tests.sh` now enforces two tiers on every `//@ expect: accept` fixture:

1. **Compile** — the generated C++ must build under a real compiler
   (`g++ -std=c++17 -fsyntax-only`, overridable via `$CXX`).
2. **Run** — a fixture may declare `//@ run: <expected stdout>`, in which case it is
   compiled, executed, and its output compared. Use this tier whenever the claim is a
   RUNTIME property, because `-fsyntax-only` cannot distinguish "dispatches to the
   derived override" from "silently calls the base version". Currently tagged:

   | Fixture | Asserted output | Runtime claim being pinned |
   |---|---|---|
   | `inherit_virtual_dispatch_accepted` | `woof` | virtual dispatch reaches the override |
   | `inherit_virtual_destructor_accepted` | `16` | destruction through a base-typed owner is well-defined |
   | `inherit_base_field_and_method_accepted` | `7` | inherited field/method actually resolve |
   | `throw_catch_basic_accepted` | `bad thing` | thrown type matches the catch clause |
   | `param_stored_in_field_accepted` | `2` | a shared argument survives the call |

The runner prints `Compile gate: ON/OFF`, so a missing compiler downgrades **loudly**
rather than silently weakening every PASS in the table.

**The gate was validated by breaking it on purpose**: deleting the `virtual` keyword
from `AstEmitter` turned the suite red (`FAIL=2`, `RESULT: FAIL`). Under the previous
runner those same two fixtures still reported `PASS`, because `cling` itself still
exited 0 — which is precisely the blind spot this closes. A gate that never fails
proves nothing, so this check should be repeated if the runner is ever reworked.

`enum` support added 1 more fixture: **PASS=51, XFAIL=0, FAIL=0.**

- `enum_basic_accepted.clg` — unscoped `enum Kind { Null, Scalar, Sequence };`,
  used both to construct a value and read back via implicit int conversion. Run,
  not just compiled — this fixture's first draft asserted the wrong expected value
  (`2` instead of the correct `1` for `Scalar`), caught immediately by the run tier
  rather than shipping a silently-wrong test.

`namespace` support (desugared to name-qualification at parse time) added 4 more
fixtures: **PASS=55, XFAIL=0, FAIL=0.**

- `namespace_class_accepted.clg` — a class declared inside `namespace X { ... }`,
  constructed and used via `X::Class`. Run.
- `namespace_enum_qualified_accepted.clg` — the real yaml-cling shape:
  `namespace NodeType { enum value {...} }` alongside `namespace YAML { class Node
  {...} }`, with `NodeType::Scalar` used both to construct a `YAML::Node` and
  inside a method-body comparison. Run, prints `2 1`.
- `namespace_nested_rejected.clg` — a `namespace` declared inside another
  `namespace` is rejected with a targeted diagnostic; nesting was never supported,
  only top-level namespaces desugar.
- `namespace_moved_into_call_rejected.clg` — a namespaced-type local participates
  in ordinary move-tracking exactly like a non-namespaced one. This is also the
  fixture that caught a real bug: `SmartPointerAnalyzer::getPointerType`'s
  suffix-match fallback silently defaults to `shared_ptr` when no decision is
  found, and an emitter that stripped the `X::` qualifier before querying (instead
  of passing the class name through verbatim, matching how registration actually
  spells it) looked up the wrong key and hit that fallback — a namespaced local
  was silently getting `shared_ptr` instead of the correctly-inferred `unique_ptr`.

Return-type-generic methods with specializations (`.as<T>()`) added 3 more
fixtures: **PASS=58, XFAIL=0, FAIL=0.**

- `template_as_basic_accepted.clg` — the real `yaml_node.clg` shape: a primary
  generic `<T> T as() {...}` (never actually invoked — every call site must carry
  an explicit type argument) plus `int`/`string` specializations. Run, prints
  `42 converted` — the definitive proof real C++ template resolution dispatches to
  the right specialization body at each call site, not just that the emitted code
  compiles.
- `template_ordinary_method_unaffected_accepted.clg` — a plain, non-generic,
  non-specialized method is completely unaffected by the new optional grammar
  productions. Run.
- `template_comparison_unaffected_accepted.clg` — the specific regression risk the
  feature's plan flagged as highest-uncertainty: `x.as<int>()` and an ordinary
  comparison (`a < b`, `x.length() < 5`) are both grammatically legal at the same
  `$.expression` position after `identifier '<'` / `member_access '<'`. Resolved
  with a `conflicts:` entry (GLR explores both, keeps whichever completes) rather
  than a numeric `prec()`, the same shape of fix `container_declaration` vs
  `variable_declaration` used. Run, prints `1 0` — both comparisons still evaluate
  correctly, not just parse.

**Mechanism note**: all three of `enum`/`namespace`/templates reused patterns
already established for inheritance rather than inventing new ones — `enum`
needed zero analysis changes (an enum type name matches none of `classNames_`/
`isVectorType`/`isMapType`, so it's already treated like `int`); `namespace`
reused the "flatten at collection time via dual-registration, leave ~40
downstream lookups unchanged" pattern across all four analysis modules; templates
reused the identical per-method map structure via a synthetic lookup key
(`specializationKey`, `class_hierarchy.h`) instead of adding new data structures
anywhere.

Qualified free-function calls (`std::stoi`/`std::stod`) added 2 more fixtures:
**PASS=60, XFAIL=0, FAIL=0.**

- `qualified_call_known_accepted.clg` — `std::stoi`/`std::stod` on the new
  library allow-list (`library_summary.h`'s `isKnownQualifiedFreeFunction`).
  Run, prints `42 3.5`.
- `qualified_call_unknown_rejected.clg` — positive control: an unregistered
  qualified free function still hits the default-reject-unknown-calls path —
  adding two names to the allow-list is not a blanket "any `std::` call is
  fine" hole.

Value-returning `operator[]`/`operator=` overloading added 5 more fixtures:
**PASS=65, XFAIL=0, FAIL=0.**

- `operator_subscript_read_accepted.clg` — a class's own `operator[]` called
  via `n[key]` in read position, routed through as an ordinary method call
  (`n->operator[](key)`) since `n` is a smart pointer with no `operator[]` of
  its own. Run, prints `42`.
- `operator_assign_value_accepted.clg` — `n = "text";` on a class-typed `n`
  whose class declares `operator=(string)`: a MUTATING call
  (`n->operator=(...)`), not ownership reassignment — `n` is read again
  right after via `.get()`, proving it's still the same live object. Run,
  prints `99`.
- `operator_subscript_write_rejected.clg` — positive control: `n[key] =
  value;` on a class-typed receiver is rejected even though `operator[]`
  itself exists and works in read position — real C++ needs a
  REFERENCE-returning `operator[]` for the subsequent `=` to assign through,
  and CeeLing has no `ref` return type yet. Rejected with a diagnostic
  naming that limitation, not silently mis-emitted.
- `operator_subscript_container_unaffected_accepted.clg` — regression proof:
  ordinary `std::vector`/`std::map` subscript (read AND write) coexists with
  a class's own `operator[]` in the same program with no cross-contamination
  between the container and class-typed dispatch branches. Run.
- `operator_assign_field_store_unaffected_accepted.clg` — **a real bug found
  while re-testing the actual yaml-cling `yaml_node.clg`** against this
  feature, not caught by the fixtures written before that re-test: a
  bare-identifier FIELD store (`child = nullptr;`, implicit `this`, inside a
  constructor) on a field whose class happens to declare `operator=` was
  wrongly routed through the new operator= path
  (`this->child->operator=(nullptr);`, which does not compile — `unique_ptr`
  has no such overload) instead of an ordinary field store
  (`this->child = nullptr;`). Fixed by excluding `isCurrentField` targets
  from `AstEmitter`'s operator= detection; `DanglingChecker` was already safe
  here since its equivalent `isBareFieldStore` check runs first. Run, prints
  `1 0`.

**Re-tested against the actual `yaml-cling/src-clg/yaml_node.clg`** after both
slices landed: it now parsed and reached codegen (previously failed at
`std::stoi` before either slice existed), but at that point still failed to
COMPILE — stopped exactly at the reference-return shape (`return
this->child;`/`return this;` from a method declared to return `Node`, lowered
to `unique_ptr<Node>`, which is move-only and cannot be returned as a copy).
This was the honest stopping point at the time — closed by the alias-return
inference work below, landed in the same increment as the port's completion.

Alias-returning-method INFERENCE (Ch. 18 Step 23) added 5 more fixtures:
**PASS=70, XFAIL=0, FAIL=0.**

A method whose body ONLY ever returns `this`, a same-class object-typed
field, or a call to another already-classified alias-returning method (never
`new T(...)`) is automatically classified to return `shared_ptr<T>` instead
of the default `unique_ptr<T>` — purely structural inference over the method
body, no annotation (a `//@ptr shared Class.method` hint was prototyped
first and explicitly rejected: CLAUDE.md's "ownership is inferred, not
annotated" rule applies to method returns exactly as it does to fields).
This closes the reference-return need noted above WITHOUT a `ref` return
type ever being built — yaml-cpp's `operator[]` returning an alias into the
tree falls out of the existing smart-pointer-kind machinery once extended to
methods.

- `shared_return_alias_accepted.clg` — a method returning a same-class field
  is inferred `shared_ptr`; mutating the result mutates the field itself.
  Run, prints `77`.
- `shared_return_this_accepted.clg` — `return this;` is inferred `shared_ptr`
  and emits `shared_from_this()` (needs `std::enable_shared_from_this<T>`,
  auto-added), NOT a raw `shared_ptr<T>(this)` — confirmed via direct probe
  the latter compiles but creates a SECOND, independent control block (a
  genuine double-free). Run, prints `1`.
- `shared_return_inference_unaffected_accepted.clg` — regression proof: a
  class with an alias-returning method ALSO declares an ordinary
  `new`-returning method; the ordinary one stays `unique_ptr`, completely
  unaffected by its sibling's inferred classification. Run, prints `5 9`.
- `shared_return_namespaced_class_accepted.clg` — **a real bug found while
  re-testing the actual yaml_node.clg**: the inference pass wrote its
  findings under a class's bare name only (`"Node"`), never the
  namespace-qualified one (`"YAML::Node"`) a namespaced local's declared
  type actually uses, so `root["name"]` silently kept emitting `unique_ptr`
  and failed to compile. Fixed with the same dual-registration every other
  collection pass in this codebase already needed for namespaced classes.
  Run, prints `5`.
- `operator_subscript_two_overloads_accepted.clg` — the real yaml-cpp shape
  (`operator[](string)` + `operator[](int)` on one class) no longer collide
  in `classMethodParams_`'s move/pointer-kind bookkeeping (a real,
  confirmed-but-not-yaml_node.clg-triggering bug: real C++ overload
  resolution still picked the correct body regardless, but the checkers'
  own internal state was corrupted for any overload taking an object-typed
  argument). Fixed with `overloadKey(methodName, paramCount)`
  (`class_hierarchy.h`). Run, prints `10 15`.

**A second real bug found and fixed** during the same re-test, before the
namespaced-class one above: a bare-identifier FIELD store (`child =
nullptr;`, implicit `this`, inside `Node`'s own constructor) was wrongly
routed through the alias-return machinery instead of an ordinary field
store. Same root cause and same fix shape as `operator_assign_field_store_
unaffected_accepted.clg` above (excluding `isCurrentField` targets), applied
this time to `isSharedReturningCall`'s local-declaration companion check
rather than the `operator=` detection.

**The real end-to-end proof**: `yaml-cling/src-clg/yaml_node.clg`, entirely
UNCHANGED (zero ownership annotations, matching its own header comment "NO
ownership annotations — inference decides"), now both COMPILES and RUNS
correctly. A driver program builds a small map and sequence by hand, reads
them back through `.as<string>()`/`.as<int>()`/`operator[]`/`size()`, and
prints correct output — verified via
`yaml-cling/tests/run_node_smoke_rust_backend.sh` against
`expected_node_smoke_rust_backend.txt`. One honestly-stated remaining
limitation surfaced while writing the driver (not a `yaml_node.clg`-specific
gap): a method call chained directly onto a subscript/call result
(`root["name"].Scalar()`) is rejected, since every checker does lightweight,
non-full-type-inference resolution — worked around with an intermediate
variable at each chaining point, tracked as `yaml-cling/notes/GAPS.md`'s RB5.

`char` as a value type, and a real YAML text parser (Ch. 18 Step 24) added
12 more fixtures: **PASS=82, XFAIL=0, FAIL=0.**

`char` was added as a prerequisite for a hand-rolled tokenizer — a pure
VALUE type like `int`/`bool`, with `char*`/`char[]` grammatically
inexpressible by construction (no pointer/array production exists anywhere
in this grammar), closing the classic "char is secretly a raw C-string
pointer" door before it could open (an explicit constraint from the user,
not an afterthought).

- `char_basic_accepted.clg` — declaration, a char literal, comparison. Run,
  prints `x 0`.
- `char_string_subscript_accepted.clg` — `string[i]` returns `char`, exercised
  in a tokenizer-shaped while-loop counting spaces. Needed ZERO emitter
  changes for the read side (the existing generic `SubscriptExpression`
  fallback already emitted `receiver[index]` literally, which happens to be
  exactly correct for `std::string::operator[]` too). Run, prints `1`.
- `char_class_field_method_accepted.clg` — `char` as a method return type
  and a class field access pattern (a Scanner-shaped `peek()`/`advance()`
  pair). Run, prints `xy`.
- `char_escape_accepted.clg` — `\n`/`\t` escape sequences, added only once a
  real consumer (the scanner's own newline detection) needed one. Run,
  prints `1 0`.
- `char_pointer_rejected.clg` — positive control: `char*` stays a parse
  error, the same way every other raw pointer type already is.
- `multi_param_constructor_and_method_accepted.clg` — a real, significant,
  previously-undiscovered bug: `buildConstructorDeclaration`/
  `buildMethodDeclaration` both silently dropped every constructor/method
  parameter after the first (a fixed `j += 2` stride over `parameter_list`,
  correct only for the FIRST parameter — every one after it has a leading
  `,` sibling, making it 3 children wide, not 2). `Token(int k, string v)`
  compiled a constructor taking only `k`. Fixed by pairing type/identifier
  by TYPE (walking every child) instead of a fixed stride. Run, prints
  `1 a 5 9`.
- `parenthesized_expression_accepted.clg` — another real, significant bug:
  `primary_expression`'s grouped form (`'(' expr ')'`) has three children,
  but `buildExpression` unconditionally used `child(0)` (the literal `(`
  token). `bool b = (a);` and `a && (b || c)` both failed to parse entirely
  — confirmed via `tree-sitter parse` directly that the GRAMMAR already
  handled the shape correctly, purely an AST-builder bug. Fixed with
  `ts_node_named_child(node, 0)`. Run, prints `1`.
- `call_result_member_access_accepted.clg` — `someCall().field` had no
  valid parse at all (`member_access`'s left-operand choices never included
  `function_call`; `tree-sitter parse` confirmed `.field` became a bare
  ERROR-node sibling, silently dropped by error recovery). Fixed by adding
  `function_call` to `member_access`'s choices, plus new
  `functionReturnType_`/`classMethodReturnType_` maps so `exprObjectClass`
  can resolve a call RESULT's class (no variable to look up). Covers both
  explicit-receiver (`h.get().val`) and implicit-`this` (`current().val`)
  shapes. Run, prints `5 1`.
- `container_field_via_member_access_accepted.clg` — `containerVarType`
  only ever checked a bare-identifier receiver (a local/param, or the
  CURRENT class's own implicit-`this` field); an explicit `member.field`
  receiver (`scanner.out[pos]`) had no case, emitting `.kind` instead of
  `->kind`. Fixed with a `MemberAccess` branch resolving the receiver's
  class first. Run, prints `5`.
- `shared_return_via_promoted_local_accepted.clg` — `inferAliasReturningMethods`'s
  structural rule never recognized `return <local>;` at all; a method
  returning a local that gets promoted to `shared_ptr` by a LATER pass
  (`promoteSharedCallArguments`) kept its own return type at `unique_ptr`.
  Fixed by a new `promoteReturnsOfSharedLocals` pass, running genuinely LAST
  in `analyzeProgram`. Run, prints `2`.
- `shared_return_free_function_namespaced_accepted.clg` — the free-function
  counterpart (`inferAliasReturningFreeFunctions`, which must run even
  LATER than `promoteReturnsOfSharedLocals` — confirmed via probe that
  running it too early, as originally attempted, queried a classification
  that did not exist yet), plus `AstEmitter::emitFunction`'s free-function
  call site never passing the `methodName` its own shared-check reads, plus
  a namespaced call site needing the classification dual-registered under
  the qualified name. Run, prints `9`.

**The definitive end-to-end proof**: real upstream yaml-cpp
(`C:\project\yaml-cpp`, built and run directly on this machine from its own
actual source, not simulated) and yaml-cling's `rust_backend` port
(`yaml_scanner.clg` + `yaml_parser.clg` + `YAML::Load`) produce
**byte-identical output** parsing the same representative YAML document
(nested maps, a sequence, a multi-key compact sequence-of-maps entry, a `#`
comment, quoted scalars, a top-level key after deep nesting). This
cross-check caught two real scanner/parser bugs that no fixture written
beforehand had been shaped to catch — a compact sequence-of-maps entry
tagging its tokens with the dash's column instead of the key's own column
(so a second key on a continuation line silently vanished), and the
map-parsing loop treating ANY `TkDedent` as "this scope is over" even when
its target indent was still above the current scope (so a top-level key
after deeply nested content silently vanished). See
`yaml-cling/tests/run_parser_smoke_rust_backend.sh` and Chapter 18 Step 24
for the full account, including why the cross-check technique itself (not
just its result) is worth recording for future parser work.
