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
