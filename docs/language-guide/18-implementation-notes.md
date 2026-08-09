# 18. Implementation Notes — How It Actually Works

This chapter explains, in plain terms, *how* the safety checking is built — not the
theory (that is Chapter 5), but the actual moving parts. As of now, one thing works
end-to-end: **catching use-after-move.** Here is how that was done, step by step.

## The pipeline in one picture

```
your .clg file
      │
      ▼
  [ parser ]      turns text into a tree of "nodes" (a class, a variable, a call…)
      │
      ▼
  [ checker ]     walks that tree looking for unsafe patterns → may reject here
      │
      ▼
  [ generator ]   turns the tree into ordinary C++
```

The safety check is a **new stage in the middle**. If it finds a problem, compilation
stops with an error and never reaches code generation.

## Step 1 — Teach every node where it came from

To point an error at "line 22," each node in the tree has to remember which line of
your file it came from. The parser already knew this internally; we just started
**writing it down** on every node (a small `line` / `column` tag).

Why it matters: without it, the compiler could say "there is a use-after-move
*somewhere*" but not *where*. With it, you get:

```
  --> move_then_use.clg:22:11
```

## Step 2 — Make `sink(a);` a real statement

A surprise came up: a bare call like `sink(a);` on its own line was **not something
the parser recognized as a valid statement.** It only understood declarations,
assignments, `return`, `if`, and so on — not "just call a function."

That mattered because *the move happens at that call*. If the parser can't see the
statement, the checker can't see the move. So we taught the grammar one new shape:
"an expression followed by `;` is a statement." Now `sink(a);` is a first-class thing
the checker can reason about.

## Step 3 — The checker itself

The checker is deliberately small — a single walk through each function body, top to
bottom, keeping two lists:

- **object variables** — which locals are real objects (a `Node`), not plain numbers.
  Only these can be moved; `int x = y;` is a copy, not a move, so it is ignored.
- **moved-from** — variables that have been handed away and must not be used again.

As it walks each line, it does two things in order:

1. **Check uses first.** Does this line read a variable that is already on the
   *moved-from* list? If so → **error**.
2. **Then record moves.** Passing an object into a function (`sink(a)`), or assigning
   one object to another (`Node b = a;`), moves the source — add it to *moved-from*.

The order matters. In `sink(a);` the argument `a` is read *before* it is moved, so
that line is fine. It is the *next* line that gets flagged:

```
Node a = new Node(1);
sink(a);        // a is read, then moved — OK, this is the normal auto-move
std::cout << a.v << std::endl;     // a is read again, but it's moved-from now → ERROR
```

Two small but important details:

- **Reassignment revives a variable.** After `a = new Node(2);`, `a` is a fresh object
  again and comes off the *moved-from* list — using it is fine.
- **`std::cout << x` reads but does not move.** Stream output uses its operands, it
  does not consume them. Only real function calls move their arguments.

## Why this is not just "use smart pointers"

Notice the checker never looked at whether `a` is a `unique_ptr` or a `shared_ptr`.
Use-after-move is a **liveness** question — "is this variable still valid here?" — and
that is answered by tracking moves through the code, not by the pointer type. This is
the same engine the other safety checks will plug into (Chapter 5): one table of "who
is alive," many consumers.

## What actually passes today

The move-tracking above is enough to make the compiler reject its first real dangling
bug:

```
error[semantic/use-after-move]: use of moved-from value `a`
  --> tests/safety/move_then_use.clg:22:11
  help: `a` was moved earlier; assign a new value before using it
```

Verified behavior:

- `move_then_use.clg` — **rejected** (was passing through before).
- The positive controls (`observer_alive_ok`, `owner_alive_ok`) — **still compile**,
  so valid code is not wrongly rejected.
- All existing tests — **still green**, no regressions.

## Step 4 — `ref` observers, and why they can't be parameters

The next increment taught the checker about **observers**. Recall the model: `Node b
= a;` moves, `ref Node b = a;` observes. So the parser learned the `ref` keyword (it
is now its own marker, not lumped into the type), and the checker keeps a third list:

- **observer → owner** — for `ref Node b = a;`, it records "b observes a."

Now two things fall out naturally, reusing the same "who is alive" tracking:

1. **Observer dangling.** When an owner is moved away, any observer of it is now
   pointing at nothing. Using that observer is flagged:
   ```
   Node a = new Node(1);
   ref Node b = a;   // b observes a
   sink(a);          // a is moved → gone
   std::cout << b.v << std::endl;       // ERROR: b's owner is gone
   ```
   ```
   error[semantic/use-after-move]: use of dangling observer `b` (its owner `a` is gone)
     --> observer_dangles_after_move.clg:24:11
   ```

2. **Observers cannot be function parameters.** This is a *language rule*, enforced at
   parse time: `void sink(ref Node n)` is rejected outright. The reason is safety by
   *construction* rather than by analysis — if an observer could be passed into a
   function, it could be stored in a field or returned, escaping its owner and
   dangling. By simply **not allowing it**, that entire class of escape bug disappears
   without needing to check for it. Observers stay local, short-lived, and easy to
   reason about; function arguments always move.
   ```
   error[semantic/observer_param]: `ref` observers cannot be function parameters …
   ```

This is the "restricted subset" philosophy in action: **the cheapest way to make a
danger safe is to remove it from the language**, not to detect it.

## Step 5 — Scope-exit deaths

The last new-syntax case: an owner declared inside a `{ }` block dies when the block
ends. The checker walks a nested block (an `if` body) **without** resetting state — so
an observer declared outside stays visible — then, at the block's end, finds the
owners that were *born inside* the block and kills them. Any observer of a killed owner
is flagged on later use:

```
ref Node b = nullptr;
if (1) {
    Node inner = new Node(1);
    b = inner;        // b observes inner
}                     // inner dies here
std::cout << b.v << std::endl;           // ERROR: b's owner is gone
```

This also taught the checker that an observer can be **rebound** by assignment
(`b = inner;`), not only at its declaration.

With this, **every new-syntax dangling fixture passes.**

## What is deliberately not done yet

Honest scope. The checker handles use-after-move, observer-dangling (owner moved,
reassigned, or scope-exited), and the observer-parameter ban. It does **not** yet
handle:

- **Branch merges** — being conservative across `if`/`else` paths (today a block is
  analyzed, but the two arms are not merged).
- **Library summaries** — knowing that, say, `vector::push_back` invalidates iterators.
- **The legacy `owned<>/borrowed<>` fixtures** — a different (older) syntax, kept only
  for regression.

Each of these lights up one more of the red fixtures, using the same small engine —
no rewrite, just more cases fed into the same "who is alive" table.

## Step 6 — Generating C++ from structure, not from text

Everything above is the *checker* — the part that decides accept/reject. This step is
about the *generator* — the part that turns accepted CeeLing into C++.

### The problem: C++ can't be generated correctly by find-and-replace

The first version of the generator didn't look at the parsed program at all — it
edited the *source text* directly, using pattern-matching rules like "find
`ClassName varName;` and replace it with `std::unique_ptr<ClassName> varName;`."

Text patterns can't see *meaning*, and C++ needs meaning to translate correctly:

- `next.grow(x)` (a method call) and `next.v` (a field read) look identical as text
  — "word, dot, word" — but a chain like `a.b.c()` needs `->` inserted at the right
  spots, which depends on knowing what type each piece is.
- The same word means different things in different places: `next` inside a class's
  own method is a field (`this->next`); the same word elsewhere is just a variable.
  Text search has no idea which one it's looking at.
- Deciding when to hand off ownership (`std::move`) depends on whether something is an
  object or a plain number — information the text itself doesn't carry.

The old approach patched around this with growing lists of special cases, and it could
never fully solve the general problem, because "what type is this?" isn't something
you can answer by matching text patterns — it requires understanding the program's
structure.

### How CeeLing solves it: generate from the parsed structure

The new generator (`AstEmitter`) works on the same parsed representation of the
program that the safety checker uses, instead of raw text. Because it knows the real
structure — what class a method belongs to, what type each variable is, what a field
resolves to — it can answer the questions text-matching couldn't:

- It knows which bare names are fields of the current class, so it can correctly add
  `this->` only where needed.
- It can follow a chain of accesses (`a.b.c`) and resolve each step's type, so `->`
  gets inserted correctly at arbitrary depth — not just for one hop.
- It knows when an argument is being handed off to another function, so it can safely
  insert `std::move` — safely, because the checker has already proven the value isn't
  used again afterward.

### Proof by example

Take a small linked structure with a method that recurses on a field:

```cpp
// CeeLing
class Node {
public:
    int v;
    Node next;
    Node(int x) { v = x; }
    void grow(int x) {
        if (next) {
            next.grow(x);
        } else {
            next = new Node(x);
        }
    }
};

int main() {
    Node a = new Node(1);
    a.grow(2);
    return 0;
}
```

The AST-driven emitter produces:

```cpp
class Node {
public:
    int v;
    std::unique_ptr<Node> next;
Node(int x) {
    this->v = x;
}
void grow(int x) {
    if (this->next) {
        this->next->grow(x);
    } else {
        this->next = std::make_unique<Node>(x);
    }
}
};

int main() {
    std::unique_ptr<Node> a = std::make_unique<Node>(1);
    a->grow(2);
    return 0;
}
```

`next` becomes `this->next` because the generator knows it's inside `Node`'s method.
`next.grow(x)` becomes `this->next->grow(x)` because it resolves `next`'s type at both
steps of the chain. This is real C++ that compiles as-is — not something the old
text-matching approach could produce reliably for chains like this, no matter how many
special cases were added, because the question it needs answered ("what type is this
expression?") isn't one text patterns can answer.

### Linking vs. handing off

`a.b = x;` (storing into a field, like linking a node into a list) and `a = x;`
(assigning a plain variable) look similar but mean different things. The second one
hands ownership of `x` over — `x` shouldn't be used again afterward. The first one
just links `x` in; the caller may well keep using `x` right after, which is completely
normal for structures like linked lists (`node.next = other; other.prev = node;`). The
generator treats these differently: only the plain-variable case triggers an automatic
move.

Getting this distinction wrong at first caused a real bug: a value handed off inside
one branch of an `if`/`else` was briefly treated as unusable even in code paths where
that branch never ran, which wrongly rejected valid programs. That's fixed now — a
move that happens conditionally, inside just one branch, isn't assumed to have
happened once you're past the `if`.

### Status

The new generator is the default today; the older text-based generator remains
available via `--legacy-cpp` for comparison. Both generators agree on what the safety
checker accepts or rejects — this is purely about how accepted code gets turned into
C++. See Step 7 for why the switch was forced, not optional.

## Step 7 — Sealing escape hatches, and why the generator switch wasn't optional

Chapter 17 describes *what* is forbidden (`.get()`, raw pointers, casts, `delete`,
`union`). Building the actual check surfaced two problems that weren't visible until
this was attempted for real.

### The silent-drop bug

The parser's tree can contain an `ERROR` node wherever it hit something it couldn't
make sense of — that's expected; a syntax error should produce one. The bug was in
what happened *next*: every place that collects a function/method/constructor body
(`for (child : body) { if (child is a statement) build it }`) simply **skipped**
anything that wasn't a recognized statement, `ERROR` node included. No error was
raised. The statement was just... gone from the tree, as if it had never been written.

This meant a program containing `Node* p;` didn't get rejected — it got a *slightly
different program* silently substituted in its place, missing whatever line couldn't
be understood. For a class with a `private:` field, that meant the field itself
vanished, and every method that referenced it got miscompiled around the hole. The
program would still "succeed" (exit code 0), because as far as the compiler could
tell, there was nothing wrong with the (shorter) program it actually built.

Fixed by making every one of those body-collecting loops treat an `ERROR` node, or a
statement that fails to build, as a hard error instead of a skip. This is also what
makes rejecting `Node* p;`, `delete p;`, and the others inside a function body
actually work — before this fix, the rejection logic could be sitting right there and
still never fire, because the offending line had already been quietly erased before
the checker ever got to look at it.

### Why sealing the AST wasn't enough on its own

With the silent-drop bug fixed, the validated AST correctly excludes every forbidden
construct. But CeeLing had two generators, and only one of them actually reads that
AST to produce its output. The original generator (`CppGenerator`) works by
regex-substituting the **raw source text** — it never looked at the parsed tree for
most of what it emits. So even with the AST layer perfectly rejecting `Node* p;`,
running it through `CppGenerator` would still copy that exact text into the output
C++, untouched, because the text substitution pipeline doesn't know or care what the
AST-level check decided.

Confirmed directly: before this fix, `Node* p;`, `delete a;`, and
`reinterpret_cast<int>(1.0)` all transpiled successfully (`exit 0`) and produced real
`.cpp` files containing those exact dangerous constructs verbatim.

This is why `AstEmitter` — which can only emit node types it recognizes, because it
walks the tree structurally rather than substituting into text — had to become the
**default** generator, not stay an opt-in alternative. "Rejected in the checker" only
means something if the thing that turns accepted code into C++ is reading from the
same tree the checker validated. The old generator is kept around (`--legacy-cpp`)
for comparison, but does not carry the Phase 1 guarantee.

### The `.get()` / cast ambiguity

Two hatches don't produce a parse error at all, because their tokens are already
meaningful in ordinary CeeLing syntax:

- `.get()` is grammatically an ordinary method call. `p.get()` and `box.get()` look
  identical in the tree; only *which class* `p`/`box` is, and whether that class
  declares its own `get()`, tells them apart. This needs a semantic pass with type
  information, not a grammar rule — see `EscapeHatchChecker`, which tracks
  local/parameter/field types (mirroring the same lightweight resolution
  `AstEmitter` already does for `this->` lowering) well enough to answer "is this
  receiver a CeeLing object, and does its class define its own `get()`?"
- `reinterpret_cast<int>(x)` has no cast grammar to claim the `<`/`>` tokens, so it
  parses as a chain of comparisons: `reinterpret_cast < int > (x)`. This is caught by
  name — `reinterpret_cast`, `const_cast`, `static_cast`, `dynamic_cast` can never be
  legitimate CeeLing identifiers, so their appearance as an `Identifier` node is
  itself the signal, independent of whatever nonsense the rest of the "comparison"
  parses into.
- Symmetrically, `Node* p;` parses as multiplication (`Node * p`) once `*` exists in
  the grammar for real arithmetic — caught the same way, by noticing the left operand
  of `*` is a known class name, which a real multiplication never has.

### A second, distinct dangling-checker bug found along the way

Getting `while` loops working (needed to exercise the escape-hatch checks against a
realistic program, `tests/linked_list.clg`) surfaced a second conditional-move bug,
separate from the one described above under "Linking vs. handing off." That earlier
fix made a move *inside* one `if`/`else` branch not leak out to code *after* the
`if`. This new one was about the two branches leaking into **each other**: the `else`
branch was being checked while still carrying moved-from state left over from
whatever the `if` branch had done, because only the *pre-`if`* snapshot was restored
— not a fresh snapshot per branch. Since only one branch ever actually runs, a name
moved in the `if` branch has no business being "already moved" when checking the
`else` branch. Fixed by snapshotting/restoring around each branch independently, then
merging: a name is moved after the `if` only if *either* branch moved it (the
conservative, "can't know which branch ran" answer).

## Step 8 — Auto-derived function summaries, and the pointer-type bug they surfaced

CLAUDE.md's component A calls for per-function borrow summaries: which parameters are
borrowed, which are moved, and what the return value borrows from. Before writing this,
it's worth noticing that CeeLing already answers the parameter half by construction —
`ref` is banned as a function parameter (Step 4), so every object-typed parameter is
unconditionally *moved* into the callee. There is no borrowing-parameter case to
summarize yet. That leaves exactly one question a summary needs to answer:

> Does this function's return value alias one of its own parameters (a passthrough,
> `T f(T a) { return a; }`), or is it always a fresh value?

`DanglingChecker::deriveSummaries` walks every function/method body once, before any
body is checked, and records `returnBorrowsParam` (a parameter index, or -1 for
"fresh") per function. A call site that assigns the result — `Node r = pick(x, y);` —
looks the callee up and, if the summary says the return aliases parameter *i*,
registers `r` as an **observer** of whatever object local was passed at that position,
reusing the exact same `observerOf_`/`danglingObservers_` machinery that an explicit
`ref` already uses. No new death-event logic was needed — only a new way to populate
the provenance table.

One subtlety: the argument passed to `pick` is *also* moved-from by the call itself
(as every call argument is). Naively, "the object `r` observes" and "the owner that
call just moved from" are the same name, so the existing "an observer's owner is
moved-from ⇒ dangling" check would immediately flag `r` as dangling right after
creation — always true, useless. The fix is a small `aliasObservers_` set: for an
alias-derived observer, only an *explicit* death event (the owner being reassigned or
falling out of scope) makes it dangling, not the move that produced the alias in the
first place. `tests/safety/summary_return_aliases_param.clg` (accept — using `r` while
`x`'s object is still alive) and `summary_return_alias_dangles.clg` (reject — `x` gets
reassigned out from under `r`) are the fixtures that pin this down.

### A pre-existing bug this surfaced: parameters had no real pointer type

Writing the accept fixture immediately hit a compile error unrelated to the dangling
logic: `pick`'s generated signature was `std::shared_ptr<Node> a`, but the caller's
locals were `std::unique_ptr<Node>` — moving a `unique_ptr` into a `shared_ptr`
parameter doesn't compile. Tracing it back: `SmartPointerAnalyzer` had never registered
function/method/constructor *parameters* at all — only class fields and locally
declared variables. `AstEmitter` then hardcoded every object-typed parameter as
`std::shared_ptr<T>` in the function signature, independent of whatever pointer type
the analyzer would have derived from actual usage. This had been true since parameters
were first emitted; it just never had a fixture shaped like "pass a `unique_ptr` local
into a function and use the result" until now.

Fixed in two parts:

- `SmartPointerAnalyzer::analyzeStatement` now registers each object-typed parameter
  the same way a local declaration is registered — but *deliberately minimally*: only
  identity (scope, class, name), not the `isAssignedFrom`/back-reference bookkeeping
  that drives the shared/weak-promotion heuristics. Those heuristics are tuned around
  locals and fields observed through full statement analysis, and wiring parameters
  into them directly risked perturbing unrelated decisions in ways that weren't
  verified safe. With this minimal registration, a parameter that is never itself
  stored into a shared field defaults to `unique_ptr` (the same base case a fresh local
  gets); a parameter that *is* stored into a field still gets promoted to `shared_ptr`
  by the existing (separate, regex-based) `scanSource` pass, which already covered
  that case — see `tests/new_expression_paths.clg`'s `attach(Node child)`, which
  stores `child` into a `shared_ptr`-promoted field.
- `AstEmitter::emitFunction` now asks `analyzer_.getPointerType(paramType, paramName)`
  for each parameter's smart-pointer type instead of hardcoding `shared_ptr`.

A second, related bug came out of the same investigation: a bare `new T(...)` passed
*directly* as a call argument — `head.attach(new Node(3))`, no named variable of its
own — always emitted `std::make_shared<T>(...)`, regardless of what the receiving
parameter actually needed, because the argument-emission path had no way to know which
function was being called or what its parameter's type was. Fixed by
`AstEmitter::calleeParam`, which resolves the callee (free function by name, or method
by the receiver's statically-known class) and returns the `(class, name)` of the
parameter at that argument position; `emitMovedArg` uses it to pick the right factory
(`make_unique` vs `make_shared`) for a literal `new` argument instead of a
context-free default.

## Step 9 — Return-local-reference (#2)

CeeLing's grammar has no `ref`-qualified return type — a function's `returnType` is
always a plain type name, which the emitter always lowers to an owning smart pointer
(or a value type). That's a real structural guarantee, but it only covers the
*declared* return type; nothing stopped the checker from accepting a `return`
statement whose *expression* was a `ref` observer:

```
Node makeDangling() {
    Node n = new Node(1);
    ref Node b = n;
    return b;      // b observes n, which is about to stop existing
}
```

`n` lives only inside `makeDangling`'s own stack frame. `b` observes it. The instant
the function returns, that frame is gone, and `b` — along with whatever the caller
would receive — refers to nothing. In this particular repro, C++'s own type system
happens to reject the generated code too (`unique_ptr<Node>` has no implicit
conversion from `Node&`), but that's incidental to the return type chosen for *this*
function, not a guarantee: it's exactly the kind of thing that should be a clear
`.clg`-level diagnostic, not a mystery error several layers downstream in generated
code the user never wrote by hand.

The fix doesn't need new machinery. Every observer already has a name in
`observerNames_`, populated at the point of its `ref` declaration. `checkReturnedReference`
just asks: is the returned expression a bare identifier, and is that identifier in
`observerNames_`? If so, reject. There's no need to separately ask "does it observe a
parameter or a local" — `ref` parameters are already banned at parse time (Step 4), so
any observer name reachable inside a function body was necessarily declared *in that
body*, making it unconditionally local. Returning the *owner* itself (`return n;`) is
untouched — that's the existing, already-supported factory pattern.
`tests/safety/return_local_ref_rejected.clg` (reject) and
`return_owner_not_observer_accepted.clg` (accept — same function shape, but returning
the owner) pin down the boundary.

## Step 10 — Reference-stored-in-field escape (#3), and a third silent-drop site

The legacy `tests/borrow_stored_in_field.clg` fixture describes the threat: a class
holds a borrow in a field (`Holder.ref`), a method stores a caller's reference into
that field, and later code reads through the field after the original owner is gone —
the field outlived the thing it was pointing at. That fixture uses the superseded
`borrowed<T>` scaffolding syntax, so it says nothing about whether *modern* CeeLing
can express the same danger.

It can't — but not because anything detects it. Checking `member_variable_declaration`
in the grammar shows it has no `optional($.observer_qualifier)` the way
`variable_declaration` (locals) and `parameter_list` do. There is no grammar
production for a `ref`-typed field at all. Combined with the pre-existing ban on `ref`
parameters (Step 4), the two together close every route a borrow could take into a
field: it can't arrive as a parameter, and it can't be declared as a field type. What
*can* be stored in a field is an owning value — `stored = n;` where `n` is a
moved-in parameter lowers to `this->stored = std::move(n);`, a real ownership
transfer, not a borrow.

Trying to actually write `ref Node r;` as a field to confirm this surfaced a third
instance of the Phase 1 silent-drop pattern (the first was body-collecting loops, the
second was `member_variable_declaration`'s own hidden ERROR child covered in Step 7).
Since fields have no `ref` production, tree-sitter doesn't reject `ref Node r;`
outright — it parses `ref` as the field's *type* (a plain identifier, grammatically
valid) and `Node` as the field's *name*, leaving the real name `r` behind as an
orphaned `ERROR` node, a third child of `member_variable_declaration` that
`buildMemberVariableDeclaration` never looked at (it only ever reads children 0 and
1). The result: `class Holder { ref Node r; }` silently became `class Holder { ref
Node; }` in the AST — a field of made-up type `ref`, named `Node`, no error raised, no
sign anything was wrong until it failed to compile as legal C++ several steps later
for an unrelated-looking reason.

Fixed the same way as the other silent-drop sites: `buildMemberVariableDeclaration`
now checks its own child count and rejects anything beyond the well-formed
`type identifier ;` shape. Because this exact malformed shape is common enough to be
worth a clear message (rather than the generic parse-error fallback), a `type == "ref"`
check produces a dedicated diagnostic naming the actual rule being enforced. See
`tests/safety/ref_field_rejected.clg` (reject) and `field_stores_owner_accepted.clg`
(accept — proves the ban doesn't touch the ordinary moved-parameter-into-field
pattern).

## Step 11 — Branch-merge precision, and a real false positive it fixed

"Linking vs. handing off" and Step 7's second bug both talk about `movedFrom_`
specifically: a move inside one `if`/`else` branch shouldn't leak past the `if`, and
shouldn't leak into the *sibling* branch either. Both fixes were scoped to
`movedFrom_` alone. The observer-tracking state — `observerOf_` (who does an observer
point at), `danglingObservers_` (which observers are known-dead), `aliasObservers_`,
`observerNames_` — never got the same treatment. It was additive across both
branches with no snapshot/restore at all.

That gap is real, not hypothetical:

```
Node a = new Node(1);
Node b = new Node(2);
ref Node obs = a;
if (cond) {
    obs = b;           // rebind: obs now observes b
    b = new Node(3);   // reassigning b kills the object obs (in this branch) observes
} else {
    std::cout << obs.v << std::endl;   // obs still observes a here -- a is alive!
}
```

The `else` branch never touches `obs` at all — on that path, `obs` is exactly what it
was before the `if`: an observer of `a`, which is alive. But the checker rejected it,
because processing the `if` branch first mutated `observerOf_["obs"]` to `"b"`, and
that mutation was still sitting there when the `else` branch got checked next — the
`else` branch's `std::cout << obs.v` looked up `obs`'s owner and found `b`, which the
`if` branch (which never ran, on this path) had just killed.

The fix generalizes the `movedFrom_` snapshot/restore pattern into one `BranchState`
struct covering all five pieces of state, plus a `mergeBranchStates` step that
replaces the old "just restore and move on" flow:

- **`movedFrom_`, `danglingObservers_`, `aliasObservers_`, `observerNames_`**: still a
  simple union — a name affected in *either* branch stays affected after the merge,
  same reasoning as before (a later use can't know which branch ran).
- **`observerOf_`** needs more care, because it's not a boolean flag but a *binding*
  (which owner does this observer point at). For every observer name either branch
  touched: if both branches agree on the owner (or only one branch touched it and the
  other implicitly kept the pre-`if` binding), that binding is certain either way —
  keep it. If the branches disagree — rebound to different owners, e.g. `obs = b;` in
  one arm and nothing in the other where `obs` was already bound to something else —
  the merge can't know which binding survives, so it's marked dangling. This is the
  same "ambiguous → conservative reject" principle CLAUDE.md states for the analysis
  generally, just applied to observer bindings specifically. `while` loops get the
  same treatment, modeled as a two-way branch against "the loop body never runs".

One easy way to get this subtly wrong: in the ambiguous-disagreement case, it's
tempting to *only* mark the name dangling and leave `observerOf_` without an entry for
it. That silently breaks the fix, because the use-site check
(`checkExpressionUses`) only consults `danglingObservers_` for a name that already
*has* an `observerOf_` entry — it needs an owner name to put in the diagnostic. Without
an entry, the whole dangling branch of the check is skipped and the ambiguous case
would wrongly pass. The fix keeps a (possibly stale, but only used for the message)
owner name in `observerOf_` even in the disagreement case, so the existing
`danglingObservers_` lookup still fires.

`tests/safety/branch_merge_no_false_positive_accepted.clg` pins down the false
positive above; `branch_merge_ambiguous_rejected.clg` is the positive control proving
the fix doesn't over-correct — the same rebind pattern but with *both* branches
rebinding `obs` (to different owners) must still be rejected, since that really is
ambiguous.

### A separate gap this surfaced: `ref` rebinding had no C++ lowering (now fixed)

Writing the false-positive fixture above surfaced an unrelated, pre-existing problem:
`obs = b;` (rebinding a `ref` observer to a different owner) could not be emitted as
valid C++ at all. `AstEmitter` lowered `ref T x = y;` to a native reference, `T& x =
*y;` — and a native C++ reference is bound for its entire lifetime; there is no syntax
to make an existing `T&` refer to something else afterward. `obs = b;` in the
generated code was unavoidably interpreted by C++ as *assignment through* the
reference (copying `b`'s contents into whatever `obs` already refers to), which is not
what CeeLing's rebind semantics mean and typically didn't even type-check (a smart
pointer has no `operator=` accepting another smart pointer's pointee). The dangling
checker's own model of rebinding was already correct (`observerOf_[name] = newOwner`,
with no move applied to the RHS); only the *emitter* was wrong.

## Step 12 — `ref` observers: from `T&` to `T*`

Fixed by changing what a `ref` observer lowers to. A native reference can't be
rebound; a raw pointer can. So:

```
ref T x = y;      ->   T* x = (y).get();
x = z;  (rebind)  ->   x = (z).get();
ref T x = nullptr;  ->  T* x = nullptr;
```

This is not a reopening of the raw-pointer ban (Chapter 17, hatch #15/#14) — that
ban is about what a user can *write* in a `.clg` file, checked by
`EscapeHatchChecker` against the user's own AST, and it is completely untouched by
this change. What changed is a codegen decision inside `AstEmitter`, for a construct
(`ref`) whose safety the dangling checker has already fully established before any
C++ is emitted. The raw pointer here is invisible from CeeLing source, unreachable
from CeeLing source, and cannot be extended into a dangling one by anything the user
writes — it exists purely as an implementation detail of a proof that already
happened. Chapter 17 now has a short note making this "source vs. generated code"
distinction explicit, since a reader inspecting `.cpp` output and seeing a raw
pointer might otherwise wonder if the guarantee had quietly been dropped.

The `.get()` calls in the generated code are the same story: `EscapeHatchChecker`
rejects `.get()` in `.clg` source (Chapter 17), but that check runs on user-authored
AST nodes and has no opinion about what `AstEmitter` writes into `.cpp` output — the
compiler already calls `std::make_unique`/`std::make_shared` directly for `new`,
which is exactly this same category of "internal codegen, not a user-reachable
construct."

Two smaller things fell out of the same fix, for free:

- **Member access on observers now needs `->` in the generated code**, not `.`
  (`obs.v` -> `obs->v`), since `obs` is now a pointer. This is handled by
  `exprObjectClass` — the single function that already decides `.` vs `->` for smart
  pointers — recognizing observer names via a new `observerClass_` map, populated
  alongside the existing `VarKind::Reference` tag (which, it turned out, had been set
  on declaration but never actually read by anything until now). Source-level syntax
  doesn't change at all; `obs.v` is still written with a dot.
- **A latent null-dereference bug disappeared as a side effect.** `ref Node b =
  nullptr;` used to lower to `Node& b = *(nullptr);` — dereferencing a null pointer,
  undefined behavior if it were ever compiled. This had never surfaced in a compiled
  fixture because every existing `nullptr`-observer fixture
  (`observer_dangles_after_scope.clg`) is an `//@ expect: reject` case, so codegen
  never actually ran on it. With the `T*` lowering, `ref Node b = nullptr;` is simply
  `Node* b = nullptr;` — valid, unremarkable C++.
  `tests/safety/observer_nullptr_rebind_accepted.clg` is the first accept-case
  fixture to exercise `ref ... = nullptr;` through to compiled output, closing the
  gap for real rather than leaving it merely untested.

`tests/safety/branch_merge_no_false_positive_accepted.clg` (from Step 11, exercising
the rebind) now compiles under g++ where it previously only passed the dangling
checker's own exit-code check — direct proof the fix closes the gap Step 11 left
open.

## Step 13 — `std::vector<T>`, and why one container's elements can't be two types

Phase 2.6 ("std container support") starts here, deliberately scoped to the smallest
slice that's genuinely usable: `std::vector<T>` with `push_back`/`size`/`begin`/`end`
and range-`for` — no `operator[]` (no subscript grammar exists yet), no
`map`/`string`, no iterator-invalidation analysis (there's nothing to invalidate
until vectors exist). Three things made this bigger than "add a grammar rule and a
codegen branch."

### Generic syntax didn't exist, and had to be added carefully

`std::vector<int>` didn't parse before this — `<`/`>` were consumed as comparison
operators, `int` as a bare identifier, silently, with no `ERROR` node at all (the
same failure class Step 7 found repeatedly: a *misparse*, not a rejection). Adding
`generic_type: scoped_identifier '<' type '>'` reintroduced the same
`scoped_identifier '<'` ambiguity Step 7 fixed for casts, this time against real
multiplication/comparison — resolved with `prec(10)`, verified by parsing both
`std::vector<int> v = ...;` and a plain `a < b` comparison in the same file.

A second, smaller grammar problem followed directly from Phase 1's definite-init
rule: `variable_declaration` requires `= expression`, so there was no way to write
`std::vector<int> v;` (the ordinary way to start an empty vector) at all. Unlike a
bare `int n;` or `Node n;`, though, a container's default-constructed (empty) state
is always well-defined and safe to read from — there's no uninitialized-memory risk
to guard against. So `container_declaration: generic_type identifier ';'` was added
as the one deliberate exception to definite-init, scoped narrowly to `$.generic_type`
so it can never apply to a plain class or primitive. This created a genuine
1-token-lookahead ambiguity with `variable_declaration` (both start `generic_type
identifier`, and the parser can't know which production it's in until it sees `;` or
`=` next) — not a precedence problem, so `tree-sitter generate`'s suggested fix
(`conflicts: [[$.container_declaration, $.type]]`) was the right one: it tells the
generator this ambiguity is real and GLR-resolvable at parse time, not a grammar bug
to eliminate.

### Why a vector's elements can't just "be shared_ptr" or "be unique_ptr"

The obvious first design — always wrap class-typed elements in `shared_ptr`, since
`shared_ptr` copies cheaply and a vector needs to store/return elements by value —
breaks immediately: a plain local declared `Node a = new Node(1);` is independently
inferred as `unique_ptr` by `SmartPointerAnalyzer` (Rule A: single owner, no sharing
signal), and `v.push_back(a)` then fails to compile — there's no implicit conversion
from `unique_ptr<Node>` to `shared_ptr<Node>` by copy. The equally obvious opposite
fix — always `unique_ptr`, and make `push_back` always *move* — avoids that specific
error, but breaks the moment two different pointer-kind sources both need to go into
the same vector, and contradicts the existing "storing into a field links, doesn't
consume" precedent from member-target assignment (`a.b = obj;`).

The real constraint neither naive fix respects: **a single `std::vector<T>`'s
element type is fixed** — `vector<unique_ptr<Node>>` and `vector<shared_ptr<Node>>`
are different C++ types, so a container variable's element kind has to be decided
*once*, consistently, and every `push_back` into it has to work with that decision.
That's a whole-program property, not a per-call-site one — the same shape of problem
`SmartPointerAnalyzer`'s existing two-pass design already solves for ordinary
fields/locals, so the fix follows the same pattern: `scanContainerElementKinds` is a
new, self-contained pass (own data — `containerElementShared_` — own recursive
statement walk) that runs *after* the existing two-pass decision, over every
`push_back` call site in the program. For each one, it looks up the pushed
argument's own already-decided pointer kind (via `getPointerType`, keyed by the
argument's class and name) and records: if *any* push into a given container
variable came from an already-shared source, that container's elements are
`shared_ptr`; otherwise `unique_ptr`. It runs after (not folded into) the existing
second pass specifically because it depends on that pass's output — a chicken-and-egg
problem solved by ordering, not by merging the two passes together.

`SmartPointerAnalyzer::containerElementIsShared(varName)` is deliberately kept
separate from the existing `pointerDecisions`/`variableReferences` machinery (own
map, own pass) rather than threaded through it, for the same reason earlier
container-related additions to that class stayed minimal (Step 8's parameter
registration comment explains this directly): that engine's heuristics are tuned
around locals/fields observed through full statement analysis, and it wasn't
verified safe to wire container semantics into it without risking the existing
back-reference/bidirectional-assignment rules.

### `push_back` shares or moves depending on the specific argument, not the method

Once a container's element kind is fixed, whether one particular `push_back` call
shares or moves its argument turns out to depend on that argument's own pointer
kind, not on `push_back` as a blanket rule:

- Element kind shared, argument already shared → **shares**: plain copy (a
  `shared_ptr` refcount bump), the argument remains a live, usable owner. This is
  the case that matches "storing into a field links, doesn't consume."
- Element kind shared, argument is unique → **moves**, but has to be *wrapped*:
  `std::shared_ptr<Node>(std::move(a))`, since a `unique_ptr` can't be copied into a
  `shared_ptr` slot, only moved-and-converted. The argument is consumed either way.
- Element kind unique → **always moves**, the ordinary call-argument rule, no
  wrapping needed (`unique_ptr` moves anywhere freely).

Both `DanglingChecker` (does the argument count as moved-from afterward?) and
`AstEmitter` (plain copy vs. `std::move(x)` vs. `std::shared_ptr<T>(std::move(x))`)
have to agree on exactly this rule, or the checker's answer and the generated code's
actual behavior diverge. That agreement is why `DanglingChecker` gained a dependency
on `SmartPointerAnalyzer` in this step — the first time these two components have
been coupled. Previously `DanglingChecker` was purely AST-level, with no opinion on
pointer *types*, only on *lifetimes*; but "is this argument already shared" is
inescapably a pointer-type question. `main.cpp` now constructs and runs
`SmartPointerAnalyzer` before `DanglingChecker::check` (previously it only ran for
the AST-emitter branch, after the dangling check), and passes it in by reference.

`tests/safety/vector_push_back_shares_when_already_shared_accepted.clg` and
`vector_push_back_moves_unique_source_rejected.clg` are the fixture pair pinning
down this exact boundary — same-shaped source, different pointer-kind history for
the pushed variable, opposite verdicts.

### A pre-existing bug this surfaced: method-call receivers were never checked

Writing `vector_moved_into_call_rejected.clg` (`sink(v); v.size();`) surfaced a bug
with nothing to do with containers: `DanglingChecker::checkExpressionUses`'s
`FunctionCall` branch walked only the call's *arguments*, never `call->callee` (the
`recv.method` `MemberAccess` for a method call). So `a.getV()` on a moved-from `a`
silently passed — a real gap, present since the dangling checker's earliest version,
just never exercised: every prior fixture that used a moved-from object afterward
did so through a bare field read (`b.v`), which the `MemberAccess` branch already
covers by checking `member->object`, never through a method call. Fixed by also
walking `call->callee` when present, using the exact same `checkExpressionUses`
recursion already in place.

## Step 14 — `v[i]` subscript access, and container invalidation as a third death event

Slice 1 deliberately stopped short of `operator[]` and iterator-invalidation
analysis (threat #4): without subscript syntax there's no way to produce an
observer *of* a container element, so "push_back may invalidate existing
references" had nothing to invalidate yet. This step closes that gap.

### Subscript syntax reuses the tight-binding trick from Step 6/7, not precedence

`member_access` and `function_call` already bind tightly to `.`/`->`/`()` not
through a numeric `prec(N)` climb, but by restricting their left-operand grammar to
`choice($.member_access, $.primary_expression)` — deliberately excluding general
`$.expression`, so `.`/`->`/calls can never silently apply across a full binary
expression without parens. `subscript_expression` follows the exact same pattern:

```js
subscript_expression: $ => prec.left(seq(
  choice($.subscript_expression, $.member_access, $.primary_expression),
  '[', $.expression, ']'
)),
```

with `member_access` and `function_call`'s operand `choice(...)` widened to also
include `$.subscript_expression`, so `v[i].field`, `a.v[i]`, and `v[i].method()` all
compose bidirectionally. This regenerated with **zero conflicts** on the first
attempt — a real contrast to slice 1's `generic_type`/`container_declaration` work,
which needed a `prec(10)` fix and a `conflicts:` entry. The difference: subscript's
`[`/`]` brackets are unambiguous delimiters the moment they appear, unlike `<`/`>`
(shared with comparison operators) or the `generic_type identifier` vs
`variable_declaration` prefix overlap.

The AST node, `SubscriptExpression` (`object` + `index`, both real `ExpressionPtr`s
— unlike `MemberAccess::member`, which is a bare name, an index is itself an
expression that can be checked/emitted recursively), and its builder mirror
`MemberAccess`/`buildMemberAccess` exactly.

### `v[i]` observes the container, not an element — a deliberate granularity choice

The natural question once `ref Node n = v[i];` needs to be trackable: should `n`'s
provenance point at "element `i` of `v`," or at `v` itself? Per-element provenance
would be more precise in principle, but elements in a `std::vector` have no
independent identity — `push_back` can reallocate the entire backing store, which
invalidates *every* existing reference into the vector regardless of which index it
came from. Tracking per-element provenance would imply a precision the underlying
container doesn't actually provide, so `n` observes the container `v` as a whole.

Mechanically, this meant giving `DanglingChecker` a second way to answer "what does
this expression observe," alongside the existing `asObjectLocalName` (bare
identifier only). `asContainerSubscript` recognizes `v[i]` specifically when `v` is
a live, container-typed local (not an arbitrary subscript on some unrelated
expression), and a new `resolveObservedOwner` choke point tries
`asObjectLocalName` first, then falls back to `asContainerSubscript`, feeding both
of the `ref`-binding sites (initial declaration and observer rebind) that
previously called `asObjectLocalName` directly. Move-tracking call sites
deliberately keep calling `asObjectLocalName` unchanged — a container subscript
result is never itself a movable owner, only ever an observed target.

### Invalidation is a third death event, not a new mechanism

Before this step, exactly two things could kill an owner and dangle its observers:
reassignment (`AssignmentStatement`) and scope-exit (`endBlockScope`) — both work by
inserting into `danglingObservers_` every observer whose `observerOf_` value equals
the dying name. `push_back`-invalidates-existing-observers turns out to be
*structurally identical*: "the container's previous backing storage is gone" is the
same shape of event as "the owner is gone," just triggered by a mutating call
instead of a reassignment or scope boundary.

`library_summary.h` gained `vectorMethodInvalidatesReferences(methodName)` (true
only for `push_back` this slice — the extension point for `insert`/`erase`/`resize`
once they exist), and `DanglingChecker::invalidateContainerObservers` — called from
`applyCallArgMoves`, the single place every call expression already passes through
— checks whether a call is `<container>.push_back(...)` on a known vector-typed
local, and if the method is invalidating, walks `observerOf_` for every entry whose
owner is that container and inserts it into `danglingObservers_`.

Because this reuses `danglingObservers_` rather than adding new state, it gets
`mergeBranchStates`' existing conservative union **for free**: an invalidating call
inside only one arm of an `if` still makes an observer dangling after the merge,
with zero changes to `BranchState` or the merge logic itself.
`vector_element_observer_branch_merge_rejected.clg` proves this the same way
`branch_merge_ambiguous_rejected.clg` proved it for plain reassignment in Step 11 —
by construction, not by adding a special case.

### Codegen needed almost nothing new

`AstEmitter` lowers `v[i]` to literal C++ `v[i]` — `std::vector::operator[]` is
exactly what the subscript means, no translation required. The one real addition
was extending `exprObjectClass` (the function deciding `.` vs `->` for member
access on an arbitrary expression) with a `SubscriptExpression` case, so
`v[i].field` correctly lowers to `v[i]->field` when the vector's element type is
class-typed (and therefore a smart pointer under the hood) — found immediately
after the first build by manual inspection of generated output (`v[0].v` was
emitting `.` instead of `->`), fixed the same way `MemberAccess`'s own
`exprObjectClass` case already worked.

`ref Node n = v[i];` needed **zero** new emission logic: it composes with the
existing `ref`-to-raw-pointer lowering unchanged (`Node* n = (v[0]).get();`), since
`emitExpression` on a `SubscriptExpression` already produces a valid
smart-pointer-typed expression to call `.get()` on. This is the confirmation signal
the plan called for — if it hadn't composed cleanly, that would have meant the
container-observer design was wrong, not that a parallel emission path was needed.

## Step 15 — Container-held observers were already grammatically banned; the builder just didn't know it

The three remaining Phase 2.6 follow-up items (container-held-observer ban,
`std::string`, `std::map<K,V>`) turned out to be worth doing in that order, because
the first one revealed something worth knowing before touching the other two: a
construct can be grammatically impossible and still silently compile, if the AST
builder captures a subtree's text without checking for embedded parse errors.

`std::vector<ref Node>` was assumed, going in, to need a grammar-level or
checker-level ban. Direct inspection of `grammar.js` showed neither was necessary —
`generic_type`'s type argument is `$.type`, and `observer_qualifier` (`ref`) is only
ever reachable from `variable_declaration` and `parameter_list`, never from
`$.type` itself. So `std::vector<ref Node>` was *already* unparseable as valid
syntax. Testing it anyway (rather than trusting the grammar-reading alone) surfaced
the real bug: `cling` exited 0 and silently emitted `std::vector<ref Node> v;`
verbatim into the generated C++.

The cause was the exact "misparse, not rejection" failure class Step 7 first
identified for `reinterpret_cast`: tree-sitter's GLR error recovery wraps the
misplaced `ref` as an `ERROR`-tagged node **nested inside** `generic_type` (not a
sibling of the whole `container_declaration`), and `buildContainerDeclaration` was
calling `getNodeText` on the whole `generic_type` node — a raw source-text slice
that includes ERROR-node text without complaint, since `getNodeText` has no opinion
about parse-tree well-formedness. Nothing downstream ever checked the subtree for
embedded errors.

The fix is `ts_node_has_error`, a tree-sitter API that recursively checks a node
and every descendant for ERROR/missing nodes — the first use of it in this
codebase. Every prior ERROR-detection site (`buildMemberVariableDeclaration`'s
`ref`-field check, Step 10; the various `rejectUnbuildableChild` call sites) checks
only **direct** children, because in those cases the ERROR node genuinely is a
direct sibling. Here it isn't — `ref` is swallowed one level down, inside
`generic_type`'s own children — so a sibling-count check would have missed it
silently, same as `getNodeText` did. `ts_node_has_error` was the right tool
specifically because it doesn't care how deep the error is nested.

`tests/safety/container_held_observer_rejected.clg` pins this down: it's tagged
`//@ expect: reject` for a *parse-level* rejection, not a semantic one — there is
no dedicated checker rule for container-held observers, because after this fix
there's nothing for a checker to see. The syntax itself doesn't exist.

## Step 16 — `std::string`, and a bug it was never possible to see before

`string` had been a token in `grammar.js`'s `type` rule since early in the
project — alongside `int`/`float`/`double`/`bool` — but nothing downstream ever
gave it special treatment. It flowed through exactly like a primitive: never
registered in `DanglingChecker`'s `isObjectType` (correctly — a bare `string`
local is a C++ value type with cheap real copies everywhere, not a container with
a checkable "did you still have the old one" question), and never mapped to
`std::string` anywhere in `AstEmitter`.

That second part was the actual gap: `AstEmitter::cppTypeFor` returned the
CeeLing type string verbatim whenever it wasn't a recognized vector shape, so a
field or return type declared `string` emitted literal `string` in the generated
C++ — not valid C++ without a `using namespace std;` this codebase deliberately
never emits. Testing two of the repo's own `examples/*.clg` files (written before
this fix, presumably aspirationally, since they were never wired into CTest)
confirmed this wasn't hypothetical: `rule_b_shared_resources.clg`'s `string
filename;` field failed to compile with `'string' does not name a type`. The fix
is threading every remaining `cppTypeFor` gap (member fields, function/method
return types — parameters already routed through it) through the same function,
plus one addition to `cppTypeFor` itself: map `string`/`std::string` to
`std::string`, checked first before the vector-element logic.

The method allow-list (`isKnownStringMethod`: `length`/`size`/`empty`/`substr`/
`find`/`append`/`clear`) deliberately excludes `.c_str()` and `.data()` — both
extract a raw `const char*`, the same shape of hole Step 7's `.get()` ban closes
for smart pointers (#14): once raw pointers are otherwise forbidden, a method that
hands one out is a back door regardless of what type it's hanging off of. This is
the first time the `.get()` reasoning has been applied to something other than a
smart pointer, and it generalizes cleanly — the escape-hatch shape ("extracts a
raw pointer from a safe wrapper") is what matters, not which wrapper.

## Step 17 — `std::map<K,V>`, and reusing `target` instead of special-casing it

`std::map<K,V>` needed two real grammar changes neither prior container slice
needed: a second type argument (`generic_type` was hard-restricted to one — the
grammar comment said so explicitly), and a subscript usable as an **assignment
target**, not just an rvalue (`m[key] = value;` is how a `std::map` is actually
built up; Step 14's `subscript_expression` only ever appeared on the right-hand
side). Both changes were narrow and resolved with zero grammar conflicts:
`optional(seq(',', $.type))` after the first type argument (commas are already
unambiguous inside `parameter_list`/`argument_list`, neither of which nests inside
`<...>`), and adding `$.subscript_expression` to `assignment_statement`'s left-hand
`choice(...)` (the same node, just newly legal in a second grammatical position,
not a new production to disambiguate against anything).

**Scope, confirmed before writing code**: keys and values are restricted to
primitive types this slice (`int`/`float`/`double`/`bool`/`string`). This
sidesteps, rather than re-solves, the shared_ptr-vs-unique_ptr-per-element
decision Step 13 needed a whole two-pass scanning mechanism to answer for
`std::vector<Node>` — primitives are always plain-copy values, so there's no
ownership question to answer for a map's entries at all. The rejection of
non-primitive key/value types lives in `ast_builder.cpp`'s
`buildContainerDeclaration`, reusing the exact `ts_node_has_error`-adjacent style
Step 15 established: a clear, targeted diagnostic at the point where the
restriction is violated, not a silent mishandling.

**The one open design question going in** — should a `std::map` variable itself
participate in move-tracking the way `std::vector` does, or stay untracked like
`string`? — was resolved by asking, not guessing: confirmed with the user that
`isObjectType` should recognize `isMapType` too, on the reasoning that "did you
still have access to the old one after passing it to a function" is a real,
checkable question for any container, independent of what's stored inside it.
`string` is different in kind, not just degree: it was never a container in the
first place.

The pleasant surprise was how little `DanglingChecker` needed to change for
`m[key] = value;` itself. `AssignmentStatement::target` (the field that already
holds a `MemberAccess` for `a.b = ...;`) was never actually typed as
`MemberAccess` — it's a plain `ExpressionPtr`, and the checker's own branch on it
(dangling_checker.cpp, `processStatement`'s `AssignmentStatement` case) already
keyed off "is `target` set at all," not "is `target` specifically a
`MemberAccess`." A `SubscriptExpression` target falls into that same branch and
gets exactly the right semantics for free: assigning into a map links/shares into
the receiver, it does not reassign or move the container `m` itself — the same
rule that was already correct for `a.b = obj;`. The one real gap was in
`AstEmitter`, which HAD hardcoded a `MemberAccess`-specific check (`memberTarget`)
that a `SubscriptExpression` target fell through past, landing in branches that
assumed a scalar/object local rather than a container — fixed with a parallel
`subscriptTarget` case, simpler than the member case since map values are
guaranteed primitive this slice (a plain copy, no move/factory/weak_ptr logic).

## Step 18 — `throw`/`catch`: less new machinery than expected, and a runtime-only bug

TODO.md had already pre-scoped this from an earlier planning pass: model `throw`
as an early-exit triggering the same scope-exit death events as `return`. Reading
the actual `if`/`while`/`for` code before implementing anything overturned part of
that framing: block-exit (`endBlockScope`) already runs **unconditionally** after
every body statement list, regardless of whether a `return` appeared inside it —
`ReturnStatement`'s own handling in `processStatement` does nothing beyond
checking its expression's uses. There was never a "return triggers scope-exit"
mechanism to begin with; block-exit was always going to happen once the walk
finished that block, return or no return. So `throw` needed exactly the same
trivial treatment `return` gets — no early-exit concept had to be invented at all.

**Scope, confirmed before writing code**: `throw` is restricted to `string`
operands only. CeeLing has no inheritance/polymorphism, so a
`throw ParseError(...)`-shaped exception-class hierarchy has no answer for two
real questions — what does throwing an object move/copy/share, and what would
`catch` even match against without a type hierarchy — that don't arise for a
plain message. `catch (string msg)` is accordingly the only catch shape the
grammar's `try_statement` needs to support meaningfully, though the grammar itself
stays general (`$.type $.identifier`, not the literal token `string`) the same way
`ContainerDeclaration::type` stays a general string even though this slice only
validates a narrow subset of it — `DanglingChecker` enforces the restriction, not
the grammar.

`try`/`catch` needed exactly one genuinely new idea, built entirely from existing
primitives with zero new `BranchState` fields or merge rules. A try body can throw
at *any point* inside it, not just succeed-or-fail as a whole — modeled the same
way Step 11's `while` models "body runs zero or more times": merge `before` (try
body never ran) against `tryState` (try body ran to completion) via the existing
`mergeBranchStates`, which produces exactly the right conservative "could have
thrown after any prefix of the try body" state. The catch block is then analyzed
starting from *that* merged state, not from `before` directly — because the
checker genuinely doesn't know how much of the try body executed before a
hypothetical throw. The state after the whole construct is a second, ordinary
two-way merge (try-succeeded vs. catch-ran), shaped exactly like `if`/`else`.
`try_catch_scope_exit_dangles_rejected.clg` proves the first merge does real work:
an observer's owner is reassigned partway through a try body that then throws, and
a use after the `try`/`catch` is correctly rejected on the conservative
assumption that the reassignment might have already happened.

`throw`'s string-only enforcement needed one small extension to existing state
rather than new state: `localType_` (already populated for object-typed
locals/parameters) now also records `string`-typed locals/parameters — they still
never enter `isObjectType`/move-tracking, but their declared type is now
look-up-able, which is all `ThrowStatement`'s check needs (a string literal is
always fine; an identifier must resolve via `localType_` to `string`; anything
else is rejected).

**The bug that would have shipped without running the fixture, not just compiling
it**: `throw "bad thing";` throws C++ `const char*`, not `std::string` — string
literals never implicitly become `std::string` just because they're in a `throw`
statement. `catch (const std::string&)` does not match a thrown `const char*` at
all (C++ exception matching is exact-type, with no implicit conversions), so the
first version of this feature *compiled cleanly* and then called
`std::terminate()` the moment it actually ran, since nothing catches a `const
char*` when only a `std::string` handler exists. This was only caught because the
verification step for this fixture specifically ran the compiled binary, not just
`g++ -fsyntax-only` — a reminder that "compiles" and "does what the source says"
are different claims, and a checker/emitter pair proving memory safety doesn't
automatically prove exception-matching correctness. Fixed by having `AstEmitter`
force-wrap thrown string literals in `std::string(...)`, so the thrown type always
matches the fixed `const std::string&` catch-clause type it also generates.

## Step 19 — `a.attach(b)` shouldn't consume `b`: the parameter-storage summary that turned into three modules

While scoping the next increment after slice 2, direct testing (not a hypothetical)
found that CeeLing could not express the single most common object-graph
construction pattern:

```
class Node {
public:
    Node link;
    void attach(Node other) { link = other; }
};
...
a.attach(b);
b.attach(a);   // ERROR: b was moved into a.attach(b)
```

`a.link = b;` (a direct field store at the call site) was already correctly a
link/share, not a move — the existing `AssignmentStatement` `target`-is-set path
in `dangling_checker.cpp` handled that from Phase 2's earliest version. Going
through a method didn't get the same treatment, because CLAUDE.md's component-A
summary — "which parameters are borrowed, which are moved" — had only ever been
implemented for the RETURN half (Step 8's `returnBorrowsParam`); every parameter
had been unconditionally-moved since the checker's first version, on the
reasonable-at-the-time assumption that banning `ref` parameters meant there was
nothing else to summarize. That assumption held right up until a method's body
could itself perform the exact same store-into-field CLAUDE.md already treats as
non-consuming at the call site.

### A prerequisite bug hiding inside the summary's own precondition

Before any summary could even be *derived*, a narrower bug had to be found and
fixed: inside `attach`'s own body, `link = other;` is a bare-identifier-LHS
`AssignmentStatement` — the grammar only sets `AssignmentStatement::target` for a
`member_access`/`subscript_expression` LHS, and `link = other;` with implicit
`this` parses as a plain `identifier` LHS, exactly like an ordinary local variable
assignment. `DanglingChecker` had no notion at all of "is this bare name a field
of the current class" — no `classFields_`-equivalent map, unlike
`EscapeHatchChecker`, which had built exactly this (`map<className,
map<fieldName, fieldType>>`) since Phase 1. So `link = other;` fell into the
"reassign a local" branch, and that branch's own rule — "the RHS being another
object local moves that source" — marked `other` moved-from immediately, *inside*
`attach`'s own body, before any caller or summary logic was ever involved. A
`std::cout << other.v` two lines later inside `attach` itself already
false-rejected, with no method call anywhere in the picture. Fixed with a new
`classFields_` member (built the same way `collectClassNames` already built
`classNames_`) and a `isBareFieldStore` check ahead of the existing
"reassignment vs. rebind vs. field-store" branch in `processStatement` — a bare
identifier naming a current-class field now gets exactly the same treatment a
`MemberAccess` target already got, with a local/observer of the same name (an
ordinary shadowing case) still taking precedence.

### Why this couldn't stay checker-only: `unique_ptr` really can't be copied

The natural next step — add a `paramsStoredInField` summary next to
`returnBorrowsParam`, consult it in `applyCallArgMoves` — looked sufficient right
up until the actual generated C++ was read. Before this step, `attach` compiled
to `void attach(std::unique_ptr<Node> other) { this->link = std::move(other); }`,
and the call site emitted `a->attach(std::move(b));`. That's not a checker
artifact to relax — it's a real move, and a `unique_ptr` genuinely cannot be
copied (its copy constructor is deleted). So "`b` doesn't get consumed" can only
become true in the generated code if `b` (and `attach`'s own parameter `other`)
actually become `shared_ptr<Node>` — which meant the real work was in
`SmartPointerAnalyzer`, not `DanglingChecker`.

Reading `SmartPointerAnalyzer` closely surfaced two more findings that reshaped
the plan mid-investigation:

- `registerParameters`'s own comment claimed a parameter stored into a field
  "still gets promoted to SHARED via `scanSource`'s independent regex pass,
  which already covers that case today." Empirically false for this exact
  case: `scanSource`'s member-promotion rule
  (`secondPassDeterminePointerTypes`, the `containingScope == className` check)
  only fires for a class's OWN field entries, where the field's declaring scope
  literally equals its own class name (`link`'s entry: scope "Node" == class
  "Node"). A *parameter's* entry is scoped to `"Node::attach"`, which is never
  equal to any class name — so this rule silently never fired for parameters at
  all, and the comment's claim, true once, had gone stale as the codebase grew.
- `analyzeStatement` (the AST-driven pass) has no case for `FunctionCall`,
  `ExpressionStatement`, `IfStatement`, `WhileStatement`, or `ForStatement` — call
  sites and control flow are completely invisible to it. This wasn't a small gap
  to patch; it meant "propagate a promoted parameter's SHARED-ness to its
  caller's argument" needed an entirely new kind of walk this analyzer had never
  done before.

### A third, self-contained pass — mirroring `scanContainerElementKinds`'s own precedent

Rather than extend the tuned, delicate two-pass model (`firstPassCollectReferences`
+ `secondPassDeterminePointerTypes`) and risk its existing back-reference/
bidirectional-assignment heuristics, the fix follows Step 13's own established
pattern: a new, self-contained third pass, own data, own recursive walk, running
strictly after the existing two-pass decision (because it needs to *read* that
decision — a field only propagates its SHARED-ness to whatever's stored into it
if the field itself is already known to be SHARED).

Two sub-passes, run in order:

1. **`promoteParamsStoredInSharedFields`** walks every method/constructor body's
   TOP-LEVEL statements (deliberately not recursing into `if`/`while`/`for` — see
   below), looking for `field = param;` where `field` resolves (via
   `symbolTypesByScope`, populated when the class body was first walked) to an
   already-SHARED type, and `param` is exactly one of that body's own
   parameters. When found, it writes directly into `pointerDecisions[key] =
   SHARED` for the parameter's own `VariableReference` entry — the same map
   `secondPassDeterminePointerTypes` already populates, just written to a second
   time by a different pass, which is safe because `getPointerType` just reads
   whatever's there whenever it's asked, with no ordering assumption baked in
   beyond "after the two-pass decision."
2. **`promoteSharedCallArguments`** does the walk `analyzeStatement` never did:
   a full recursive statement/expression traversal (local lambdas in the `.cpp`,
   not new member functions, since nothing else in the class needs this shape of
   walk) over every function/method/constructor body, resolving each
   `FunctionCall`/`NewExpression` argument to the parameter it's bound to (via a
   small, transient `CalleeParams` lookup built once per call, not permanent
   state) and promoting the ARGUMENT's own entry to SHARED whenever the
   parameter it feeds was promoted in sub-pass 1.

**Explicitly not scoped, and documented as a real (not silent) limitation**: no
recursion into conditional bodies for sub-pass 1 (`if (x) { link = other; }`
doesn't promote `other` — promoting a parameter whose storage only happens on
some paths would incorrectly promise sharing on paths where the store never
runs), and no fixed-point iteration across multi-hop chains (`a.wrap(b);
c.wrap(a);` where `wrap`'s field only becomes SHARED after seeing `a` promoted
would need a second round to catch `c`'s argument). Both default to the existing
safe UNIQUE behavior — over-restrictive, never unsafe, the same boundary-drawing
principle used for slice 2's invalidation-summary scope and Step 17's map
primitive-only restriction.

### The blanket rule that made the checker/emitter side almost disappear

The obvious next step — add a `paramsStoredInField` summary to
`DanglingChecker::FunctionSummary`, consult it in `applyCallArgMoves`, do the
matching lookup in `AstEmitter` — turned out to be unnecessary once the
`SmartPointerAnalyzer` promotion was actually correct. The load-bearing
observation: `std::shared_ptr` is *always* safely copyable, unconditionally, for
any call whatsoever. So the question "does this call consume this argument"
reduces entirely to "is the argument's own already-decided pointer type SHARED,"
with **no need to separately resolve which callee it's going into or what that
callee's summary says** — the promotion work above already guarantees that if an
argument's own type is SHARED, some prior analysis has already established it's
safe to copy anywhere. This generalizes the existing `push_back`-specific
`vectorMethodSharesArg`/`elementIsShared && argIsShared` rule (Step 13) from "only
this one hardcoded library method shares" to "any call, if the argument is
already SHARED, shares" — one new guard in `applyCallArgMoves`'s per-argument
loop, and a matching one in `AstEmitter::emitMovedArg` (emit a plain copy instead
of `std::move(id)` when the argument's own `getPointerType` resolution is
SHARED), both far smaller than a per-callee summary mechanism would have been.

**A real regression this generalization introduced, caught by the fixture
sweep, not anticipated in the plan**: `SmartPointerAnalyzer::getPointerType`'s
suffix-match has no entry at all for a container type like `"std::vector<Node>"`
(`pointerDecisions` is only ever keyed by genuine class names), and its
not-found fallback unconditionally defaults to `PointerType::SHARED`. Applying
the new blanket "SHARED argument never moves" rule without excluding container
types made `vector_moved_into_call_rejected.clg` (`sink(v); v.size();`) flip
from PASS to XFAIL — `v`'s type, `"std::vector<Node>"`, isn't a registered class
name, so `getPointerType` fell through to its SHARED default and the new rule
incorrectly treated the vector as never needing to move. Fixed by restricting
the blanket rule to arguments whose declared type is a genuine `classNames_`
entry (`classNames_.count(argType) > 0`) before consulting `getPointerType` at
all — the vector-specific share logic (which already correctly handles a
container's own move-vs-share question through `vectorMethodSharesArg`) is
untouched and remains the only path for container arguments.

### A pre-existing, unrelated bug found and deliberately left alone

Writing the constructor-storage fixture surfaced a genuine, pre-existing bug with
nothing to do with this step: a class with more than one constructor, or a single
constructor with more than one parameter, has its later parameters silently
dropped from the emitted C++ signature (confirmed on a stock two-parameter
constructor with none of this step's changes applied, via `git stash`) — the
parameter appears used in the constructor body (`this->w = other;`) but never
declared in the signature, a guaranteed compile error. Out of scope for this
step; the constructor-storage fixture
(`param_stored_in_field_constructor_accepted.clg`) works around it by using a
single-parameter constructor whose sole parameter is exactly the field being
linked to, rather than fixing constructor-overload/multi-parameter emission.

### Runtime proof, not just type-checking

Given the previous step's lesson about compiling-but-not-matching, the primary
fixture (`param_stored_in_field_accepted.clg`) is compiled AND actually run, not
just checked with `-fsyntax-only`: `a.attach(b); b.attach(a); std::cout <<
b.v;` must print `2`, not crash. It does — confirming all three modules (checker,
pointer-type inference, emitter) actually agree at runtime, not just that each
one's isolated logic looks locally correct.

## Step 20 — Inheritance: flattening beats chain-walking, and two things users can't write

CeeLing had no inheritance at all: no base clause, no `virtual`/`override`, no
`ClassDeclaration::baseClass`. TODO.md had carried a designed-but-unimplemented
entry for it since early on. Without it most real class hierarchies are
inexpressible — a hard blocker for the YAML-parser milestone (a YAML node is
naturally a polymorphic scalar/sequence/mapping hierarchy) and for the language
being credible as "a safe subset of C++."

### Measuring the blast radius first

The decisive question wasn't "how do I parse a base clause" — it was "what breaks
when a variable's class is no longer exactly its declared type." Counting the
answer before designing anything: **~40 exact-class-match lookup sites** across four
modules (`DanglingChecker` 4, `EscapeHatchChecker` 13, `AstEmitter` 7,
`SmartPointerAnalyzer` 16). Every one asks "does class X have field/method Y" or
"is X a known class," and every one silently becomes wrong under inheritance.

Critically, the two failure modes are **not equally bad**, and noticing that shaped
the priority:

- `EscapeHatchChecker::isKnownCallable` fails **safe** — an inherited method isn't
  found in the flat map, so `d.baseMethod()` is rejected by the
  default-reject-unknown-calls rule. Annoying (valid code refused), not dangerous.
- `AstEmitter::fieldType` fails **silently wrong** — an inherited field returns `""`,
  so `exprObjectClass` concludes "not a class type" and emits `.` instead of `->` on
  what is actually a smart pointer. A compile error at best; wrong code at worst.
  This is the one that had to be right.

### The structural finding that picked the design

Reading all four collection passes side by side surfaced the thing that made this
tractable: they are **identical in shape**. Every module builds its own private,
*flat* `className -> members` map in a single pre-pass over `program.statements`
(`EscapeHatchChecker::collectClasses`, `DanglingChecker::collectClassNames`,
`AstEmitter::collectClassNames`, `SmartPointerAnalyzer::firstPassCollectReferences`).

So rather than teach 40 lookup sites to walk a base chain — 40 chances to get it
wrong, in four modules with different conventions — each collection pass instead
visits classes **base-before-derived** and copies a base's already-flattened members
down into its derived classes. Every downstream lookup then works **unchanged, with
zero edits**. A derived member with the same name naturally overwrites the inherited
entry, which is exactly correct shadowing.

`class_hierarchy.h`'s `classesInBaseFirstOrder` provides that ordering once instead
of it being re-derived (and re-bugged) four times. It's a new file rather than an
addition to `library_summary.h` because that header is specifically about *std
library* summaries — an AST-walking helper would muddy its purpose.

`SmartPointerAnalyzer` is the one that doesn't use a flat map: it keys everything by
`(scope, class, variable)`, so "flattening" there means **re-registering** a base's
fields under the derived class's own scope. Without that, an inherited field has no
entry at all and `getPointerType` falls through to its SHARED default — silently
making every inherited field a `shared_ptr` regardless of actual ownership shape.

### Two things the compiler generates because users can't

The most useful discovery of this step: **CeeLing has no destructor syntax at all.**
That reads like a gap; it's an advantage. Because CeeLing generates every destructor
itself, it can unconditionally emit `virtual ~Base() = default;` for any class used
as a base — closing the classic UB of deleting a Derived through a base pointer
**by construction**, with no user-facing syntax and no way to forget it. That's a
genuine memory-safety hole (exactly the kind CLAUDE.md's contract must not have),
not a style nicety. It also removes half of Phase 2.7's virtual-call trap: there are
no destructor bodies to check, only constructor bodies.

The second generated member came from a problem the plan did **not** anticipate,
found only by compiling the first working output: a derived constructor implicitly
calls the base's *no-arg* constructor, but declaring any constructor (e.g.
`Animal(int)`) suppresses C++'s implicit default one. Real C++ solves this with a
base-initializer list (`Dog(int a) : Animal(a)`), which CeeLing has no syntax for —
and deliberately still doesn't: adding one would grow the surface syntax CLAUDE.md
explicitly tries to keep small. Instead the emitter generates a `protected` no-arg
constructor for every base, and a derived constructor simply assigns the inherited
fields directly in its own body (`age = a;` already lowers to `this->age = a;`
thanks to flattening). `protected` keeps it an implementation detail — it exists for
derived classes to chain through, not for users to call.

### Declared type vs. constructed type

`AstEmitter::factoryFor` had always assumed these were the same string, because
before inheritance they always were. `Animal a = new Dog();` breaks that: the object
constructed is a `Dog` (`make_*<Dog>`), but the smart-pointer **kind** must come from
`a`'s own declared type `Animal` — asking the analyzer about `Dog` consults a
different variable's ownership decisions entirely. Getting it backwards emits
`std::unique_ptr<Animal> a = std::make_shared<Dog>(3);`, which does not compile. The
fix is a fourth `declaredType` parameter defaulting to empty (meaning "same as
className"), so every non-inheriting call site is untouched.

### Runtime proof, again

Both claims this step makes are runtime properties that `-fsyntax-only` cannot
check, so both fixtures are **executed**:
`inherit_virtual_dispatch_accepted.clg` must print `woof`, not `generic` — code that
compiles but calls the base version would pass a compile-only check while being
exactly the bug the feature exists to prevent. And
`inherit_virtual_destructor_accepted.clg` is both run and grepped for
`virtual ~Shape() = default;` in the emitted output, since the safety claim there is
about what was generated, not merely that the program was accepted. This is the same
discipline Step 18's `throw`-matching bug and Step 19 established.

A final check worth doing explicitly: a non-inheriting program's emitted C++ contains
**zero** occurrences of `virtual` or `: public`. Everything here is additive.

## Step 21 — `enum`, `namespace`, and `.as<T>()`: the three features a real yaml-cling port needed

A companion project (`yaml-cling`) had prior work written against an earlier,
reference-based CeeLing architecture that already had `namespace`/`enum`/
templates/operator overloading. That architecture and this branch's
provenance-table dangling checker are genuinely different designs — the old files
were kept only as a shape-of-the-target reference, not ported as code. Restarting
`yaml-cling` on this branch meant closing the specific gap: yaml-cpp-shaped code
needs `enum` (node kind tags), `namespace` (the `YAML::` prefix), and
`node.as<T>()` (a scalar's text parsed differently depending on the caller's
requested return type — `stoi` for `int`, `stod` for `double`, identity for
`string`). All three landed together, in that order, each reusing a pattern the
inheritance work (Step 20) had already established rather than inventing new
mechanisms.

### `enum`: the cheapest possible feature, because C++'s unscoped enum already IS an int

Deliberately **unscoped**, not `enum class`: yaml-cling-shaped code stores a node's
kind tag in a plain `int` field and compares it against enumerators directly
(`kind == NodeType::Scalar`), which needs the implicit enumerator→int conversion
only an unscoped enum gives for free. This is why the feature needed **zero**
changes to `DanglingChecker`'s move/observer tracking: an enum type name matches
none of `classNames_`, `isVectorType`, or `isMapType`, so every existing "is this
an object type" check already falls through to treating it like `int`/`bool` — the
correct behavior — without a single new branch anywhere in the checker.

The one real bug this surfaced was in a fixture, not the compiler:
`enum_basic_accepted.clg`'s first draft asserted `//@ run: 2` for `Scalar` in
`enum Kind { Null, Scalar, Sequence };`, but `Scalar` is enumerator index 1, not 2.
The compile gate's run tier (Step 4.5) caught the mismatch immediately
(`FAIL (ran, expected '2', got '1')`) — the compiler was right, the test was
wrong. Small, but a real validation of why the run tier exists: a `-fsyntax-only`
check alone would have let a fixture asserting the wrong fact pass forever.

### `namespace`: desugar to name-qualification, reuse inheritance's flattening pattern

Two designs were on the table: give `AstEmitter` a real nested-namespace-aware
emission path, or desugar `namespace X { class C {...} }` at PARSE time into a
top-level `ClassDeclaration` tagged `namespaceName = "X"`, then have
`AstEmitter` re-wrap it in an actual `namespace X { ... }` block on the way out.
Desugaring won: it means exactly one module (the AST builder) understands
namespaces exist at all; every other module — `DanglingChecker`,
`EscapeHatchChecker`, `SmartPointerAnalyzer` — sees an ordinary, if
oddly-named, top-level class. This is the same "solve it once at the edge, leave
the middle unchanged" instinct behind Step 20's member flattening.

`buildNamespaceDeclaration` walks the namespace body and tags each
`ClassDeclaration`/`EnumDeclaration` it finds with the enclosing namespace's name;
a nested `namespace` inside a `namespace` is rejected outright with a targeted
diagnostic rather than silently flattened or accepted (`namespace_nested_rejected.
clg`) — nesting was never a requirement and silently getting it wrong (e.g.
losing a qualifier level) would be worse than refusing it.

**The actual work was DUAL-REGISTRATION, and it touched four modules because each
one keeps separate, independently-keyed state.** A namespaced class must be findable
under both its bare name (`"Node"`, for lookups inside its own namespace's other
members) and its fully-qualified name (`"YAML::Node"`, for lookups at every external
use site) — so `collectClasses`/`collectCallables`/`collectClassNames`/
`analyzeClassDeclaration` in `EscapeHatchChecker`, `DanglingChecker`, `AstEmitter`,
and `SmartPointerAnalyzer` respectively each got one extra line inserting the
qualified-name entry alongside the existing bare-name one. No downstream lookup
site needed to change — same shape of win Step 20's flattening produced, just
dual-keying instead of copying-down-the-hierarchy.

A companion grammar fix fell out of testing this, not the original plan:
`new_expression` only accepted a bare `identifier` for the type being constructed,
so `new YAML::Node(...)` didn't parse at all. Extended to `choice($.identifier,
$.scoped_identifier)`; `getNodeText` already captures the full qualified text
verbatim, so no AST builder change was needed once the grammar accepted it.

**The one genuinely subtle bug, found through direct debug instrumentation, not
inspection:** a namespaced local correctly got `shared_ptr` instead of the
correctly-inferred `unique_ptr`. The instinct was to blame `SmartPointerAnalyzer`
for being "namespace-unaware" and strip the `YAML::` qualifier in `AstEmitter::
smartPointerType` before querying it. That was backwards. Once
`SmartPointerAnalyzer::analyzeClassDeclaration` was given the same dual-registration
treatment (a prerequisite for move-tracking a namespaced local to work at all), it
started registering a namespaced local's OWN pointer-kind decision under the
QUALIFIED name — because `decl->type` for `YAML::Node a;` literally contains the
string `"YAML::Node"`, and that's the key `SmartPointerAnalyzer` naturally uses.
Stripping the qualifier on the query side therefore looked up a key that was never
written, silently fell through `getPointerType`'s not-found fallback (which
defaults to `SHARED` — the same fallback behavior Step 19's verification sweep had
already found and worked around once for vectors), and produced the wrong pointer
kind with no error, no warning, nothing. Confirmed via temporary `getenv`-gated
debug printing of candidate `pointerDecisions` keys, comparing the registered key
(`"main|YAML::Node.a"`) against the (wrong, post-stripping) queried key
(`"...|Node.a"`). The fix was to delete the stripping entirely — pass `className`
through to `getPointerType` exactly as spelled at the call site, matching however
registration actually happened, never re-normalize on the query side.

### `.as<T>()`: the risk was one grammar ambiguity; the mechanism was one string key

Scope was deliberately narrow, confirmed with the project owner before starting:
only the exact shape yaml-cpp needs — a primary generic `<T> T method() {...}`
(one type parameter, return type must equal it) plus concrete specializations
`ConcreteType method<ConcreteType>() {...}`, called as `n.as<int>()`. Not generic
free functions, not parameter-type genericity, not multiple type parameters —
those stay filed under "light generics" in TODO.md as a separate, larger,
not-yet-scoped feature.

**The only place this was genuinely risky was grammar, and only in expression
position.** `x.as<int>()` and an ordinary comparison chain like `a < b` are both
grammatically legal starting from the exact same prefix — `identifier '<'` or
`member_access '<'` — at the same `$.expression` position. This is categorically
different from `generic_type`'s `vector<T>` case (Step 13), which only ever
appears in `$.type` position and so could be resolved with a numeric `prec()`;
here BOTH interpretations are live at the SAME position, which is exactly the
shape `container_declaration` vs `variable_declaration` needed a real `conflicts:`
entry for, not a precedence tiebreak. `template_call`'s completion shape (a
`type`, then `>`, then `(`) never coincides with an ordinary comparison's
shape, so GLR always resolves the ambiguity correctly once told to explore both —
but "told to explore both" took two iterations: the first `generate-grammar`
attempt named `primary_expression` vs `template_call` as unresolved; adding that
pair to `conflicts:` surfaced a second, related pair, `expression` vs
`template_call` (from the `member_access` variant of the callee). Both added,
zero conflicts remained. Tree-sitter's own conflict reporter named exactly what
was needed each time — no guessing required, just iterating on its output.

**Once the grammar risk was resolved, the checker/emitter mechanism turned out to
be almost nothing new.** A specialization's BODY has zero genericity in it once
you know which body belongs to which call — `int as<int>() { return rawValue; }`
is an entirely ordinary method. So the only new mechanism needed was a shared
naming convention for a lookup key, not a new data structure:
`specializationKey(methodName, typeArg)` → `"as<int>"` (`class_hierarchy.h`,
next to `classesInBaseFirstOrder`, the same "one shared helper instead of four
reimplementations" instinct). A specialization registers under this synthetic key
in the exact same `classMethods_`/`classMethodParams_`-shaped maps every module
already had from inheritance/enum/namespace work; a call carrying a type argument
(`FunctionCall::templateArg`) constructs the identical key at every existing
lookup site — `EscapeHatchChecker::isKnownCallable`, `DanglingChecker::
resolveCallReturnAlias`, `AstEmitter::calleeParam` — instead of the bare method
name, only when `templateArg` is non-empty. Every one of those lookup sites
already existed; this was a keying change at each, not new logic.

**Emission needed one real new shape**, because C++ itself requires it: the
primary emits `template<typename T> T as() {...}` INSIDE the class body (an
ordinary method position), but each specialization must emit OUTSIDE the class
body, after the closing `};`, as `template<> int Node::as<int>() {...}` — the C++
rule for explicit member-template specializations. `emitClass` collects
specializations during its normal member-emission loop instead of emitting them
inline, then emits each in a second pass once the class body closes. The call
site itself emits as literal `n->as<int>()` — CeeLing does no dispatch of its own
at the call site; real C++ template resolution picks the right specialization at
compile time, and CeeLing's `specializationKey` lookups are a separate,
analysis-only mechanism that must agree in spirit but never needs to appear in
the emitted text.

One easy-to-miss detail cost a round of debugging output inspection before the
fix was obvious: the `<int>`/`<string>` text has to go through the same
`cppTypeFor` mapping every other type does (`string` → `std::string`) — both in
the out-of-class qualified name and at the call site. Using the raw CeeLing
token (`Node::as<string>` instead of `Node::as<std::string>`) doesn't error; it
silently defines a second, unrelated template instantiation that the real
`n->as<std::string>()` call site never binds to, so the specialization's body
would never run and the primary's `throw` would fire instead — a bug that only
`-fsyntax-only` cannot catch, and that the `.as<T>()` fixture's `//@ run:` tier
was specifically added to guard against for exactly this reason.

### Runtime proof, again

`template_as_basic_accepted.clg` is **run**, not just compiled: it asserts
`42 converted`, the concrete output of both specializations actually being
selected and executed by real C++ template resolution. A compile-only check would
have passed even with the `cppTypeFor` bug above (raw `string` token) still in
place — the code compiles fine, it just quietly runs the wrong body — so this is
the same "the claim is a runtime property" reasoning Step 18 and Step 20 already
established, applied here because template specialization dispatch is exactly
that kind of claim.

A final regression check worth calling out explicitly, since it's what the whole
grammar-conflicts investigation was protecting against:
`template_comparison_unaffected_accepted.clg` asserts `1 0` for `a < b` and
`x.length() < 5` — both still parse and evaluate as ordinary comparisons, not
misparsed as an incomplete `template_call`. Full CTest (13/13) and safety-suite
(58/58, compile-gated) sweeps after this step showed zero regressions elsewhere.
