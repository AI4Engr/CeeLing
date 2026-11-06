# Plan: Eigen view/borrow tracking (replace the exploratory allow-list)

> **STATUS: v1 IMPLEMENTED.** See the internal notes' "Eigen view/borrow tracking —
> v1 IMPLEMENTED" section for the final outcome, what changed, and the
> verification record. This file is kept as-is for its design rationale
> (especially the "why Eigen is different" section below) — the steps below
> describe what was planned; the internal notes describes what actually landed. No
> steps were skipped; Step 3b's flagged risk (receiver-vs-argument
> distinction) was real and is exactly what got resolved during
> implementation, documented in the internal notes.

## Status before this plan (verified empirically, not assumed)

Move/copy tracking for third-party value types (Eigen::MatrixXd) is **already
fixed and shipped** (see the internal notes' "`struct`/value-type move tracking — FIXED"
section). Confirmed by direct probe, all passing:

- `sink(a); use(a);` on `Eigen::MatrixXd` → correctly REJECTED (use-after-move)
- `Foo b = a; Foo c = a;` (declaration copy) → correctly ACCEPTED, compiles, runs
- Passing the same Eigen value to a function twice (`use(a); use(a);`) →
  correctly REJECTED (2nd use)
- A parameter moved inside a function body, then used again → correctly
  REJECTED
- `Eigen::MatrixXd b = passthrough(a);` then using `b` → correctly ACCEPTED,
  compiles, runs, correct output
- Same, but using `a` afterward instead → correctly REJECTED

**What is NOT yet done, and is the entire subject of this plan**: Eigen
methods that return a VIEW aliasing their receiver (`.block()`, `.row()`,
`.col()`, `.transpose()`, `.segment()`, ...) are currently **rejected
outright** with `error[semantic/forbidden-construct]: call to unknown
function/method 'block'` — confirmed by direct probe:
```
Eigen::MatrixXd a = Eigen::MatrixXd::Identity(3, 3);
Eigen::MatrixXd b = a.block(0, 0, 2, 2);
```
This is IMPORTANT: the call is rejected by `EscapeHatchChecker`'s
default-reject-unknown-call path, NOT a parse error. The grammar already
accepts `a.block(0, 0, 2, 2)` as an ordinary member-call AST node — nothing
needs to change in `tree-sitter-ceeling`. The gap is purely "CeeLing has no
summary telling it what `.block()` does," which is exactly what this plan
adds.

## Why Eigen is a genuinely different shape from CeeLing's own classes

This section is the "write the Eigen specialness into the docs" part —
read this before touching any code, since it explains why the existing
`resolveCallReturnAlias`/`FunctionSummary` machinery (built for CeeLing's
own classes) does not directly apply.

1. **Eigen is never parsed by CeeLing.** `Eigen::MatrixXd` has no
   `ClassDeclaration` anywhere in the AST — it arrives via a real `#include
   <Eigen/Dense>` system header, which `ASTBuilder`/tree-sitter never looks
   inside. Every mechanism in `DanglingChecker` that resolves a class's
   methods (`classMethodParams_`, `summaries_`, keyed by
   `summaryKey(className, methodName)`) is populated ONLY by walking a real
   `ClassDeclaration`'s members (`deriveSummaries`). There is no such walk
   for Eigen, and cannot be — nothing to walk. Confirmed by reading
   `resolveCallReturnAlias` (`dangling_checker.cpp:368-417`): the
   member-call branch requires `classMethodParams_.find(typeIt->second)` to
   succeed, which will NEVER be true for `Eigen::MatrixXd` because no
   `ClassDeclaration` for it ever populates that map. **This is the reason
   Eigen's view-borrowing needs a parallel mechanism, not just "add Eigen's
   ClassDeclaration somehow."**

2. **Eigen's own declarations are unparseable by any reasonable scanner** —
   confirmed by reading the real source (`eigen/Eigen/src/plugins/
   BlockMethods.inc`), not assumed:
   ```cpp
   EIGEN_DEVICE_FUNC constexpr EIGEN_STRONG_INLINE typename FixedBlockXpr<NRows,NCols>::Type
   block(Index startRow, Index startCol, NRowsType blockRows, NColsType blockCols) const {
   ```
   Macros (`EIGEN_DEVICE_FUNC`, `EIGEN_STRONG_INLINE`), a dependent return
   type (`typename FixedBlockXpr<NRows,NCols>::Type`), and the declaration
   itself lives in a `.inc` file `#include`d into the *middle* of a class
   body defined via CRTP (`MatrixBase<Derived>`), 4-5 template layers deep.
   `Eigen::MatrixXd` itself is not even a class — it's `typedef
   Matrix<double,-1,-1,0,-1,-1> MatrixXd`. **Do not attempt to write a
   signature parser for this.** It is a fundamentally different, much
   harder problem than CeeLing's own flat, hand-written class bodies.

3. **But the borrow fact is legible from the RETURN TYPE NAME alone**,
   without parsing the signature that produces it — confirmed directly:
   ```cpp
   // eigen/Eigen/src/plugins/BlockMethods.inc:1080
   ColXpr col(Index i) { return ColXpr(derived(), i); }
   // eigen/Eigen/src/plugins/BlockMethods.inc:15
   using ColXpr = Block<Derived, internal::traits<Derived>::RowsAtCompileTime, 1, !IsRowMajor>;
   // eigen/Eigen/src/Core/Block.h (Block's own constructor)
   Block(XprType& xpr, Index i) : Impl(xpr, i) { ... }   // takes a REFERENCE and stores it
   ```
   `col()` returns `ColXpr`, which is a `Block<...>` alias, and `Block`'s
   constructor takes `XprType&` and stores it — that reference-storing IS
   the borrow. This pattern is uniform across Eigen's whole expression-
   template surface: `Block`, `Transpose`, `Ref`, `Map`, `Diagonal`,
   `Reverse`, `CwiseBinaryOp`, `CwiseUnaryOp`, `Product`, `Replicate`,
   `Select` are all "holds a reference/pointer to its operand(s), does not
   own storage." Whereas `Matrix`/`Array`/`PlainObjectBase` own real
   storage. **The class name families are mechanically listable** —
   confirmed via `ls eigen/Eigen/src/Core/*.h`, no manual signature reading
   needed:
   ```
   Borrowing (view) family:  Block Transpose Ref Map Diagonal Reverse
                             CwiseBinaryOp CwiseUnaryOp Product Replicate Select
   Owning (plain-object) family: Matrix Array PlainObjectBase
   ```
   So the rule this plan implements is a **type-name classification**, not
   a signature parse: if a method's return type name matches the borrowing
   family, the call result is an observer of the receiver; if it matches the
   owning family (or the destination the user declares is a plain-object
   type — see point 4), it's a fresh owned value.

4. **CeeLing has no `auto` — this is what makes classification safe and
   sufficient, unlike in real C++.** Confirmed: `tree-sitter-ceeling/
   grammar.js`'s `variable_declaration` requires an explicit `$.type`; there
   is no type-inference production anywhere in the grammar. This matters
   because Eigen's actual danger in normal C++ is:
   ```cpp
   auto e = A + B;   // e is a lazy CwiseBinaryOp, borrows A and B -- dangling risk
   ```
   CeeLing structurally cannot express this — the user must always write
   `Eigen::MatrixXd e = A + B;`, and a `Eigen::MatrixXd`-typed destination
   FORCES evaluation into an owned copy regardless of what lazy expression
   type the RHS would otherwise produce in real C++. This is the design notes'
   threat #6 (`auto`-vs-concrete-type provenance) — already closed by
   construction, not something this plan needs to solve. What THIS plan
   handles is the narrower remaining case: a user explicitly writing a VIEW
   TYPE as the destination (`Eigen::MatrixXd::ColXpr b = a.col(0);`, if/when
   that's even writable — see Step 0) or, more commonly for v1, calling a
   borrowing method and immediately using the result without an intervening
   named view variable.

5. **Zero runtime/codegen cost.** This is a compile-time-only
   classification feeding `DanglingChecker`'s existing `observerOf_` table.
   `AstEmitter` needs NO changes — a call to `a.block(...)` already emits
   as an ordinary method call today (confirmed: emission is generic
   `FunctionCall` handling, gated only by whether `EscapeHatchChecker`
   accepted the call). Eigen's expression templates keep their normal
   zero-copy lazy-evaluation behavior in the generated C++; CeeLing is only
   adding compile-time knowledge of what already happens at runtime.

## Scope decision for v1 (keep this narrow)

- **In scope**: `receiver.method(args...) -> borrows self` shape only —
  covers `.block()`, `.row()`, `.col()`, `.transpose()`, `.segment()`,
  `.head()`, `.tail()`, `.diagonal()`. This is the single most common and
  highest-value Eigen pattern.
- **Out of scope for v1** (do not attempt): multi-operand borrow expressions
  (`operator+(a, b)` borrowing both `a` and `b` — needed for the `auto e =
  A+B` case, which is moot per point 4 above since CeeLing has no `auto`);
  a general per-library declaration file format (defer to the separately
  planned Part B mechanism in the internal notes if/when a second library beyond Eigen
  needs this); nested/chained views (`a.block(...).transpose()`) — reject
  these for now rather than trying to compose provenance through a chain.
- **Non-goal**: do not try to make CeeLing understand Eigen's *storage*
  semantics (row-major vs column-major, aliasing between overlapping
  blocks, etc.) — this is purely about "does this call's result observe the
  receiver," the same question CeeLing already answers for its own
  `ref`/ownership model.

## Step 0 — verify the exact call shapes actually parse (spend 30 min here first)

Before writing any classification logic, confirm what CeeLing's grammar can
already express for Eigen usage, since some of this plan is worthless if the
call shape itself can't be written. Already confirmed:
- `a.block(0, 0, 2, 2)` parses as an ordinary member-call AST node (verified
  — reaches `EscapeHatchChecker`'s unknown-call rejection, not a parse
  error).
- `Eigen::MatrixXd::Identity(rows, cols)` (qualified static factory) already
  works (existing spike).

Still to verify empirically before Step 2 (each is a 5-minute probe, do NOT
skip — a wrong assumption here wastes the rest of the plan):
- `Eigen::MatrixXd b = a.block(0, 0, 2, 2);` — does the FULL declaration
  (not just the bare call) parse and reach `EscapeHatchChecker`? (Expect
  yes, but confirm — the earlier probe only tested the call in isolation... 
  actually it WAS the full declaration form; re-check the exact probe used
  and note the result here once repeated.)
- `a.row(0)`, `a.col(0)`, `a.transpose()` — same shape, zero-arg or
  single-int-arg member calls; should parse identically to `.block()` but
  confirm none of them collide with an existing CeeLing keyword or grammar
  production (`row`/`col` are unlikely to, but check).
- Whether the RECEIVER of the borrowing call can be resolved by
  `EscapeHatchChecker::resolveDeclaredType`/`DanglingChecker`'s
  `localType_` the same way an ordinary local is — i.e. `a` in `a.block(...)`
  must resolve to declared type `Eigen::MatrixXd` through the existing
  machinery so the classification step (Step 2) has something to key off.

If any of these fail to parse or resolve, STOP and report back rather than
working around it with a grammar change — a grammar change is a much bigger
decision than this plan currently scopes for.

## Step 1 — declare the borrowing-method allow-list (data, not parsing)

Add to `library_summary.h`/`.cpp` (same file the exploratory
`isKnownExternalClassType`/`isKnownExternalOperator` allow-list already
lives in — extend it, do not create a parallel mechanism):

```cpp
// Eigen methods known to return a VIEW that borrows `self` (the receiver),
// NOT a fresh owned value -- extracted by reading Eigen's own return-type
// naming convention (ColXpr/RowXpr/Block, etc. all alias Eigen's Block/
// Transpose/... expression-template classes, which store a reference to
// their source -- see docs/design/eigen-views.md for the full justification).
// className -> set of method names that borrow self.
bool isKnownBorrowingMethod(const std::string& className, const std::string& methodName);
```

Implementation: a `std::set<std::string>` of `"Eigen::MatrixXd::block"`-style
keys (mirror the existing `isKnownExternalOperator`'s `type + "|" + op` key
style, e.g. `type + "::" + methodName`), seeded with: `block`, `row`, `col`,
`transpose`, `segment`, `head`, `tail`, `diagonal`. Comment this list as
"NOT exhaustive — extend by consulting Eigen's own Block/Transpose/Ref/Map/
Diagonal/Reverse family names, never guess."

This is intentionally still a hand-maintained set for v1 (not yet the fully
general "classify by scanning Eigen's own headers" mechanism) — the
scanning automation is a follow-up once the tracking mechanism itself is
proven, per this project's established discipline of landing the narrow
mechanism first and generalizing after it's verified working end-to-end.
Do NOT skip straight to auto-scanning Eigen's headers for this v1 pass.

## Step 2 — `EscapeHatchChecker`: accept the call

`EscapeHatchChecker::isKnownCallable` (`escape_hatch_checker.cpp:216-287`)
currently has no path for "member call on a known-external class type."
Add a branch alongside the existing `classMethods_` lookup: if
`resolveDeclaredType(receiver)` returns a known-external class type (reuse
`isKnownExternalClassType`) AND `isKnownBorrowingMethod(type, methodName)`
is true, accept the call. This is a narrow, additive branch — do not touch
the existing CeeLing-own-class path.

## Step 3 — `DanglingChecker`: register the borrow (the actual safety work)

This is the step that makes rejection/acceptance real, not just "the call
compiles." Two sub-parts:

### 3a. Recognize the call as "borrows receiver" at both call sites that matter

The existing `resolveCallReturnAlias` (`dangling_checker.cpp:368-417`)
resolves borrow-by-PARAMETER-INDEX for CeeLing's own functions via
`summary->returnBorrowsParam`. Eigen's case is different in kind: it borrows
the RECEIVER (`self`), not a parameter — there is no
`FunctionSummary`/`classMethodParams_` entry for `Eigen::MatrixXd` at all
(see point 1 above for why). Do NOT try to force this into
`classMethodParams_`/`summaries_` (that map assumes a real parsed
`ClassDeclaration` backs it — several other call sites may implicitly rely
on that; do not special-case around it, per the earlier plan's own
"Part B" risk note in the internal notes).

Instead, add a SEPARATE, parallel check, e.g. a new method
`DanglingChecker::resolveBorrowingMethodReceiver(const Expression* expr)`:
- If `expr` is a `FunctionCall` with a `MemberAccess` callee,
- and the receiver resolves (via `localType_`) to a known-external class
  type,
- and `isKnownBorrowingMethod(receiverType, methodName)` is true,
- return the receiver's identifier name (same shape as
  `asObjectLocalName`'s return convention).

Call this new function at the exact same point `resolveCallReturnAlias` is
currently called (`processStatement`'s `VariableDeclaration` branch,
`dangling_checker.cpp` around line 847) — try
`resolveCallReturnAlias` first (CeeLing's own functions), then fall back to
`resolveBorrowingMethodReceiver` if that returns empty. Whichever succeeds
feeds the SAME existing observer-registration code (`observerNames_`,
`observerOf_`, `aliasObservers_`) — no new provenance/death-event/use-site
logic needed, this is the part that's already fully built and tested.

### 3b. Confirm receiver-is-moved-away still triggers "observer dangles"

Once 3a registers `b` as an observer of `a` (via `observerOf_["b"] = "a"`),
the EXISTING machinery (`movedFrom_`, `danglingObservers_`,
`checkExpressionUses`) should catch `a`'s death (reassignment or move-into-
sink) and reject a later use of `b` — this is exactly what
`observer_dangles_after_move.clg`/`observer_dangles_after_reassign.clg`
already prove for the `ref` case. Verify this actually fires for the Eigen
borrowing-method case specifically (do not assume the reuse is perfect —
`aliasObservers_`'s exact semantics around "owner moved" vs "owner killed"
should be re-read for this new call path, since it was written with
CeeLing's own function-return-alias case in mind, not a receiver-borrow
case — there may be a subtle mismatch worth checking, e.g. does the
RECEIVER of a borrowing call count as "moved" the same way an argument
does, or does it need its own death-event kind since `a.block(...)` does
NOT consume `a` the way passing `a` to a sink does?).

**This receiver-vs-argument distinction is the trickiest real design point
in this whole plan** — flag it explicitly rather than assuming the existing
alias-observer machinery transfers cleanly: a function argument that
aliases is CONSUMED (moved) by the existing model, but `a.block(...)` must
NOT mark `a` as moved (you can still use `a` normally after taking a block
of it — only a SUBSEQUENT reassignment/move of `a` should kill the
`.block()` result). Read `aliasObservers_`'s full set of use sites in
`dangling_checker.cpp` before implementing 3a to confirm this distinction
is handled correctly, and adjust if the existing alias-observer path
assumes the source was consumed.

## Step 4 — fixtures (required, both directions, per this project's own
verification discipline — a reject-only fixture cannot prove real checking)

Add to `tests/safety/`:
1. `eigen_block_dangles_after_reassign_rejected.clg` — take `b =
   a.block(...)`, reassign `a`, use `b` → must REJECT.
2. `eigen_block_dangles_after_move_rejected.clg` — take `b = a.block(...)`,
   pass `a` to a sink function, use `b` → must REJECT.
3. `eigen_block_alive_accepted.clg` — take `b = a.block(...)`, use `b`
   immediately (source still alive) → must ACCEPT, and (per this project's
   compile-gate discipline) the generated C++ must actually compile against
   real Eigen and run with correct output.
4. `eigen_block_source_reused_after_block_accepted.clg` — take `b =
   a.block(...)`, then use `a` normally (NOT reassign/move it) BEFORE using
   `b` → must ACCEPT (this is the fixture that specifically catches the
   "does taking a block wrongly mark `a` as moved" bug flagged in Step 3b).

Run the full `bash tests/safety/run_safety_tests.sh` (expect 96 + 4 = 100
PASS, 0 regressions) and `ctest` (expect 13/13) after landing.

## Step 5 — update the internal notes

Mark this plan's outcome in the same section the move-tracking fix was
documented in (the internal notes' Eigen spike section) — note explicitly which
methods are covered (`block`/`row`/`col`/`transpose`/`segment`/`head`/
`tail`/`diagonal`), which are NOT (anything not in `isKnownBorrowingMethod`'s
set falls through to the pre-existing default-reject, which is the safe
failure mode), and that the general "scan Eigen's headers automatically"
mechanism is still future work, not done by this pass.

## Order of work (do not reorder)

1. Step 0 (verify parse/resolve assumptions — cheap, de-risks everything
   after it)
2. Step 1 (data-only, no behavior change yet — compiles standalone)
3. Step 2 (call now accepted, but still zero safety tracking — a reject
   fixture at this point would wrongly pass with NO error, confirm this
   expected intermediate state rather than being alarmed by it)
4. Step 3a + 3b together (this is where real safety tracking turns on —
   do not split these, 3a without 3b's receiver-vs-argument fix would
   likely either over-rejct innocent code or under-reject real dangles)
5. Step 4 (fixtures — if any fail, the bug is almost certainly in 3b's
   receiver-vs-argument distinction; re-read that section before guessing)
6. Step 5 (docs)

## If something in Step 0 or Step 3b doesn't hold

Stop and report the specific finding rather than working around it with a
special case — per this project's established discipline (see the internal notes'
"Part B" risk note making the same point about not patching around a
found architectural mismatch). This plan's confidence is high on the
type-name-classification idea (verified against real Eigen source) and
lower on the exact mechanics of wiring a "receiver-borrow, not argument-
borrow" case into `DanglingChecker`'s existing alias-observer machinery,
which was written for a different (argument-borrow) shape. That mechanical
wiring is where an implementer should expect to spend most of the real
effort in this plan, not in Step 1's classification list.
