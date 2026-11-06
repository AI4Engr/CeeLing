> **History, not documentation.** This is the development log of the compiler, written while it was being built; parts of it are out of date. The current description of the language is the
> [Language Guide](../language-guide/README.md); the authoritative status is the design notes and the internal notes.

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

The design notes' component A calls for per-function borrow summaries: which parameters are
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
  same "ambiguous → conservative reject" principle the design notes state for the analysis
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

The internal notes had already pre-scoped this from an earlier planning pass: model `throw`
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
through a method didn't get the same treatment, because the design notes' component-A
summary — "which parameters are borrowed, which are moved" — had only ever been
implemented for the RETURN half (Step 8's `returnBorrowsParam`); every parameter
had been unconditionally-moved since the checker's first version, on the
reasonable-at-the-time assumption that banning `ref` parameters meant there was
nothing else to summarize. That assumption held right up until a method's body
could itself perform the exact same store-into-field the design notes already treats as
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
`ClassDeclaration::baseClass`. The internal notes had carried a designed-but-unimplemented
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
genuine memory-safety hole (exactly the kind the design notes' contract must not have),
not a style nicety. It also removes half of Phase 2.7's virtual-call trap: there are
no destructor bodies to check, only constructor bodies.

The second generated member came from a problem the plan did **not** anticipate,
found only by compiling the first working output: a derived constructor implicitly
calls the base's *no-arg* constructor, but declaring any constructor (e.g.
`Animal(int)`) suppresses C++'s implicit default one. Real C++ solves this with a
base-initializer list (`Dog(int a) : Animal(a)`), which CeeLing has no syntax for —
and deliberately still doesn't: adding one would grow the surface syntax the design notes
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
those stay filed under "light generics" in the internal notes as a separate, larger,
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

## Step 22 — Qualified free-function calls, and operator overloading scoped to stop exactly where `ref` returns begin

With `enum`/`namespace`/`.as<T>()` landed, the actual test was attempting to
transpile yaml-cling's real `src-clg/yaml_node.clg` against this branch for the
first time — not a hand-written probe shaped like it, the real file. This
surfaced two concrete, confirmed parse errors (not guesses): `std::stoi(scalar)`
inside a `.as<int>()` specialization, and `Node operator[](string entryKey)
{...}` — the grammar had no way to call a qualified free function or declare an
operator-named method at all.

A third, larger issue was found in the same pass and deliberately NOT
attempted here: `yaml_node.clg`'s `operator[]`/`findOrAppendSibling`/
`siblingAt` all want to return a *reference* into the object graph so
`node["a"]["b"] = "c"` chains — CeeLing has no `ref` RETURN type (only `ref`
locals/observers), and inventing one is a real, separate design problem (what
does a `ref`-returning method's borrow summary even mean — does the return
alias `this`, or a field of `this`? how does escape analysis stop the returned
reference from outliving its receiver?). This step's operator-overloading slice
was scoped, on purpose, to stop exactly at that boundary: VALUE-returning
`operator[]`/`operator=` only.

### Qualified free-function calls were nearly free, because of a precedent already on file

`function_call`'s callee only accepted `identifier | member_access |
subscript_expression`; extending it to `scoped_identifier` is the exact same
shape of fix `new_expression` already got for `new YAML::Node(...)` (Step 21) —
and for the exact same reason it needed almost no follow-up work.
`buildFunctionCall`'s plain-call branch does `getNodeText(calleeNode, source)`
verbatim, so `funcName` becomes `"std::stoi"` with **zero** AST-builder
changes once the grammar accepts it.

The only real gate left was `EscapeHatchChecker::isKnownCallable`'s plain-call
branch (Phase 1's default-reject-unknown-calls rule): `knownFreeFunctions_`
only ever contained CeeLing-defined functions, so `std::stoi` needed a library
allow-list entry — the free-function counterpart to `library_summary.h`'s
existing `isKnownVectorMethod`/`isKnownStringMethod` (which cover *methods*,
not qualified *free functions*). `DanglingChecker` needed **no** changes at
all: `std::stoi`/`std::stod` take a `string` and return a primitive, no
object-typed argument or return value, so `resolveCallReturnAlias`'s existing
graceful `{}` fallback (for names absent from `functionParams_`/`summaries_`,
both CeeLing-only maps) is already exactly correct — there is no aliasing
return to miss. `AstEmitter` needed no changes either: the plain-call emission
path already emits `call->function_name` verbatim, so `"std::stoi"` comes out
correct with no translation.

One thing confirmed directly rather than assumed: `std::cout << x` is a
`BinaryExpression` (the `<<` token), never a `FunctionCall` at all — so it was
never subject to `isKnownCallable` in the first place, and this step's allow-
list addition really is the last remaining gate for plain qualified calls, not
one of several.

### `operator[]`/`operator=` could not be desugared at parse time — the AST builder has no type environment

A first instinct was to desugar `n[key]` into an ordinary method-call AST node
(`n.operator[](key)`) right in the AST builder, the same way namespace
declarations desugar into tagged top-level statements (Step 21). That does not
work here: the AST builder is a pure syntax-directed translation with **no type
environment** at all — `buildSubscriptExpression` has no way to know whether
`n[key]`'s `n` is a `std::map`/`std::vector` (today's only meaning) or a
class-typed local with its own `operator[]` overload. That distinction can only
be resolved later, by each checker/emitter module against its own
`localType_`/`classNames_` map — exactly the place `isVectorType(declaredType)`
already branches today. So `SubscriptExpression` stays ONE generic AST node (no
new node type), and each of the three modules that already inspect it
(`EscapeHatchChecker`, `DanglingChecker`, `AstEmitter`) gained one added
class-vs-container branch. This is the established pattern applied once more,
not a new one.

Grammar-wise, this made the feature simpler than templates' call-site ambiguity
(Step 21): `n[key]` and `n = value` already parsed via the existing
`subscript_expression`/`assignment_statement` productions, so the ONLY new
grammar was letting a method be *named* `operator[]`/`operator=` — a new
`operator_method_name` token (`'operator[]'`/`'operator='` as whole tokens, not
built from separate `[`/`]`/`=` terminals) as an alternative to `$.identifier`
in `method_declaration`'s name slot. Zero new call-site grammar, zero GLR
conflicts.

### Read position reuses ordinary method-call machinery; write position needed real new logic

`x = n[key];` on a class-typed `n` is checked/emitted as if it were
`n.operator[](key)` — same `classMethods_`/`classMethodParams_` registration
under the literal key `"operator[]"`, same `isKnownCallable` lookup shape, same
emission machinery every ordinary method call already has. The only emitter
change was in the read-path: it used to emit `obj[index]` literally for ANY
`SubscriptExpression`, correct for a container but WRONG for a class-typed
`obj` (a `unique_ptr<T>` has no meaningful `operator[]` of its own — confirmed
this fails to compile via a direct probe before fixing it) — now it checks
`exprObjectClass(sub->object.get())` first and emits `obj->operator[](index)`
instead when the receiver is class-typed.

Write position is genuinely two different sub-cases, not one:

- `n[key] = value;` (subscript target): real C++ needs `operator[]` to *return
  a reference* for the subsequent `=` to assign through — exactly the
  capability this step deliberately does not have. Rather than half-support
  this (emit something that silently doesn't compile, or worse, compiles into
  the wrong thing), `EscapeHatchChecker` rejects it outright with a diagnostic
  that names the real limitation, whether or not `operator[]` itself exists on
  the receiver's class.
- `n = value;` (plain assignment, RHS type-mismatched against `n`'s declared
  class — `n = "text";` is the concrete yaml-cpp shape, `node = "b"`): this
  one genuinely doesn't need reference returns, because it isn't chainable —
  it's a plain mutating call on the still-alive object
  (`n->operator=(value);`), not an ownership handoff. `AstEmitter`'s
  assignment-statement branch detects "LHS is class-typed AND has its own
  `operator=` AND the RHS isn't itself the same class" and emits the method
  call instead of ordinary reassignment codegen; `DanglingChecker` got a
  parallel `isOperatorAssignMutation` check so this is never treated as an
  owner-death/rebind event — `n`'s identity doesn't change, so none of the
  move/observer-invalidation machinery should fire for it.

### A real bug, found only by re-testing the actual file, not the fixtures written first

Every fixture written before re-attempting the real `yaml_node.clg` passed. The
re-attempt still found a genuine bug the fixtures had missed: `Node(int
nodeKind) { ...; child = nullptr; next = nullptr; }` — a bare-identifier
assignment to a FIELD (implicit `this`, inside a constructor) — emitted as
`this->child->operator=(nullptr);`, which does not compile (`unique_ptr` has no
such overload). The new operator= detection in `AstEmitter` checked "is the LHS
class-typed with its own `operator=`, and does the RHS not match" — true for
`child`'s declared type `Node` — but never excluded the case where the LHS is
actually a FIELD STORE (`isCurrentField(assign->variable)`), which the codebase
already handles as its own, earlier, unconditional category (the same one `a.b
= x;` uses) precisely because a field store is a link/store, never a
reassignment OR an operator call. Fixed by adding that exclusion.
`DanglingChecker` was never actually vulnerable to this: its own field-store
detection (`isBareFieldStore`) is checked earlier in the same `if`/`else if`
chain, before its `isOperatorAssignMutation` branch is ever reached — but the
bug was real in `AstEmitter`, and a dedicated regression fixture
(`operator_assign_field_store_unaffected_accepted.clg`) now pins the fix,
exercising exactly this shape (a field reset to `nullptr` in a constructor, on
a class that separately declares `operator=`).

This is the same lesson Step 19's constructor-parameter bug and Step 20's
`SmartPointerAnalyzer` fallback bug already taught: **fixtures written before a
real end-to-end attempt only prove the scenarios you already thought of.** The
actual target file is what surfaces the scenario you didn't.

### The honest stopping point

Re-running `yaml_node.clg` after both slices landed: it now parses and reaches
codegen (previously failed immediately at `std::stoi`, before either slice
existed). It still does not COMPILE. `g++` stops at exactly the predicted
place — `return this->child;` / `return this;` inside methods declared to
return `Node`, which lower to `unique_ptr<Node>` (move-only; cannot be
returned as a copy, and `this` cannot be converted to one at all). This is the
deferred reference-return capability, not a new surprise — stated here plainly
rather than claiming the port succeeded. `ref`-returning methods are tracked as
their own, unstarted the internal notes item.

Full CTest (13/13) and safety-suite (65/65, compile-gated) sweeps after this
step showed zero regressions elsewhere.

## Step 23 — Alias-returning methods: closing the reference-return gap without a `ref` return type

Step 22 stopped at a deliberately deferred, larger problem: yaml-cpp's
`Node::operator[]`/`findOrAppendSibling`/`siblingAt` all need to hand back a
*reference* into the object graph so `node["a"]["b"] = "c"` chains meaningfully
— `return this;` / `return this->child;` from a method whose return lowers to
`unique_ptr<T>` (move-only, cannot be returned as a copy, and `this` cannot
convert to one at all) simply does not compile. The the internal notes item this left
behind assumed the fix would be a `ref` RETURN type — a real, separate design
problem (what does a `ref`-returning method's borrow summary even mean; how
does escape analysis stop the returned reference from outliving its
receiver). That turned out to be the wrong shape entirely.

### The actual gap was narrower than "add reference returns"

Re-reading what `operator[]` and its two recursive helpers actually need: none
of them need a genuine C++ reference. They need to hand back something that
*aliases* shared storage — which is exactly what `shared_ptr<T>` already is.
CeeLing's method-return type is a *choice between two smart-pointer kinds*
today (hardcoded to always pick `unique_ptr`), not something requiring a third,
fundamentally different reference mechanism. This reframing is what made the
fix small: extend the EXISTING smart-pointer decision (already made for every
field and local) to methods too, rather than build reference-return machinery
CeeLing has never had.

### A hand-written hint was built first, then explicitly rejected

The first working version used a `//@ptr shared Class.method` comment
annotation (reusing the pre-existing `//@ptr shared Class.member` mechanism
`smart_pointer_analyzer.cpp` already had for fields/locals) — mechanically it
worked end-to-end. It was rejected on sight: the design notes' "ownership is
inferred, not annotated" is a hard design constraint, not a preference, and a
per-method annotation the user has to write for every alias-returning method
is exactly the `owned<>`/`borrowed<>`-style manual bookkeeping the whole
project exists to avoid. The entire hint mechanism (a regex extension to
`parseAnnotationLine`, a `methodReturnHints_` map, a `scanMethodReturnHints`
pass) was torn out and replaced with real inference — but the DOWNSTREAM
consumers (`AstEmitter::emitFunction`'s return-type lowering, the
`ReturnStatement` copy-vs-move choice, `enable_shared_from_this` emission,
`isSharedReturningCall`'s local-declaration fix) needed zero changes, since
they only ever called `getMethodReturnPointerType`/`classNeedsSharedFromThis`
— the query surface stayed identical; only what populated it changed. That
clean separation is what made the swap cheap once the direction changed.

### The inference rule, and why it needs a real (if small) fixed-point

A method M is classified `ALIAS_RETURNING` (return type `shared_ptr<T>`) iff
every reachable `return <expr>;` in its body is one of:
- `return this;`
- `return <field>;` — a bare identifier or `this->field` naming an
  object-typed field of the same class (never `new T(...)`, never anything
  else)
- `return <call>;` where the call targets ANOTHER method already classified
  `ALIAS_RETURNING` (explicit receiver or implicit `this` — both count)

A field ever returned this way is forced `shared_ptr` too, through the exact
same `explicitHints`/`getPointerType` lookup path a real `//@ptr` hint would
use (no new field-side query — only the method-return side needed one).

This genuinely cannot be a single top-to-bottom scan: yaml-cpp's
`operator[](string)` calls `findOrAppendSibling`, so `findOrAppendSibling`
must be classified FIRST. The implementation (`SmartPointerAnalyzer::
inferAliasReturningMethods`) runs a small `while (progress) { ... }` loop per
class — bounded by method count, since a method's classification only ever
moves from undecided to decided, never back.

**A real bug in the first working version of the fixed-point itself**: direct
recursion (`findOrAppendSibling` calling `next.findOrAppendSibling(entryKey)`
inside its OWN body) deadlocked the algorithm — classifying `findOrAppendSibling`
requires knowing whether `findOrAppendSibling` is alias-returning, which isn't
decided yet (it's what's currently being decided), so the method waited
forever on its own classification and silently defaulted to NOT
alias-returning. Confirmed via a minimal probe (a two-method
`findOrAppend`/`access` class) before touching the real file — the generated
C++ simply never picked up `shared_ptr` anywhere, no error, just wrong output.
Fixed by special-casing self-recursion (direct or via implicit `this`): a
method calling itself can never REFUTE its own alias-returning classification
(if every other return matches the rule, the recursive call is consistent
with, not contrary to, that conclusion), so it's skipped rather than treated
as "waiting on an undecided dependency."

### `return this;` needs `shared_from_this()`, and why the alternative is a real double-free

Confirmed by direct probe, not assumed: emitting `shared_ptr<T>(this)` for
`return this;` COMPILES — and is a genuine bug. It constructs a brand-new
control block that thinks it uniquely owns the object, completely unaware of
whatever `shared_ptr` already owns it; when both control blocks eventually
run their destructor, the object is deleted twice. The correct call is
`shared_from_this()`, which requires the class to inherit
`std::enable_shared_from_this<T>` AND every instance to already be
constructed as a `shared_ptr` (never `unique_ptr`) — confirmed working via a
minimal `enable_shared_from_this` + `make_shared` probe before wiring it into
the emitter. `classNeedsSharedFromThis` derives which classes need the base
from the SAME inference pass (a class needs it iff some alias-returning
method's body contains `return this;`) and reuses the EXISTING
`classesNeedingSharedRoots` set — the same mechanism back-reference/
bidirectional field patterns already use to force a class's construction kind
to `shared_ptr` — so `new ClassName(...)` sites need no separate change to
start picking up `make_shared`.

### Multiple `operator[]` overloads on one class silently collided

yaml-cpp's real `Node` declares TWO `operator[]` overloads (`(string)` for map
access, `(int)` for sequence access) — and `classMethodParams_`
(`AstEmitter`/`DanglingChecker`) keyed both under the bare name `"operator[]"`,
so the second declaration's `methodParams[key] = method->parameters;`
silently overwrote the first's. Confirmed via probe that this does NOT corrupt
the actual CALL — real C++ overload resolution still picks the correct body
at each call site regardless of what CeeLing's own bookkeeping thinks — but it
DOES corrupt move/pointer-kind decisions for any overload taking an
object-typed argument (not exercised by yaml-cpp's own two overloads, which
both take primitives, but a real, confirmed bug nonetheless). Fixed with
`overloadKey(methodName, paramCount)` (`class_hierarchy.h`, next to
`specializationKey` — same "shared naming convention, no new data structure"
precedent), applied ONLY when a class genuinely declares more than one method
of the same name, at both the collection sites
(`DanglingChecker::deriveSummaries`, `AstEmitter::collectClassNames` —
`EscapeHatchChecker`'s `classMethods_` is a bare-name `set` and was never
actually vulnerable, just imprecise) and their lookup sites
(`resolveCallReturnAlias`, `calleeParam`, both retrying with the overload key
when the plain name misses).

### Two more real bugs, found only by re-running the actual file

Every fixture written before the real end-to-end re-test passed. The re-test
found two more bugs anyway — the same lesson Steps 19/20/22 already taught,
repeating because it keeps being true: **a fixture only proves the scenario
you already thought of; the actual target file is what surfaces the one you
didn't.**

1. `Node(int nodeKind) { ...; child = nullptr; ...; }` — a bare-identifier
   FIELD store (implicit `this`, inside a constructor) — was wrongly routed
   through the alias-return machinery (`isSharedReturningCall`'s companion
   assignment check) instead of an ordinary field store, emitting
   `this->child->operator=(nullptr);`, which does not compile. Fixed by
   excluding `isCurrentField` targets — a field store is handled by its own,
   earlier, unconditional category, never an operator= mutation or a
   shared-return concern.
2. `root["name"]` (a subscript call, not a `recv.method(...)` `FunctionCall`)
   kept emitting `unique_ptr<YAML::Node>` even after `operator[]` itself
   correctly emitted `shared_ptr<Node>` as its own return type.
   `isSharedReturningCall` (which decides a LOCAL declaration's own pointer
   kind when initialized from a shared-returning call) only checked
   `FunctionCall`, never `SubscriptExpression` — Step 22's `operator[]` read
   path lowers to a synthetic `operator[]` call for emission purposes, but
   nothing on the shared-return-detection side knew that. Fixed by extending
   `isSharedReturningCall` to also recognize a class-typed
   `SubscriptExpression` (querying `getMethodReturnPointerType(objClass,
   "operator[]")`, the same synthetic name Step 22's read-path emission
   already keys by). A SECOND, independent bug surfaced once THAT was fixed:
   `inferAliasReturningMethods` only ever wrote its findings under a class's
   BARE name (`"Node"`), never the namespace-qualified one (`"YAML::Node"`) —
   but a namespaced local's declared type IS the qualified string, so the
   query above found nothing and silently fell back to `unique_ptr` again.
   Fixed with the same dual-registration every other collection pass in this
   codebase already needed for namespaced classes (Step 21) — write under
   both `cls->name` and `cls->namespaceName + "::" + cls->name`.

### The real end-to-end proof

`yaml-cling/src-clg/yaml_node.clg`, entirely UNCHANGED from before this step
(zero ownership annotations, matching its own file-header comment: "NO
ownership annotations — inference decides"), now both compiles and runs. A
driver program (`yaml-cling/tests/node_smoke_main_rust_backend.clg`,
concatenated with the library the same way the pre-existing `master`-branch
smoke test does) builds a small map (`name`/`count` keys) and a sequence,
reads them back through `.as<string>()`/`.as<int>()`/`operator[]`/`size()`,
and prints correct output — verified via
`yaml-cling/tests/run_node_smoke_rust_backend.sh`, comparing against
`expected_node_smoke_rust_backend.txt`. This is the definitive proof the whole
`rust_backend` yaml-cling effort (enum, namespace, templates, qualified calls,
operator overloading, alias-return inference) actually composes for the real
target, not just that each slice's own fixtures pass in isolation.

One real, confirmed, and honestly-stated remaining limitation surfaced while
writing the driver: `root["name"].Scalar()` (a method call chained directly
onto a subscript result) is rejected as an unknown call — every checker in
this codebase does lightweight, non-full-type-inference resolution
(`resolveObjectType` and equivalents only resolve bare identifiers), so a
call on the result of ANOTHER call/subscript can't be statically typed. The
driver works around this with an intermediate variable at every chaining
point (`YAML::Node x = root["name"]; x.Scalar();`), which is not a
`yaml_node.clg`-specific issue — it's a general, pre-existing constraint on
what any `.clg` program can chain today, tracked in `yaml-cling/notes/
GAPS.md`'s `rust_backend` section (RB5) rather than silently worked around.

Full CTest (13/13) and safety-suite (70/70, compile-gated) sweeps after this
step showed zero regressions elsewhere.

## Step 24 — `char`, a real YAML parser, and the value of a ground-truth cross-check

Step 23 closed the `Node` port; this step closes the last explicit exclusion
from that plan: "no parser this round." The user asked, directly, for
`YAML::Load` to actually parse real `.yaml` text — not just hand-built trees.

### `char`: a small, deliberately narrow addition, with one explicit constraint from the user

A hand-rolled tokenizer needs to scan text character by character. CeeLing had
no `char` type and no `string[i]` at all. The user's instruction going in was
explicit: add `char`, but do NOT let it become a route back to raw
pointers — `char*` is THE classic C raw-pointer/string type, and reopening it
would undermine the whole "no raw pointers" contract for one convenience
feature. The fix matches the design notes' existing philosophy exactly: `char` is a
pure VALUE type (`char_literal` token, `string[i]` returns `char`), and
`char*`/`char[]` are grammatically INEXPRESSIBLE — no pointer/array
declarator production exists anywhere in this grammar, the same "forbidden by
missing syntax, not by a checker rule" approach every other raw-pointer
closure in this codebase already uses. `string[i]` needed zero new emission
code: the existing generic `SubscriptExpression` read path already emitted
`receiver[index]` literally (the container fallback from Step 22's work),
which happens to be exactly correct real C++ for `std::string::operator[]`
too. Escape sequences (`\n`/`\r`/`\t`/`\\`/`\'`/`\0`) were added only once a
real consumer (the scanner's own newline detection) needed one, not
speculatively.

### The Parser surfaced four real AST-builder/emitter bugs that have nothing to do with `char`

Writing an actual ~350-line, 4-class recursive-descent parser (`Scanner` +
`Token` already existed from an earlier session; `Parser` + `YAML::Load` are
new) is a much larger, more structurally varied piece of code than anything
previously written in CeeLing — and it immediately surfaced bugs that had
been sitting latent in the language for a while, simply because nothing had
exercised these particular shapes before:

1. **Multi-parameter constructors and methods silently dropped every
   parameter after the first.** `parameter_list`'s own grammar is `seq(type,
   identifier), repeat(seq(',', type, identifier))` — the FIRST parameter is
   2 children wide (`type`, `identifier`), but every parameter after it has a
   leading `,` SIBLING too, making it 3 children wide. Both
   `buildConstructorDeclaration` and `buildMethodDeclaration` walked
   `parameter_list`'s children with a fixed `j += 2` stride — correct only
   for the first parameter, silently desyncing and dropping everything after
   it. `Token(int k, string v)` compiled a constructor taking only `k`; `v`
   vanished with no error (the constructor body's own `this->value = v;`
   happened to reference the dropped name, turning into a real but confusing
   "undeclared `v`" g++ error — a body that didn't reference the dropped
   parameter by name would have compiled into something silently wrong
   instead). This was NOT a new bug — the fixed-stride code had presumably
   been wrong since whenever multi-parameter constructors were first
   written — but nothing before this session's work had a multi-field class
   with a multi-parameter constructor densely enough to trip over it twice.
   Fixed by pairing type/identifier by TYPE (walking every child, matching
   `buildFunctionDeclaration`'s own already-correct pattern) instead of a
   fixed stride.
2. **Parenthesized sub-expressions did not parse at all.**
   `primary_expression`'s grouped alternative (`'(' expression ')'`) has
   THREE children, but `buildExpression`'s `primary_expression` case
   unconditionally used `ts_node_child(node, 0)` — correct for every OTHER
   alternative (`identifier`/`number`/`string_literal`/`char_literal`, all
   exactly one child), but child 0 of the grouped form is the LITERAL `(`
   TOKEN. `bool b = (a);` and `a && (b || c)` both failed to parse entirely.
   Confirmed via `tree-sitter parse` directly that the GRAMMAR already
   handled the shape correctly — this was purely an AST-builder bug, not a
   grammar gap. Fixed with `ts_node_named_child(node, 0)` instead, which
   skips literal tokens automatically and is correct for every alternative.
3. **`someCall().field` had no valid parse at all.** `member_access`'s
   left-operand choices (`member_access | subscript_expression |
   primary_expression`) never included `function_call`. Confirmed via
   `tree-sitter parse` that `.field` after a call became a bare `ERROR` node
   SIBLING at the top level (not nested inside a builder call, so none of
   this codebase's existing `rejectUnbuildableChild` discipline caught it) —
   the AST builder's error recovery silently dropped it. `int x =
   h.get().val;` transpiled clean with `.val` entirely gone (`int x =
   h->get();`), only failing at the FINAL g++ step with a confusing
   type-mismatch pointing nowhere near the real cause. Fixed by adding
   `function_call` to `member_access`'s choices (confirmed zero new GLR
   conflicts via `generate-grammar`), plus a companion fix `exprObjectClass`
   needed regardless: deciding `.` vs `->` for a call RESULT has no
   variable to look up a class through, so two new maps
   (`functionReturnType_`/`classMethodReturnType_`, mirroring
   `functionParams_`/`classMethodParams_`'s existing shape exactly) let it
   resolve both an explicit-receiver call (`h.get().val`) and an
   implicit-`this` call inside a method body (`current().val`).
4. **A container FIELD accessed through an intermediate class-typed
   receiver emitted `.` instead of `->`.** The Parser holds its own
   `Scanner` as a field and reads `scanner.out[pos].kind` throughout.
   `containerVarType` (deciding a subscript's element `.`-vs-`->`) only ever
   checked a bare-identifier receiver — a local/parameter, or (from an
   earlier, already-shipped fix) the CURRENT class's own implicit-`this`
   field. An EXPLICIT `member.field` receiver (`scanner.out`, not bare
   `out`) had no case at all. Fixed by adding a `MemberAccess` branch that
   resolves the receiver's class via `exprObjectClass` first, then looks up
   the field the same way the bare-identifier case already does.

None of these four are `char`-specific — they were general AST-builder/
emitter gaps that a sufficiently varied piece of real code was always going
to surface eventually. This is the same lesson every prior "re-test against
the real file" step in this project has taught, restated once more because
it keeps being true: **a fixture only proves the scenario someone already
thought to write; a genuinely new piece of real code is what finds the one
nobody did.**

### Namespaced free functions were never part of the original namespace design

`YAML::Load(...)` needed to resolve as a known, callable free function — but
namespace support (Step 21) only ever gave `ClassDeclaration`/
`EnumDeclaration` a `namespaceName` field and the matching dual-registration;
`FunctionDeclaration` had neither. This was not a bug in the earlier work — free
functions inside namespaces genuinely were out of scope at the time — but it
needed closing now. Added `FunctionDeclaration::namespaceName`, wired through
the AST builder's namespace-desugaring (`buildNamespaceDeclaration`), the
emitter's namespace re-wrapping (`emitTopLevel`'s grouping logic), and all
three checker modules' qualified-name dual-registration
(`EscapeHatchChecker::collectCallables`, `DanglingChecker::deriveSummaries`,
`AstEmitter::collectClassNames`) — the exact same pattern classes/enums
already had, mechanically extended to a fourth `Statement` subtype.

### Two more alias-return inference passes, and why ORDER matters between them

`Node Load(string text) { Parser p = new Parser(text); return
p->parseDocument(); }` needed to be classified alias-returning too (`Load`
returns a call to `parseDocument`, itself alias-returning because it returns
a LOCAL that gets promoted to `shared_ptr`) — and Step 23's original
inference (`inferAliasReturningMethods`) covered neither shape:

- **`promoteReturnsOfSharedLocals`** — a method returning a bare LOCAL whose
  pointer-kind was independently promoted to `shared_ptr` by a LATER pass
  (`promoteSharedCallArguments`, because the local was passed into a
  parameter that itself needed sharing) was never recognized by the
  original structural rule (`this`/field/alias-call only — never a plain
  local). Confirmed via probe on `Box makeAdopted() { Box parent = new
  Box(1); Box kid = new Box(2); parent.adopt(kid); return kid; }` — `kid`
  correctly becomes `shared_ptr` (passed into `adopt`'s parameter, itself
  needing sharing), but `makeAdopted` stayed `unique_ptr`, a genuine
  return-type mismatch. This new pass MUST run genuinely LAST in
  `analyzeProgram` — after every local's FINAL pointer-kind decision,
  including whatever the pass immediately before it just changed.
- **`inferAliasReturningFreeFunctions`** — the free-function counterpart of
  the original rule's "calls an already-classified alias-returning method"
  case, since free functions have no class scope to be classified alongside
  in the same fixed-point loop. This one has to run even LATER than
  `promoteReturnsOfSharedLocals` — a free function calling a class method
  needs that method's classification to be TRULY final. An earlier attempt
  inlined this check directly into `inferAliasReturningMethods` (running
  first, right after the class-method fixed-point loop) and it silently
  failed: `Load`'s own check queried `parseDocument`'s classification before
  `promoteReturnsOfSharedLocals` had set it, confirmed via temporary debug
  instrumentation (`fprintf(stderr, ...)` gated behind `getenv
  ("CLING_DEBUG_PTR")`, removed once the ordering bug was found and fixed).
  Moving it to its own, later-running pass fixed it.
- Two more small, separate wiring gaps surfaced alongside these:
  `AstEmitter::emitFunction`'s free-function call site (`emitTopLevel`)
  never passed the `methodName` parameter its own shared-return check
  actually reads (`methodName` defaults to `""`; only class-method call
  sites had ever wired it through) — even a CORRECTLY classified free
  function stayed `unique_ptr` at emission until this was fixed. And a call
  SITE outside the namespace (`Node root = NS::build(...);`) needed the
  classification dual-registered under the QUALIFIED name too, since the
  definition-side query always uses the bare name but a call site spells it
  qualified — the same qualified-vs-bare split every other namespaced
  lookup in this codebase already has, just one more place it needed
  applying.

### A real scanner bug that only a ground-truth cross-check against actual yaml-cpp caught

Every fixture written up to this point passed. Re-testing against a more
representative document — closer to what `yaml_node.clg`'s own smoke test
already covered, but now parsed from TEXT instead of hand-built — surfaced
two more real bugs that no fixture had been shaped to catch, because writing
the "correct" fixture requires already knowing the right answer, and this is
exactly the kind of case where the right answer isn't obvious without a
ground-truth to compare against:

- **Compact sequence-of-maps entries tagged every token on their line with
  the DASH's column, not the KEY's.** `- key: v1` at column 2 has its key
  actually starting at column 4 (`"- "` is two characters); a continuation
  key on the next line (`    other: v2`) is correctly aligned at column 4
  too. The scanner tagged `key`/`v1` with column 2 (the dash's own
  `lineIndent()` result) instead of the content's real column, so `other`'s
  correctly-computed indent (4) never matched what the parser expected for
  that map (wrongly 2) — parsing silently stopped after the FIRST key.
  `other: v2` simply vanished, a wrong VALUE with no error or crash, not
  something a compile-only or even a run-only-on-a-simpler-fixture check
  would ever surface. Fixed by tracking a separate `contentCol` (the dash's
  column plus however many characters `- ` actually consumed) and tagging
  the compact entry's own tokens with that instead of the line's leading
  `lineIndent()` value.
- **Returning from deep nesting straight to the top level can require
  popping the indent stack multiple times** (one `TkDedent` per level), but
  the map-parsing loop's `atEnd` check treated ANY single `TkDedent` as "this
  map is over" — even when that dedent's own target indent was still ABOVE
  the current map's own indent, meaning the map genuinely was not over yet.
  A top-level key written after a multi-level-nested block (`flag: 'ok'`
  after a compact sequence-of-maps entry two levels deep) was silently
  never parsed at all. Fixed by having `atEnd` CONSUME (advance past) a
  `TkDedent` whose own indent is still `>=` the scope currently being
  parsed — only a dedent (or an ordinary token) genuinely below that scope's
  indent actually ends it.

Both were found by building and running ACTUAL upstream yaml-cpp
(`C:\project\yaml-cpp`, compiled directly from its own real source on this
machine, not simulated or recalled from training) on the exact same YAML
text yaml-cling's port was being tested against, and diffing the two
programs' output line by line. This is the strongest verification technique
available for a port like this, and it earned its keep immediately — neither
bug produced a compile error or a crash in yaml-cling's own output; both
produced silently wrong VALUES that only a real ground-truth comparison
could catch. `tests/run_parser_smoke_rust_backend.sh` locks in the fix with
one representative document; the cross-check itself is not automated into
CI (it requires yaml-cpp source on the machine, not a CeeLing/yaml-cling
dependency), but the technique and result are recorded here for anyone
revisiting the parser later.

### The result

Real upstream yaml-cpp and yaml-cling's `rust_backend` port now produce
**byte-identical output** on a document exercising every feature in this
slice's scope: nested maps, a plain sequence, a multi-key compact
sequence-of-maps entry, a `#` comment, single- and double-quoted scalars,
and a top-level key following deeply nested content. `.yaml` text now
genuinely parses into a working `Node` tree — the last item Step 23's plan
had explicitly excluded ("no parser this round") is done.

Full CTest (13/13) and safety-suite (82/82, compile-gated) sweeps after this
step showed zero regressions elsewhere.

## Step 25 — Real multi-file compilation: `.h` header generation + `.clh` source-level includes

`yaml-cling`'s three library files (`yaml_node.clg`, `yaml_scanner.clg`,
`yaml_parser.clg`, plus a new `main.clg` entry point) had never actually been
compiled as separate translation units. Every prior end-to-end proof (Step 24
included) worked by textually concatenating the `.clg` SOURCE files together
before invoking `cling` once on the combined text — documented at the time as
the sanctioned workaround for "CeeLing has no cross-.clg include yet," but a
workaround nonetheless: it could not catch a bug that only shows up under
real separate compilation (ODR violations, missing out-of-line definitions,
each `.clg` needing to compile and link independently). Asked to produce a
genuine one-`.clg`-to-one-`.cpp` build — no source merging, ordinary `g++ -c`
per file, then link — this gap became unavoidable to close for real.

### The two things a multi-file C++ story actually needs

Ordinary C++ separate compilation rests on two things `cling` did not have:
a way to emit a DECLARATION-only artifact (a header) from a `.clg`, and a way
for a `.clg`'s own semantic analysis to treat another `.clg`'s declarations
as known rather than "unknown call" (Phase 1's default-reject rule otherwise
rejects every method on an `#include`d type, since an opaque `#include` was
never parsed for declarations — this is the exact gap
[Chapter 9](09-status-and-roadmap.md)'s Phase 4.6 had flagged: "unknown call"
and "call to anything external" were the same set).

**Header generation** (`AstEmitter::emitHeader`, `cpp_generator.h`'s emitter
family): output-file extension `.h` now requests a declaration-only emission
instead of `.emit()`'s full-definition one. Fields emit as before; ordinary
methods/constructors emit SIGNATURE + `;`, no body. Two exceptions, both
forced by real C++ template rules rather than a design choice: the primary
generic template (`<T> T as() {...}`) must stay fully defined in the header,
because an ordinary function template's definition must be visible at every
instantiation point; and each concrete specialization (`as<int>`, `as<bool>`,
...) must ALSO stay in the header, marked `inline`, for the same reason — an
explicit specialization is a full definition, not implicitly inline the way
an ordinary template instantiation is, so without `inline` two translation
units both including the header would violate ODR the moment both got
compiled. `emitClassHeader`/`emitTopLevelHeader` mirror `emitClass`/
`emitTopLevel`'s traversal exactly (same namespace grouping, same field/base
handling) so a header and its paired `.cpp` never structurally drift apart.

**The `.cpp` side had a latent bug this immediately surfaced**: with a header
declaring signatures only, the paired `.cpp`'s method bodies needed to be
REAL out-of-line definitions (`ReturnType Class::method(...) {...}`), not
in-class bodies. An in-class body is implicitly `inline`, and the compiler is
free to drop an unused inline function's code from that translation unit's
object file entirely — which is exactly what happened: `yaml_node.cpp`
itself never calls `IsMap()`/`operator[]`/etc., so `g++ -c` produced an
object file with ONLY the four `as<T>` symbols in it (confirmed with `nm`),
and `main.cpp`, seeing only the header's signature, failed at link time with
"undefined reference" for everything else. `emitClass` was restructured to
declare every constructor/method in-class (via the same
`buildFunctionSignature` helper `emitClassHeader` uses) and define all of
them out-of-line afterward — ordinary external-linkage functions, always
emitted regardless of whether this TU itself calls them. Member-template
specializations are the one thing deliberately NOT emitted in the `.cpp` at
all anymore: they live in the header only (as `inline`), because emitting
them a second time, non-inline, in the `.cpp` is a textbook ODR "multiple
definition" the instant two TUs both include that header — confirmed by
hitting exactly that linker error before removing the `.cpp`-side emission.

**Cross-file semantic visibility** (`main.cpp`'s `mergeExternalDeclarations`):
`.clg` source spells a local include as `#include "foo.clh"` — `.clh` is a
SOURCE-level-only convention (no such file is ever read or written) that
names foo's declarations the way `.clg` names foo's definitions. Resolving
one does not parse a `.clh` file (there is none); it finds and parses foo's
own `.clg` with the same `ASTBuilder`, then merges its top-level statements
into the including program's `Program`, each marked with a new
`Statement::isExternalDeclaration` flag. Every semantic pass
(`EscapeHatchChecker`, `SmartPointerAnalyzer`, `DanglingChecker`) already
just walks `program.statements`, so merging is sufficient to make `Node`/
`Scanner`/etc. resolvable — no checker changed. `AstEmitter::emit`/
`emitHeader` gained one new guard each (skip a statement whose
`isExternalDeclaration` is set, including its own `#include` lines) so a
merged-in declaration is visible for analysis but never re-emitted — the
actual definition already lives in that file's own separately-generated
`.cpp`. Resolution recurses (so `parser.clg`'s own `#include "yaml_node.clh"`
+ `"yaml_scanner.clh"` resolve transitively into `main.clg`'s program too)
and de-duplicates via a `seenClgFiles` set, so a diamond include only merges
once. The emitted C++ rewrites `#include "foo.clh"` to `#include "foo.h"`
(`rewriteClhIncludes`) since `.h` is the real generated file g++ needs to
find — the `.clg` author only ever writes `.clh`, never worrying about
whether the on-disk artifact happens to exist yet.

### What this does NOT do

This is not the general "Multi-file compilation and module dependency" item
[Chapter 9](09-status-and-roadmap.md) tracks as an open Phase 4.6 goal — there
is still no incremental build, no package/module system, and a `.clh`
include still requires the sibling `.clg` to be readable at compile time (it
re-parses the dependency's full source on every compile, just for
declarations — cheap at yaml-cling's current size, not yet optimized for a
large project). What it does close is the specific, previously-open gap this
step set out to fix: a `.clg` file can now `#include` another's declarations
and the two compile as genuinely separate translation units, linkable with
ordinary `g++ -c` + link — not a text merge.

### The end-to-end proof

`yaml-cling/src-clg/{yaml_node,yaml_scanner,yaml_parser,main}.clg` each
transpile independently (`yaml_node.clg`/`yaml_scanner.clg` to both a `.cpp`
and a `.h`; `yaml_parser.clg` additionally `#include`s both `.clh`s;
`main.clg` is the new entry point, `#include`ing `yaml_node.clh` and
`yaml_parser.clh`). All four `.cpp` files compile independently
(`g++ -std=c++17 -c`) into `.o` files, and `g++`-linking the four `.o` files
produces a working executable that prints the expected `Load`/`operator[]`/
`.as<T>()`/`IsMap`/`IsSequence`/`size` output — the same YAML fixture Step 24
proved correct against upstream yaml-cpp, now built the way a real multi-file
C++ project actually builds, not via source concatenation.

## Step 26 — A real parser bug found by a fifth `yaml-cling` file: a top-level comment before a namespace-of-only-free-functions

Writing `yaml-cling/src-clg/yaml_emitter.clg` (a `Dump(Node) -> string`
serializer, the inverse of `yaml_parser.clg`'s `Load`) hit a genuine CeeLing
bug on the very first attempt — a plain, conventional file header comment
(the same style every other `src-clg/*.clg` file already used successfully)
made `cling` reject the file with `Error: Parse error at line 1, column 1,
node type 'comment'` — a message that pointed at the comment itself but
was, in the end, unrelated to its content.

### Root-caused with the CST, not by guessing at the fix

`tree-sitter parse` on the failing input showed a completely clean tree —
zero `ERROR` nodes, the comment sitting exactly where it should as a bare
child of `source_file`. That ruled out the grammar (`grammar.js`) outright:
whatever was wrong lived in CeeLing's own C++ (`ast_builder.cpp`), not the
parser generator's output. `buildProgram`'s top-level loop
(`ast_builder.cpp`) tracks two booleans while walking `source_file`'s
children: `hasUnresolvedTopLevel` (some child failed to become a
`Statement`) and `hasStructuralAnchor` (a `ClassDeclaration` or
`MainFunction` was seen) — and rejects the whole file only when the first is
true and the second is false, the idea being "a genuinely broken construct
is only a hard error if there's nothing anchoring this as a real program."
The loop calls `buildStatement` on every child that isn't `ERROR`/missing
and isn't a `namespace_declaration`-wrapped-in-`statement` — including a
bare `comment` node, which `buildStatement` naturally doesn't recognize
(comments are declared as a grammar `extras` token, "skippable anywhere,
like whitespace," specifically so they never need their own AST node). That
made every top-level comment count as "unresolved." Ordinarily harmless
(nearly every real `.clg` file has a `class`/`main` somewhere to serve as
the anchor) — but `yaml_emitter.clg` is a pure library file: no class, no
`main`, only free functions in a reopened `namespace YAML { ... }`. With
nothing to set `hasStructuralAnchor`, the comment's false "unresolved" flag
became fatal. `yaml_parser.clg` (which also opens with a comment, also
reopens `namespace YAML`) never hit this only because its namespace's FIRST
member is `class Parser` — one `ClassDeclaration` anywhere in the file is
enough to mask the bug for the whole file.

The actual confirmation this diagnosis was right, not just plausible: a
sibling function, `buildNamespaceDeclaration`, walks a namespace body with
the SAME shape of loop, and its `if`/`else if` dispatch (`"ERROR"`/missing →
reject, `"statement"` → build) has no matching branch for a bare `comment`
child at all — meaning it silently ignores one by simple fallthrough. That
this second, independent loop already got it right (and that moving the
same header comment to inside the namespace, as a manual test, made the
top-level failure disappear) pinned the exact missing guard.

### The fix

`ast_builder.cpp`'s top-level loop in `buildProgram` gained one explicit
check — skip (via `continue`) any child whose type is `"comment"`, mirroring
`buildNamespaceDeclaration`'s existing (correct) fallthrough behavior —
before the code that calls `buildStatement` and records "unresolved" on a
null result. Three lines of real logic; the rest of the diff is the comment
explaining why, per this project's own no-silent-fix discipline.

### Verification

Full CeeLing regression (`ctest`, all 13 tests including the 82-fixture
safety-contract baseline) stayed green — this fix touches a path no
existing fixture exercises (every fixture is a runnable program with
`main()`, and `main()` itself always sets `hasStructuralAnchor`, which
means **no existing or even hypothetical `main()`-containing fixture could
ever have caught this bug**, since the masking condition is unconditional
once any `MainFunction`/`ClassDeclaration` exists anywhere in the file — a
real, permanent blind spot in the current safety-fixture convention, noted
here rather than worked around with a mismatched fixture shape). Real
regression coverage instead comes from `yaml-cling` itself:
`src-clg/yaml_emitter.clg` now compiles as originally written (conventional
top-of-file header comment, no workaround), and the previously-failing
`yaml-cling/src-clg/CMakeLists.txt` build (which transpiles every
`src-clg/*.clg` file, including the emitter, as part of its
`add_custom_command`-driven library target) passes end to end. A dozen
hand-bisected minimal repros (comment position, namespace member order,
presence/absence of `#include`, free-function return type) were re-run
against the fixed binary and all now transpile successfully.

## Step 27 — `operator*`/`operator++`/`operator!=`, value-type classes, and real C++ range-for iteration

yaml-cling wanted `for (Node x : node)`-style iteration, mirroring
yaml-cpp's `begin()`/`end()`. This turned out to be the largest single
feature added this session — not because the end result is large, but
because building a genuine C++-range-for-compatible iterator class exposed
three separate, previously-unknown CeeLing limitations, each requiring a
real fix, each found by testing rather than assumed.

### The core language additions

`operator*` (dereference), `operator++` (prefix increment only — no
postfix, unneeded by range-for's `++it` desugaring), and `operator!=`
(equality only — no `operator==`, unneeded either) join `operator[]`/
`operator=` as the full set of user-definable operator overloads
(`grammar.js`'s `operator_method_name`). `unary_expression` gained `*`/`++`
as prefix operators (previously only `!`/`-`) — confirmed via a real bug
found in the SAME change: `buildUnaryExpression` (`ast_builder.cpp`)
recovered the operator from the leading source text by taking exactly ONE
character (`full[0]`), silently truncating `++` to a single `+`; fixed by
special-casing the two-character `++` prefix before falling back to the
one-character case.

**A class defining all three of `operator*`/`operator++`/`operator!=` (all
three, never fewer — confirmed as the exact threshold) is INFERRED as a
VALUE TYPE** — never wrapped in `unique_ptr`/`shared_ptr` for its own
variables, fields, or method returns, unlike every other CeeLing class.
Purely structural, no annotation — same philosophy as Step 23's
alias-returning-method inference. This exists because real C++ range-for
needs `begin()`/`end()` to return the iterator BY BARE VALUE — `std::
shared_ptr`/`unique_ptr` have no `operator++` at all, so a smart-pointer-
wrapped iterator can never satisfy the range-for protocol, confirmed via
direct probe before any design work started.

`SmartPointerAnalyzer` gained `isValueTypeClass(className)`, backed by a
new, fully independent `classifyValueTypeClasses` pass (reads only the raw
`Program`, runs before everything else, no ordering dependency on the
existing two-pass pointer-kind machinery). `AstEmitter` checks this at
every site that previously assumed "class-typed means pointer-backed"
unconditionally: local variable declarations, class fields, method/
function return types, parameter types (passed by value — value types are
meant to be cheap to copy), all four `new ClassName(...)` construction
sites (plain constructor call, no `make_unique`/`make_shared` factory), and
the `.`/`->` member-access decision. `DanglingChecker::isObjectType` gained
a matching exclusion — without it, an ordinary legal copy
(`Iterator b = a; Iterator c = a;`) was wrongly flagged "use of moved-from
value `a`" on the second copy, confirmed via direct probe: value types are
copyable, not move-only, unlike every other (always smart-pointer-backed)
CeeLing class.

**A real bug found via the FIRST test program** (a class returning `this`
from a value-type method): `this` is ALWAYS a raw pointer in C++
regardless of the containing class's value-type-ness, so `return this;`
where the return type is the class itself (not a pointer) does not
compile — confirmed via direct probe (`g++`: "invalid conversion from
'Foo*' to 'int'", the compiler trying to construct a new `Foo` from the
pointer, using the WRONG overload). Fixed by detecting this exact shape in
`ReturnStatement` handling and emitting `return *this;` (dereference, then
copy-construct) instead.

### Applying it to yaml-cling's `Node`: three more real bugs, one genuine architectural fork

Building the actual `Node::Iterator` (not a synthetic test class) surfaced
problems the synthetic tests hadn't, in order:

1. **A genuine circular class reference with no forward-declaration
   escape.** `Node::begin()`/`end()` (member methods, the obvious design)
   need `Iterator` fully defined for their return type; `Iterator` needs
   `Node` fully defined for its own `current` field and `.next` access.
   CeeLing has no forward-declaration syntax (`class Iterator;`) at all —
   confirmed there is no way to write this circular pair as CeeLing member
   methods. **Resolved architecturally, not by adding forward-declaration
   support**: `begin(Node)`/`end(Node)` as FREE FUNCTIONS in the `YAML`
   namespace instead of `Node` methods — the standard C++ idiom for adding
   iteration to a type without adding member methods to it. C++'s
   range-for resolves `begin`/`end` via ADL (argument-dependent lookup)
   automatically when no member exists; CeeLing's existing native
   `for (Type x : expr)` emission already relies on the C++ compiler to
   pick member-vs-ADL lookup, so this needed zero additional CeeLing
   changes on its own — `Iterator` is defined strictly after `Node`
   (referencing it freely), and `Node` never needs to know `Iterator`
   exists.

2. **The for-loop's own container-dereference logic (added earlier this
   session for a synthetic member-method test) broke the free-function
   case.** A smart-pointer-wrapped class container with a MEMBER
   `begin()`/`end()` needs dereferencing (`for (T x : *r)` — a smart
   pointer has no `begin()`/`end()` of its own). But a container relying on
   ADL free functions must NOT be dereferenced — the free function's
   parameter is smart-pointer-wrapped (CeeLing always wraps class-typed
   parameters; there is no reference-parameter syntax to avoid this), so it
   needs the UNDEREFERENCED container variable to match. Fixed by checking
   whether the container's class has a MEMBER `begin` (`classMethodParams_`
   lookup, mirroring the existing `operator=`/`operator[]`
   membership-check pattern) before deciding to dereference.

3. **Two independent pointer-kind inferences (a local variable's default,
   and a free function's parameter default) can disagree, and — more
   fundamentally — `unique_ptr` cannot satisfy this pattern AT ALL,
   regardless of matching.** `SmartPointerAnalyzer` decides a fresh local's
   kind and a function parameter's kind through entirely separate code
   paths with different defaults. Confirmed via direct probe that even
   when forced to agree, the deeper problem is structural: CeeLing's free
   functions always take a class-typed parameter BY VALUE (no reference
   parameters), and range-for needs the container passed to BOTH `begin()`
   and `end()` — but `std::unique_ptr`'s copy constructor is deleted, so a
   unique_ptr-backed container can never be passed by value twice, no
   matter what kind either side independently infers. Fixed by extending
   `promoteSharedCallArguments` (previously scoped to real `FunctionCall`
   AST nodes only, which a for-loop's implicit, compiler-synthesized
   `begin`/`end` calls are NOT) to recognize a `ForStatement`'s container as
   an implicit call site, and UNCONDITIONALLY force both the container
   variable AND both free functions' own parameters to `shared_ptr` — not
   merely matching whatever each side's independent inference happened to
   decide. A related, separately-confirmed qualified-name bug in the same
   fix: a container declared with a qualified type (`YAML::Node`, as the
   source wrote it) needed comparing against a free function's BARE
   parameter type (`Node`, since it's declared inside the `YAML`
   namespace) — the same qualified-vs-bare mismatch class already found and
   fixed several times this session for namespaced types, fixed here by
   stripping to the simple name before comparing.

4. **The for-loop's own LOOP VARIABLE type has a similar two-independent-
   sources problem.** Its pointer kind previously came only from
   `containerElementIsShared` — a mechanism designed for
   `std::vector<T>`'s own push_back-tracking, with no signal at all for a
   non-vector container (silently defaulting to unique). But the loop
   variable's REAL required kind is whatever `Iterator::operator*()`
   actually returns — confirmed via direct probe these two sources can
   disagree, producing a loop variable type that doesn't match what
   `operator*()` returns. Fixed by preferring `operator*()`'s own declared
   return kind (via `getMethodReturnPointerType` on the free `begin()`'s
   return-type class) whenever the container relies on ADL (no member
   `begin`), falling back to `containerElementIsShared` only for genuine
   vector/map containers.

### `yaml_node.clg`'s actual design

`Iterator` walks the same `child`/`next` chain `Node` already exposes.
Dereferencing yields the current `Node` directly — NOT a
`std::pair<Node,Node>` (CeeLing has no `std::pair` support and doesn't need
it): for a map entry, the returned `Node`'s own `.key` field already IS the
key, and the `Node` itself acts as the value (`.Scalar()`, nested
`operator[]`, etc. all already work) — mirrors real yaml-cpp's own
`iterator_value`, which is ITSELF `Node`-derived with pair-shaped access
bolted on, not a bare `std::pair`, confirmed by reading yaml-cpp's actual
`node/detail/iterator.h`.

### Verification

Three new CeeLing safety fixtures (`operator_iterator_protocol_accepted.clg`
— direct operator use + the value-type-copy bug fix;
`iterator_protocol_range_for_accepted.clg` — the member-method + container-
deref path; `iterator_protocol_free_function_adl_accepted.clg` — the ADL
free-function + forced-sharing path), all compiled AND run (not just
parsed), full CTest + 85-fixture safety suite green. `yaml-cling`'s real
`Node::Iterator` (`src-clg/yaml_node.clg`) verified via a new
`tests/run_iterator_smoke_rust_backend.sh` (real separate compilation,
mirroring the emitter/parser smoke tests), output cross-checked
byte-for-byte against real yaml-cpp's own `for (auto it : node) { it.first;
it.second; }` (map) and plain dereference (sequence) — including a
composed case (`Load()` then iterate the parsed tree, not just a hand-built
one) — on identical input.

## Step 28 — Runtime helpers (`cling::readFileToString`/`cling::fileExists`) and a `throw` string-concatenation gap, for `YAML::LoadFile`

yaml-cling wanted `YAML::LoadFile(path)`, yaml-cpp's own file-reading front
door (`Node LoadFile(const std::string& filename)`). This exposed a real,
previously-unnoticed gap: CeeLing has **zero file-I/O capability** — no
object construction/method-call support for foreign types at all, so a
`.clg` program cannot `new std::ifstream(path)` and call `.good()`/
`.rdbuf()` on it, and there is no single real `std::` free function that
reads a whole file into a string in one atomic call (unlike `std::stoi`,
which Step 22 already allow-lists). The qualified-free-function mechanism
from Step 22 was therefore the wrong tool — it only ever exposes a REAL
`std::` function; file-reading needs actual new code to exist somewhere.

### Runtime helpers: a second, distinct allow-list

`library_summary.h` gained `isKnownRuntimeHelper` (parallel to, but
deliberately separate from, `isKnownQualifiedFreeFunction`): a name on this
list is NOT a real `std::` function but a small, hand-written C++ shim that
CeeLing itself provides and `AstEmitter` auto-injects into any generated
file that calls it. `requiredRuntimeHelperDefinitions(used)` returns the
shim source for exactly the helpers a program actually calls, so a program
that never calls one is emitted byte-identically to before this feature
existed.

Two helpers, both taking/returning only primitives (no ownership/move
machinery needed, same reasoning as `std::stoi`):

- `cling::readFileToString(path) -> string` — the whole file, or `""` on any
  failure to open/read.
- `cling::fileExists(path) -> bool` — can the path be opened at all.

**Why two helpers, not one.** An earlier version had `LoadFile` check
`readFileToString`'s return value directly (`if (text.length() == 0) throw
...;`) — this is WRONG, confirmed by reading real yaml-cpp's own
`LoadFile` (`src/parse.cpp`): it only throws `BadFile` when the
`std::ifstream` fails to OPEN, never based on content length, so an
existing-but-EMPTY file opens fine in real yaml-cpp and reads back as an
empty/null `Node` — conflating that with "file not found" would be a real
behavioral divergence, not just a style choice. `fileExists` is checked
SEPARATELY, before the read, so the two cases stay distinct.

`AstEmitter::collectUsedRuntimeHelpers` is a new, self-contained recursive
walk (mirroring `SmartPointerAnalyzer`'s existing `visitExpr`/`visitStmt`
shape for a similar full-body traversal) run once per `emit()` before the
preamble is written, since deciding whether to inject `#include <fstream>`/
`<sstream>` and the shim body has to happen before any output is emitted,
but detecting the call requires walking every function/method/constructor
body — a shallower, signature-only pass (like the existing
`collectClassNames`) isn't enough. `EscapeHatchChecker::isKnownCallable`
gained the matching allow-list branch, same shape as Step 22's qualified
free-function check right above it.

### A real, narrow, pre-existing `throw` gap found along the way

`YAML::LoadFile`'s natural error message is `throw "yaml-cling: bad file: "
+ path;` — a `+` CONCATENATION, not a bare string literal or bare
identifier. `DanglingChecker`'s `ThrowStatement` check (added in Step 18)
only ever recognized those two shapes, so this was rejected:
`error[semantic/use-after-move]: throw operand is not a string` (a
misleading error CODE — this has nothing to do with moves — but it's the
generic "throw failed its type check" errors ride on).

This was a real, unjustified gap, not a safety boundary: string
concatenation has no ownership or dangling implications whatsoever — `a +
b` for two string-valued operands always produces a fresh value, exactly
like any other primitive arithmetic. Fixed with a new recursive
`DanglingChecker::isStringValuedExpr(expr)`: true for a string literal,
true for an identifier whose `localType_` entry is `string` (already
populated for string locals/parameters since Step 18), and true for a `+`
`BinaryExpression` whose LEFT and RIGHT are both (recursively)
string-valued — so chains like `a + b + c` resolve correctly via ordinary
left-associative recursion. This replaced the old two-shape inline check
verbatim; no other call site needed to change.

### Applying it to yaml-cling's real `yaml_parser.clg`

```
Node LoadFile(string path) {
    if (!cling::fileExists(path)) {
        throw "yaml-cling: bad file: " + path;
    }
    string text = cling::readFileToString(path);
    return Load(text);
}
```

Real yaml-cpp throws a class-typed `BadFile` exception here; yaml-cling
throws a plain string message instead — an already-documented divergence
(`rust_backend`'s `throw`/`catch` is scoped to string payloads only, no
exception-class hierarchy — see Step 18 and `notes/GAPS.md`'s G4/RB12
entries in `yaml-cling`), not a new one introduced by this feature.

### Verification

Two new CeeLing safety fixtures:
`runtime_helper_read_file_to_string_accepted.clg` (both helpers, exercised
against a deliberately nonexistent path — no auxiliary fixture file needed,
and the safety runner compiles/runs fixtures from a scratch tmp dir where a
real file wouldn't exist anyway) and `throw_string_concat_accepted.clg`
(a 3-way `+` chain of a literal and two string locals, thrown and caught).
Both compiled AND run, not just parsed; full CTest + 87-fixture safety
suite green.

`yaml-cling`'s real `YAML::LoadFile` verified via a new
`tests/run_loadfile_smoke_rust_backend.sh` (real separate compilation,
mirroring the parser/emitter/iterator smoke tests) — reads an ACTUAL file
from disk at runtime (not a hand-built in-memory string, unlike every prior
smoke test), plus exercises the missing-file throw/catch path. The
successful-read case's output was cross-checked against real yaml-cpp's own
`LoadFile` on the identical input file (compiled from yaml-cpp's actual
source, the same `g++ -DYAML_CPP_STATIC_DEFINE -I .../yaml-cpp/include -c
src/*.cpp` method used since Step 24) — byte-identical
(`key=name value=yaml-cling` / `key=count value=42`). The missing-file case
confirmed real yaml-cpp's `BadFile::what()` is literally `"bad file: " +
filename` (read directly from `include/yaml-cpp/exceptions.h`) — yaml-cling's
`"yaml-cling: bad file: " + path` keeps its own `"yaml-cling: "` prefix
deliberately (matching the existing convention set by `yaml_node.clg`'s
`.as<T>()` throw), not a byte-identical match, since this is an
informational caught-string message, not a structural correctness claim
the way the successful-read output is.

## Step 29 — Read-only free-function parameter inference, for `YAML::Clone`

yaml-cling wanted `contains(Key)` and `Clone(const Node&)` — the last two
gaps from `notes/API_COMPATIBILITY.md` flagged as "no CeeLing blocker, just
unwritten." `contains` turned out to be exactly that: an ordinary method
walking the same `child`/`next` chain `operator[]` already walks, no core
changes needed. `Clone` was not — it surfaced a real, previously-unnoticed
API-compatibility gap that took four found-and-fixed bugs to close properly.

### The core problem

Real yaml-cpp declares `Node Clone(const Node& node)` — a read-only
reference. The whole point of `Clone` is that the caller's original stays
usable afterward: `Node copy = Clone(root); use(root);` must not reject
`root`. But CeeLing has no reference-parameter syntax at all — every
object-typed parameter defaults to move-by-value — so a naive port
(`Node Clone(Node node) { ...; return copy; }`) made `Clone(root)`
unconditionally consume `root`, rejecting the very next use as
"moved-from." This is a real, if narrow, yaml-cpp API-compatibility bug, not
a cosmetic inconvenience — confirmed by writing it into `yaml-cling`'s own
The design notes as an explicit rule (constraint #1): parameter-passing semantics
must match yaml-cpp's declared reference-ness, not just names/return types,
and the general fix (extend CeeLing's inference) is preferred over a
function-specific workaround.

### The inference: read-only free-function parameters

A free function's object-typed parameter is INFERRED read-only — purely
structural, no annotation, same philosophy as every other inference this
project has built — iff its body:

1. Never stores, returns, or moves the BARE parameter into any sink (a
   disqualifying use), AND
2. Contains at least one GENUINE read of it (`param.field`, or a falsy/null
   check `!param`).

Condition 2 is not optional bookkeeping — see bug 2 below for why an
empty-bodied or unused parameter must NOT qualify just because it has zero
disqualifying uses.

`SmartPointerAnalyzer::classifyReadOnlyFreeFunctionParams` runs first
(alongside `classifyValueTypeClasses`, before any pointer-kind decision
exists — this classification is about a parameter's OWN body usage, nothing
else depends on it existing yet). `paramUseDisqualifiesReadOnly` is the
disqualifying-use walk (a bare `Identifier` matching the parameter name
disqualifies almost everywhere, except inside `!paramName`, which is
explicitly carved out).

`promoteSharedCallArguments`'s free-function call-site handling was
extended: when the callee's parameter is classified read-only, BOTH the
parameter's own pointer kind AND the caller's argument are unconditionally
forced to `shared_ptr` — the same reasoning Step 27's ADL `begin`/`end`
forcing already established: the parameter and the argument must end up the
SAME C++ type for the call to compile at all (a `shared_ptr` argument
cannot bind to a `unique_ptr` parameter), and a fresh local's own default
has no reason to already agree with a parameter's default by coincidence.

### Four bugs found via testing, each confirmed with a minimal probe first

**Bug 1 — `!paramName` wrongly disqualified.** The first version of
`paramUseDisqualifiesReadOnly` handled `UnaryExpression` by naively
recursing into the operand: `!node`'s operand IS the bare `Identifier`
`node`, which matches the general bare-identifier disqualifying rule and
returned `true` — even though a null/falsy check obviously doesn't consume
anything. Confirmed via a minimal isolated repro
(`if (!node) { return nullptr; } ... new Node(1); return copy;` — no
reference to `node` anywhere else) still classifying `node` as disqualified.
Fixed by special-casing `un->operator_str == "!"` with a bare-identifier
operand to explicitly return `false` before the general recursion.

**Bug 2 — an unused parameter wrongly qualified as read-only.** After fixing
bug 1, the existing `tests/safety/move_then_use.clg` fixture (`void
sink(Node n) {}`, `sink(a); use(a);` must reject) flipped from PASS to
XFAIL — a real, hard regression signal from the pre-existing safety
baseline, not something written into a new fixture. Root cause: an
empty-bodied function has ZERO disqualifying uses of `n` by construction,
so "no evidence it's consumed" was being read as "read-only" — the wrong
inference. An unused parameter provides no positive evidence of anything;
The design notes' "ambiguous → conservative reject" spirit applies here too:
no read at all is exactly as ambiguous as a disqualifying use, and should
default to ordinary move-by-value, not a new looser behavior. Fixed by
adding a second, independent walk (`bodyHasGenuineRead`, mirroring
`paramUseDisqualifiesReadOnly`'s statement-shape traversal) requiring at
least one real `param.field` access or `!param` check before granting
read-only status — condition 2 above.

**Bug 3 — qualified-name mismatches, twice more.** The exact bug class this
session had already hit twice before (Step 27's ADL forcing, Step 28's
`inferAliasReturningFreeFunctions`/`promoteReturnsOfSharedLocals`
dual-registration) recurred here: `collectCalleeParams`'s free-function map
and the new `readOnlyFreeFunctionParams_` set were both keyed by the
callee's BARE name only, but a call site from OUTSIDE the callee's
namespace spells it qualified (`YAML::Clone`, captured verbatim by
`ASTBuilder::buildFunctionCall`). Confirmed via probe: `Clone(root)`'s
use-after-move error persisted even after bugs 1–2 were fixed, because the
promotion code's `calleeParams.freeFunctions.find(call->function_name)`
lookup (`"YAML::Clone"`) never matched the bare-keyed map entry
(`"Clone"`). Fixed by dual-registering both `collectCalleeParams` and the
read-only classification set under the qualified name too — and, separately,
by stripping the qualifier back OFF before using it to build a
`pointerDecisions` key (which is always scoped by the callee's bare
DEFINITION-side name, per `registerParameters`), mirroring the
`containerSimpleClass` stripping Step 27 already needed for the same
reason.

**Bug 4 — a genuinely new, pre-existing bug: scope-blind name matching in
`promoteReturnsOfSharedLocals`.** After fixing bugs 1–3, `Clone(root)` no
longer rejected `root`, but the generated C++ failed to COMPILE: `Node copy
= Clone(root);` declared `copy` as `std::unique_ptr<Node>` while `Clone`
itself was emitted returning `std::shared_ptr<Node>`. Tracing why `Clone`'s
own return was shared at all (it returns a fresh local, `copy`, constructed
via `new Node(...)` — nothing about that shape should be alias-returning)
led to `promoteReturnsOfSharedLocals`: its existing (pre-dating this
feature) free-function loop asks "is the returned local's OWN pointer kind
SHARED" via `getPointerTypeForVariable(id->name)` — a query that does a
**program-wide, scope-blind suffix match** over every `pointerDecisions`
key ending in `.copy`, with no regard for which function that entry came
from. `yaml_node.clg`'s new `cloneChain` (a self-recursive helper `Clone`
calls) ALSO happens to declare its own, completely unrelated local named
`copy` — and `cloneChain`'s `copy` legitimately becomes `shared_ptr`
(`cloneChain`'s own parameter is read-only-and-self-recursive, forcing its
return type shared too). The scope-blind lookup then wrongly concluded
`Clone`'s `copy` must ALSO be shared, purely from sharing a bare name with
an unrelated function's local. This is a real bug that pre-dates read-only
parameter inference entirely — it just needed two same-named locals in two
different functions to surface, which no earlier fixture happened to
contain. Fixed with a new `getPointerTypeForVariableInScope(scopeName,
variableName)` (scoped via the SAME `buildKey` prefix format everything
else uses), swapped into BOTH of `promoteReturnsOfSharedLocals`'s loops
(class methods and free functions) — its other two pre-existing
`getPointerTypeForVariable` call sites (`ast_emitter.cpp`, `cpp_generator.cpp`,
both weak-pointer-reset checks on a field name) were deliberately left on
the original, unscoped query, since their semantics were not verified safe
to change. This loop's free-function branch also needed the SAME
qualified-name dual-registration fix as bug 3 (a separate instance, not the
same code path — `promoteReturnsOfSharedLocals` and
`inferAliasReturningFreeFunctions` are two different passes that both
happened to only register the bare name).

### Applying it to yaml-cling's real `yaml_node.clg`

```
bool contains(string entryKey) {
    if (!child) { return false; }
    return child.hasSibling(entryKey);
}

Node cloneChain(Node node) {
    if (!node) { return nullptr; }
    Node copy = new Node(node.kind);
    copy.key = node.key;
    copy.scalar = node.scalar;
    copy.child = cloneChain(node.child);
    copy.next = cloneChain(node.next);
    return copy;
}

Node Clone(Node node) {
    if (!node) { return nullptr; }
    Node copy = new Node(node.kind);
    copy.key = node.key;
    copy.scalar = node.scalar;
    copy.child = cloneChain(node.child);
    return copy;
}
```

`cloneChain` must be declared BEFORE `Clone` in source order (not after,
despite `Clone` being the public-facing entry point) — CeeLing has no
forward-declaration syntax, so a sibling free function calling another must
be declared in call order (a function calling ITSELF, unlike calling a
sibling, is always legal without one — the same constraint Step 25's
emitter-design writeup already found for `yaml_emitter.clg`'s
map/sequence/scalar handling). `cloneChain` is `Node`-recursive (clones a
node AND its whole `.next` sibling chain) while `Clone` itself only clones
ONE node's own subtree — a caller holding "this one specific Node" should
get back a clone of that node's subtree, not accidentally also the rest of
its parent's sibling chain.

### Verification

Two new CeeLing safety fixtures:
`readonly_free_function_param_accepted.clg` (a read-only parameter's
argument survives the call, compiled AND run — single-line output, per the
`//@ run:` harness's documented one-line-only limitation, found again while
writing this fixture) and
`readonly_free_function_param_still_moves_when_stored_rejected.clg`
(positive control — a parameter that IS stored/returned still moves
normally). Full CTest + 89-fixture safety suite green (the bug-2 regression
was caught and fixed before this count was reached, not silently left red).

`yaml-cling`'s real `contains`/`Clone` verified via a new
`tests/run_contains_clone_smoke_rust_backend.sh` (real separate
compilation, mirroring the parser/emitter/iterator/loadfile smoke tests):
`contains` on a present vs. absent key, `Clone` on a map (mutating the
clone's value leaves the original untouched) and on a sequence (cloned
items iterate in the same order). Output cross-checked byte-for-byte
against real yaml-cpp's own `contains`/`Clone` on an identical tree
(compiled from yaml-cpp's actual source, the same method used since Step
24) — byte-identical on every line. Full regression on both repos (CeeLing's
`ctest`; yaml-cling's 8 bash smoke scripts + `ctest` + the standalone
`examples/consumer_project`) stayed green throughout.

## Step 30 — Free-function `operator<<` + `ostream` passthrough type, for `YAML::operator<<`

yaml-cling wanted `std::cout << node;` — real yaml-cpp's `std::ostream&
operator<<(std::ostream& out, const Node& node)` (`node/emit.h:26`), the
last item on `notes/API_COMPATIBILITY.md`'s "Small" bucket. Confirmed via
direct read of `src/emit.cpp` that real yaml-cpp's implementation is
genuinely a thin wrapper equivalent in output to `Dump(node)`, just
streamed directly instead of returned as a string. Initially assumed
trivial for exactly that reason — investigation found two real, confirmed
gaps before any code was written, and implementation surfaced two more.

### The two gaps found before writing any code

1. **`operator_method_name` (the 5-operator allow-list: `[]`/`=`/`*`/`++`/
   `!=`) is scoped to `method_declaration` — CLASS methods only.**
   `function_declaration` (free functions) accepted only a plain
   `$.identifier` as its name. Real yaml-cpp's `operator<<` is a free
   function (not a `Node` member), so this needed new grammar, not reuse of
   the existing set.
2. **`ostream` had never been used as a CeeLing type anywhere.** `$.type`
   already accepts any bare `$.identifier`, so `ostream` PARSES with zero
   grammar change — but `AstEmitter::buildFunctionSignature`'s non-class-type
   fallback would emit a bare, unqualified, by-VALUE `ostream out`. This is
   not merely non-idiomatic: `std::ostream`'s copy constructor is deleted,
   so passing it by value does not compile at all, confirmed via direct
   g++ error before designing the fix.

Fixed narrowly: `function_declaration`'s name gained `choice($.identifier,
'operator<<')` (a separate, free-function-only allowance — NOT added to
`operator_method_name`, which stays the fixed class-method set unchanged).
Zero `ast_builder.cpp` changes needed (confirmed via probe:
`FunctionDeclaration::name` is already a plain string, name-agnostic, the
same as `operator[]`/`operator=` needed on the class-method side). New
`library_summary.h::isOstreamType` (bare `ostream` only); `AstEmitter::
buildFunctionSignature` checks it FIRST — before value-type/class-type — at
both the return-type and parameter sites, always emitting `std::ostream&`
(the only legal shape; not a choice the way value-type-vs-smart-pointer is
elsewhere). The call site `std::cout << node;` needed ZERO emitter changes:
it's an ordinary `BinaryExpression` (`<<`), already emitted generically
before any user `operator<<` existed. `DanglingChecker` needed zero changes
either — its `BinaryExpression` handling already treats both `<<` operands
as a read (not a move), and `isObjectType` already excludes anything not in
`classNames_`, so `ostream` is automatically excluded from move-tracking
the same way `string` already is.

### Bug 1 — `<<` never goes through the ordinary call-forcing path

The read-only free-function-parameter forcing (Step 29) only ever triggers
from inside a genuine `FunctionCall` AST node's visitor branch. But
`std::cout << node` is a `BinaryExpression`, never a `FunctionCall` — the
grammar/AST model `<<` uniformly this way, the same node `std::cout << x`
already used long before any user `operator<<` existed. Confirmed via a
minimal probe (`ostream operator<<(ostream out, Foo f) { ...; return out;
}`, called as `std::cout << a;`): `g++` rejected it with `use of deleted
function 'std::unique_ptr<...>::unique_ptr(const unique_ptr&)'` — the exact
same failure shape Step 29's `Clone` fix addressed, just reached through a
path the existing forcing code never visits. Fixed by extending
`promoteSharedCallArguments`'s `BinaryExpression` visitor: for a `<<`
operator whose right operand is a bare identifier of a class type matching
a declared `operator<<`'s (read-only) second parameter, force BOTH the
parameter and the argument to `shared_ptr` — the identical mechanism, just
triggered from a different AST shape.

### Bug 2 — the read-only classifier's own documented limit became a real blocker

yaml-cling's actual `operator<<` body:

```
ostream operator<<(ostream out, Node node) {
    out << Dump(node);
    return out;
}
```

passes `node` bare into `Dump(node)`. Step 29's `paramUseDisqualifiesReadOnly`
had an explicitly documented, deliberate conservatism: "this pass does not
attempt the passed-onward-to-another-read-only-parameter fixed-point case
... since no known yaml-cling shape needs it yet." That shape now existed —
`operator<<`'s own parameter was wrongly disqualified even though `Dump`
itself only ever reads its parameter's fields. Closing this properly (not
working around it) required a genuine fixed point, mirroring
`inferAliasReturningMethods`'s already-established multi-round algorithm:
`classifyReadOnlyFreeFunctionParams` now loops rounds, each round building
an `okCallees` set (`"fnName::argIndex"` for every already-classified
read-only parameter) from the PREVIOUS round's results, re-running both the
disqualification check AND the genuine-read check with it. A bare-identifier
argument passed to an already-classified read-only parameter neither
disqualifies the caller NOR satisfies its genuine-read requirement on its
own — it's treated as transparent, deferring to whatever the callee itself
already proved.

**A sub-bug found getting this right**: fixing disqualification alone was
not enough — `Dump` still failed to classify, because `bodyHasGenuineRead`/
`exprHasGenuineRead` (Step 29's positive-evidence requirement) had NO
matching "counts via an already-good callee" exception; `Dump` never
touches `node.field` directly itself, only `emitNode` (which `Dump` calls)
does. Fixed by giving the genuine-read walk the identical `okCallees`
exception the disqualification walk got. Confirmed via debug instrumentation
during development (temporary, removed before finalizing) that `Dump`
classified `dq=0 gr=0` after only the first fix — disqualification cleared,
genuine-read still failing — pinpointing exactly which half needed the
second fix, rather than guessing.

**A second sub-bug — the qualified-name mismatch class recurred a FOURTH
time this session**: after both fixed-point fixes, the standalone
`yaml_emitter.clg` transpile correctly showed `operator<<`'s parameter as
`shared_ptr`, but a SEPARATE driver `.clg` (`#include`-ing both `yaml_node.
clh` and `yaml_emitter.clh`) still emitted `root` as `unique_ptr`. Traced to
the SAME class of bug as Step 27's ADL forcing and Step 29's `Clone`
forcing: the `<<`-BinaryExpression forcing code compared the driver's
`root` (declared `YAML::Node`, qualified — the spelling a `.clg` file
writes from OUTSIDE the namespace) directly against `operator<<`'s own
`paramType` (bare `Node`, as declared INSIDE the namespace) — never equal.
Fixed by stripping the qualifier off the right-hand-side's resolved type
before comparing, the same `rfind("::")` pattern used at every prior
instance of this bug.

### Verification

New CeeLing safety fixture `operator_stream_insertion_accepted.clg` (a free
`operator<<` declared and used via `std::cout << x`, the streamed value's
usability confirmed afterward — compiled AND run, not just parsed). Full
CTest + 90-fixture safety suite green.

`yaml-cling`'s real `YAML::operator<<` verified via a new
`tests/run_operator_stream_smoke_rust_backend.sh` (real separate
compilation, mirroring the contains/Clone smoke test): builds a map,
streams it via `std::cout << root;`, then confirms `root` is still usable
afterward. Output cross-checked byte-for-byte against real yaml-cpp's own
`operator<<` on an identical tree (compiled from yaml-cpp's actual source,
the same method used since Step 24) — byte-identical, including the
"still usable" line proving the caller's `Node` genuinely survives the
call, matching real yaml-cpp's `const Node&` semantics exactly. Full
regression on both repos (CeeLing's `ctest`; yaml-cling's 9 bash smoke
scripts + `ctest` + the standalone `examples/consumer_project`) stayed
green throughout.

## Step 31 — `==`/`!=` identity comparison: a discovery, not a feature

yaml-cling wanted `Node::operator==`/`operator!=`, next in line on
`notes/API_COMPATIBILITY.md`'s Medium bucket. yaml-cling's own the internal notes had
a wrong assumption about the target semantics: it claimed real yaml-cpp's
`operator==` compares STRUCTURE/scalar VALUE, contradicting its own note two
sentences later that it "matches yaml-cpp's `is()` semantics" — `is()` is
explicitly IDENTITY (same underlying storage), not value. Reading yaml-cpp's
actual `include/yaml-cpp/node/impl.h` directly confirmed: `operator==(const
Node&, const Node&) { return lhs.is(rhs); }` — pure identity, always.

### The investigation found this already works, with zero code changes

Before writing anything, a direct probe (`a == aAlias`, `a != b` on ordinary
CeeLing class-typed locals, then confirming both operands stayed usable
afterward) was tried against the CURRENT `cling.exe` — and it transpiled,
compiled, and ran correctly with NO changes at all. The reason, once traced:

- `==`/`!=` are already ordinary comparison operators in `binary_expression`
  (`grammar.js`'s comparison production: `choice('==', '!=', '<', '>', '<=',
  '>=')`) — completely ungated by `operator_method_name`'s 5-operator
  allow-list, which only ever governed USER-DEFINABLE operator overloads on
  a class, never built-in comparison syntax itself.
- A class-typed operand is always smart-pointer-backed (`std::shared_ptr<T>`
  or `std::unique_ptr<T>`), so `a == b` in the generated C++ compiles as
  `std::shared_ptr<T>::operator==` — a REAL, BUILT-IN C++ standard library
  operator that already does exactly IDENTITY comparison (pointer/control-
  block equality). No CeeLing-side rewriting, no new grammar, no new
  emitter branch, no new checker logic — the existing generic
  `BinaryExpression` emission (unconditional since long before any operator
  overload work this session) already produces exactly the right C++.
- `DanglingChecker` needed nothing either: a comparison is a plain read via
  `BinaryExpression`'s existing generic use-checking, never a move —
  confirmed directly by the probe, where both compared locals remained
  fully usable afterward.

This mirrors this session's now-familiar "escape by construction" pattern
(Step 27's `operator*`/`operator++` on a value-type operand needed zero
emitter rewriting; the `<<` stream-chain BinaryExpression needed zero
rewriting for `operator<<`) — but taken one step further: here, CeeLing
didn't even need a NEW operator name in the grammar, because `==`/`!=` were
never gated behind the operator-overload allow-list at all, and the
underlying smart-pointer type already provides exactly the right built-in
semantics with no user-defined operator required.

### A real, honest asymmetry, found and documented rather than smoothed over

Cross-checking against real yaml-cpp surfaced a genuine, worth-flagging
difference: compiling a `root != other` probe against ACTUAL yaml-cpp
source failed with a real `g++` error — `no match for 'operator!='` — real
yaml-cpp defines `operator==` for `Node` but never `operator!=` at all.
CeeLing's version works for both (the built-in `shared_ptr` comparison
provides both unconditionally), which is a harmless superset, not a
compatibility gap — but it does mean `root != other` is expressible in
yaml-cling source that the literal equivalent C++ against real yaml-cpp
could not write. Flagged explicitly in both the fixture and the yaml-cling
smoke-test driver's comments, and the cross-check itself only exercises the
`==` half a real yaml-cpp program could actually compile, keeping the
byte-identical claim meaningful rather than comparing against something
that couldn't exist on the other side.

### Verification

New CeeLing safety fixture `class_identity_comparison_accepted.clg` (an
aliased reference compares equal to the original; two independently
constructed objects with identical field values compare unequal —
IDENTITY, not structural equality — both compared values remain usable
afterward; compiled AND run, not just parsed, since this is fundamentally a
runtime-behavior claim). Full CTest + 91-fixture safety suite green.

`yaml-cling` needed ZERO `yaml_node.clg` changes — `Node`'s existing
`shared_ptr`-backed handle shape already provides yaml-cpp's `is()`
semantics for free. New `tests/run_identity_comparison_smoke_rust_backend.sh`
(real separate compilation, mirroring the contains/Clone smoke test),
output cross-checked byte-for-byte against real yaml-cpp's own `operator==`
on an identical tree (only the `==` half, per the asymmetry above). Full
regression on both repos (CeeLing's `ctest`; yaml-cling's 10 bash smoke
scripts + `ctest`) stayed green throughout.

## Step 32 — `as<T>(fallback)`: another already-works discovery, plus a real pre-existing checker bug

yaml-cling wanted `Node::as<T>(fallback)` — real yaml-cpp's
`template <typename T, typename S> T as(const S& fallback) const`
(`node/impl.h`'s `as_if<T,S>`), the never-throws sibling of the existing
throwing `as<T>()`. Confirmed by reading `as_if<T,S>::operator()` directly:
on ANY failure (non-scalar/undefined node, or a failed `convert<T>::decode`)
it returns `fallback` and never throws, under any input.

### The core capability already worked

Before writing anything, a direct probe declared TWO independent primary
generics on the same class — `<T> T as() {...}` (existing) and
`<T> T as(T fallback) {...}` (new) — each with its own per-type
specialization, and it compiled and ran correctly with zero CeeLing
changes. The reason: CeeLing's return-type-genericity mechanism
(`method_type_parameter`/`method_specialization`) was never restricted to
ONE primary per method name — it only requires a primary's return type
equal its own type parameter, checked independently for each declaration.
Two primaries of the same name at DIFFERENT arities simply coexist as two
ordinary methods that happen to share a name (real C++ overloading, no
CeeLing-specific machinery involved at all).

This mirrors real C++'s own actual constraint, confirmed via a FAILING probe
first: a single primary CANNOT host both arities.
`template<> int as<int>(int)` does not compile against a zero-parameter
primary `<T> T as()` — an explicit specialization's parameter list must
match the primary it specializes, a genuine C++ language rule, not a
CeeLing limitation. This is why real yaml-cpp's own `as_if<T,S>` template
is a SEPARATE struct from `as_if<T,void>` (see `node/impl.h`) rather than
one struct handling both call shapes — the same structural split CeeLing's
"two primaries" approach mirrors.

### A real, pre-existing `EscapeHatchChecker` bug, found while verifying the reject side

Before trusting the accept case, a companion probe checked the REJECT
case: calling `b.as<int>(99)` on a class declaring ONLY `as<int>()` (no
fallback overload at all). This should be rejected — the design notes'
"default-reject, never default-allow" contract demands it — but `cling.exe`
exited 0, silently allowing it through. The generated C++ then failed to
compile (`no matching function for call to 'Box::as<int>(int)'`), exactly
the "misparse/mis-accept, not rejection" failure class this project has
found and fixed several times before (Step 7, RB10).

Root cause: `EscapeHatchChecker::isKnownCallable`'s specialization-call
branch checked only that A specialization existed for the requested TYPE
argument (`classMethods_[class].count(specializationKey(name, typeArg))`),
never that the call's own ARGUMENT COUNT matched any of that
specialization's actually-declared arities. `classMethods_` is a bare
existence set (`std::set<std::string>`) — it can answer "does `Class` have
a member keyed `as<int>`" but has no way to answer "at what arity."

**This is a genuinely pre-existing gap, not introduced by `as(fallback)`**
— it was simply unreachable before, since every specialization on every
existing class had always been the ONLY one under its type argument.
`as<T>(fallback)`'s two-primaries-same-name shape is the first time two
specializations legitimately share a type argument at different arities.

Fixed with a new `classSpecializationArities_` map (`"Class::specKey" ->
every arity actually declared under that key`) — populated for EVERY
specialization unconditionally (not just when a same-type-argument
collision is detected, unlike `class_hierarchy.h`'s `overloadKey`
precedent, which only switches key shapes on a detected collision — arity
correctness needs to hold even for the single-arity common case, so
tracking it always is simpler and more robust than trying to detect when
tracking is "needed"). `isKnownCallable`'s two `templateArg`-carrying
branches (receiver-qualified `n.as<int>(...)` and bare same-class
`as<int>(...)` with implicit `this`) now check the call's own
`call.arguments.size()` against this map after confirming the type match.
Base-class inheritance and namespace-qualified dual-registration are both
handled, mirroring `classMethods_`'s own existing patterns for those two
concerns (a snapshot-then-insert pattern was needed for both, since
inserting into `classSpecializationArities_` while iterating it directly
would invalidate the iteration).

### Applying it to yaml-cling's real `yaml_node.clg`

`as<int>(fallback)`/`as<double>(fallback)`/`as<bool>(fallback)`/
`as<string>(fallback)`, each mirroring its throwing sibling's parsing
logic. The `int`/`double` forms needed new validation helpers
(`isValidIntegerText`/`isValidOctalDigits`/`isValidDoubleText`) that check
the scalar text is well-formed BEFORE calling `std::stoi`/`std::stod` --
CeeLing's `catch` is scoped to `string` payloads only (Phase 1 scope, Step
18), so there is no way to catch a `std::invalid_argument` the way real
C++ could. This is not merely a workaround: real yaml-cpp's own
`convert<T>::decode` doesn't rely on exceptions for its own scalar
conversions internally either (it uses its own string-parsing helpers), so
CeeLing's "validate first" approach matches yaml-cpp's actual strategy, not
just its externally-observable behavior.

### Verification

Two new CeeLing safety fixtures:
`template_specialization_arity_overload_accepted.clg` (both the throwing
and fallback forms declared and called on the same class, correctly
dispatching independently — compiled AND run) and
`template_specialization_wrong_arity_rejected.clg` (positive control — the
exact bug shape above, confirming a call to a nonexistent arity is now
rejected). Full CTest + 93-fixture safety suite green.

`yaml-cling`'s real `as<T>(fallback)` verified via a new
`tests/run_as_fallback_smoke_rust_backend.sh` (real separate compilation,
mirroring the contains/Clone smoke test) across `int`/`bool`/`string`,
including the undefined-node and bad-parse fallback paths and the existing
octal/`.inf` special-case literals still working in the fallback form.
Output cross-checked byte-for-byte against real yaml-cpp's own
`as<T>(fallback)` on identical inputs (compiled from yaml-cpp's actual
source, the same method used since Step 24) — byte-identical on every
line. Full regression on both repos (CeeLing's `ctest`; yaml-cling's 11
bash smoke scripts + `ctest` + the standalone `examples/consumer_project`)
stayed green throughout.

## Step 33 — `Node::operator=(const Node&)` alias-rebind semantics (RB18): a real, multi-part core extension

yaml-cling wanted `Node::operator=(const Node&)` — real yaml-cpp's handle-
rebind assignment: `n = otherNode;` mutates `n`'s underlying storage in
place (`AssignNode`/`node::set_ref`, confirmed via direct reading of
`yaml-cpp/include/yaml-cpp/node/impl.h` and `node/detail/node.h`) so every
OTHER `Node` handle that already aliases `n`'s old storage observes the new
value too. This is fundamentally different from CeeLing's existing
`operator=(string)` support (Step ~22): that case never repoints WHICH
object a variable refers to, it mutates the one object in place. A
same-class assignment (`n = otherNode;`) needs the same in-place mutation,
but with TWO independently-aliasable handles both ending up consistent.

### Why this needed a real structural rewrite, not an annotation

Before RB18, `class Node` in `yaml_node.clg` directly held its own fields
(`kind`/`key`/`scalar`/`child`/`next`); every `Node`-typed variable is a
`std::shared_ptr<Node>` pointing straight at that object. Reassigning one
`shared_ptr<Node>` only repoints THAT ONE variable — it cannot make some
OTHER `shared_ptr<Node>` (aliasing the same object) observe a change,
because there is no indirection layer one level further down for two
handles to share. Real yaml-cpp solves this with a three-layer indirection
(`Node` handle → `detail::node` stable slot → `detail::node_ref` payload,
`shared_ptr`-held); yaml-cling doesn't need the middle "stable slot" layer
(nothing keys off a payload's address the way yaml-cpp's own map/sequence
storage does), so a two-layer split is sufficient:

- **`NodeData`** (new) — the actual payload: `kind`/`key`/`scalar`, plus
  `child`/`next` (see below for why these are `NodeData`, not `Node`).
- **`Node`** — now a thin handle: `NodeData data;` (inferred
  `shared_ptr<NodeData>`) plus every existing query/mutation method,
  rewritten to read/write through `data.field`. `Node::operator=(Node
  other) { data = other.data; }` is the whole mechanism: reassigning `data`
  itself (not `this`) means any OTHER `Node` object that is the exact same
  `shared_ptr<Node>` (two variables sharing one handle) shares the same
  `data` pointer automatically, so both see the new payload.

**A genuine circular-type problem, solved the same way Step 23's
`Iterator`/`Node` split was**: `NodeData` originally needed `child`/`next`
typed `Node` (so a caller holding `node.child` sees it live-update), but
`Node.data` needs type `NodeData` — a two-way reference CeeLing cannot
express (no forward-declaration syntax). Resolved by making `child`/`next`
plain `NodeData` (self-referential only, no cycle with `Node`), moving the
recursive chain-walkers (`siblingCount`/`findOrAppendSibling`/`hasSibling`/
`siblingAt`/`appendScalar`) onto `NodeData` itself (they only ever need the
payload chain, never handle semantics mid-walk), and giving `Node` a SECOND
constructor, `Node(NodeData existingData)`, that wraps an existing payload
as a fresh handle wherever a traversal step (`operator[]`, `Clone`) needs
to hand one back to the caller. `yaml_emitter.clg`'s direct field access
(`node.child`, `cur.key`, `cur.next`) — the only place OUTSIDE
`yaml_node.clg` touching these fields — was replaced with new `Node`
accessor methods (`firstChild()`/`nextSibling()`/`Key()`) that wrap the
`NodeData` chain link on the way out, keeping `NodeData` an internal
implementation detail nothing else ever names directly (mirroring real
yaml-cpp's own encapsulation of `detail::node`).

### Four real CeeLing core gaps found and fixed (confirmed via probe before each fix)

1. **Same-class `operator=` never routed through the user's overload.**
   `AstEmitter`'s `AssignmentStatement` handling had a DELIBERATE, documented
   exclusion (from the original `operator=(string)` work): `n = otherNode;`
   where `otherNode` IS the same class always fell through to ordinary
   owner-reassignment (a plain `shared_ptr`/`unique_ptr` repoint), never
   calling the user's `operator=`. Fixed by adding a second branch: when the
   class declares an `operator=` whose OWN parameter type is the class
   itself, route through `->operator=(...)` for a same-class RHS too — kept
   as a genuinely separate condition from the pre-existing string-RHS branch
   (different RHS-class comparison), not a generalization of it.

2. **Two `operator=` overloads silently collided.** A class declaring BOTH
   `operator=(string)` (pre-existing) and `operator=(Node)` (new) has two
   arity-1 overloads. `classMethodParams_`'s key-selection rule
   (`overloadKey(name, paramCount)` only when a name is genuinely
   overloaded) is ARITY-ONLY — both overloads collapse to the SAME key
   (`"operator=/1"`), and the second declaration silently overwrites the
   first. Confirmed via probe: `nameEntry = "text";` on such a class stopped
   compiling (routed to a raw `shared_ptr::operator=`, which has no
   `const char*` overload) the moment a second `operator=` was added — a
   real regression the arity-based overload key cannot resolve, since BOTH
   overloads share the same arity. Fixed by tracking every `operator=`
   overload's own parameter type independently (`operatorAssignParamTypes_`
   in `AstEmitter`, `handleRebindAssignParamName` in
   `SmartPointerAnalyzer::promoteSharedCallArguments`), bypassing the
   collapsed map entirely for this one call shape.

3. **`resolveDeclaredType` couldn't resolve a chained field access.**
   `EscapeHatchChecker::isKnownCallable`'s string-method allow-list check
   (`v.length()`, `v.substr(...)`) only resolved a bare-identifier receiver
   (`resolveDeclaredType`), so `data.scalar.length()` — receiver is
   `data.scalar`, a `MemberAccess`, not a bare identifier — was rejected as
   an unknown call even though `.length()` is a perfectly legal string
   method. This was ALREADY a latent gap (any class with a string field
   reached through another field would have hit it), just never triggered
   before `NodeData`'s introduction moved every scalar-string access one
   field deeper. Fixed by extending `resolveDeclaredType` to walk a
   `MemberAccess` chain via `resolveObjectType` (which already did this for
   class-typed terminal fields) and return the terminal field's OWN declared
   type, not just class-typed ones.

4. **`inferAliasReturningMethods` didn't recognize `return new
   SameClass(payloadField);`.** The existing alias-returning inference
   (cases a/b/c: `return this`, `return <field>`, `return
   <alias-returning-call>`) assumes a method returns something of ITS OWN
   class directly. RB18's handle-wrapping shape is different: `Node::
   operator[]` returns `new Node(data.child.findOrAppendSibling(...))` — a
   FRESH handle wrapping a field reached through a DIFFERENT class
   (`NodeData`)'s own chain. Added case (d): a `new SameClass(<inner>)`
   return is alias-returning if `<inner>` resolves (via a new
   `resolveInnerFieldChain` helper, walking `classMembersByClass_` across
   whatever class each step's field belongs to, not just the method's own)
   down to a field or an already-alias-returning call on ANY class. Without
   this, `Node.data.child`/`.next` — despite `NodeData`'s OWN chain-walkers
   correctly inferring `shared_ptr<NodeData>` — never propagated that
   sharedness up through `Node::operator[]`'s wrapping return, so `Node`
   itself silently defaulted back to `unique_ptr` at every call site.

### A fifth, genuinely pre-existing `DanglingChecker` bug, exposed (not caused) by RB18

`Parser::parseValueInto`'s shape (`if (cond) { other(target); return; }
target.setScalar(...);`) was flagged use-after-move at the LAST statement,
even though that statement is only reachable when the `if`'s condition was
FALSE (the `if`-branch already returned). Reproduced on an ordinary FLAT
class with no `NodeData` split at all — a real, pre-existing gap in
`DanglingChecker`'s branch-merge logic, just never triggered by any
existing fixture before RB18's restructuring changed which functions'
argument-passing patterns got exercised. `mergeBranchStates`'s conservative
union ("an effect in EITHER branch holds after the `if`") is correct for a
branch that FALLS THROUGH, but wrong for one that unconditionally exits
(`return`/`throw`, or an `if`/`else` where both arms do) — that branch's
effects can never be observed by code after the `if`, since that code is
only reachable via the OTHER path. Fixed with a new `alwaysExits(body)`
helper (checks only the LAST statement, conservative by design — an early
return nested deeper inside a loop does not make the enclosing block itself
unconditionally exit) and a corresponding branch in the `IfStatement`
handler: when exactly one side always exits, the post-`if` state is that
branch's sibling's state directly, skipping `mergeBranchStates`'s union.

### Read-only classification extended from free functions to class methods

Real yaml-cpp's `Parser`-equivalent parsing functions mutate a `Node`
parameter IN PLACE across a set of MUTUALLY RECURSIVE methods
(`parseValueInto` ↔ `parseBlockMapInto` ↔ `parseBlockSeqInto`) — the same
"never stored/returned, only read/mutated through" shape Step 29's
read-only-free-function-parameter inference was built for, just one level
down inside a class, and via IMPLICIT-`this` calls (`parseBlockMapInto(
target, ...)`, no explicit receiver) rather than plain free-function calls.
`classifyReadOnlyFreeFunctionParams` (kept its name for now; classifies
BOTH free functions and methods) was extended: `okCallees` entries are
"fnName::argIndex" for a free function or "ClassName::methodName::argIndex"
for a method; `paramUseDisqualifiesReadOnly`/`exprHasGenuineRead` both
gained a `currentClass` parameter so a bare implicit-this call can try the
method-scoped key before falling back to the free-function key (the two
shapes are indistinguishable at the raw AST level — `call->callee` is null
either way). A new `isReadOnlyMethodParam(class, method, param)` query
mirrors `isReadOnlyFreeFunctionParam`. Forcing the caller's argument to
`SHARED` for a read-only METHOD parameter needed its own small traversal in
`promoteSharedCallArguments` (`forceReadOnlyMethodArg`/
`forceReadOnlyMethodArgsInBody`) — the existing `visitExpr`/`visitStmt`
walk has no class-context parameter, and retrofitting one onto that
already-large, deeply-recursive pair was judged riskier than a second,
narrowly-scoped walk built specifically for implicit-this calls.

### Two more real gaps found closing out the full pipeline

Getting the ENTIRE yaml-cling regression suite green (not just
`yaml_node.clg` in isolation) surfaced two more real, confirmed gaps:

6. **`SubscriptExpression` wasn't counted as a genuine read** by the
   read-only-parameter classifier (`exprHasGenuineRead` only recognized
   `paramName.field`, never `paramName[index]`). `Parser::parseBlockMapInto`
   touches its `target` parameter ONLY via `target[key]` — never a bare
   `.field` access — so it never satisfied the positive-evidence
   requirement, which cascaded: `parseValueInto` calls `parseBlockMapInto`
   with `target` passed bare, so IT never qualified either, all the way up
   to `parseDocument`'s `root` showing as moved-from. Fixed by adding
   `SubscriptExpression` handling identical in shape to the existing
   `MemberAccess` case.
7. **`begin()`/`end()`/`Clone`'s parameter-sharing forcing only fired for a
   call site in the SAME transpile unit.** CeeLing compiles each `.clg`
   file as a genuinely separate translation unit (confirmed via
   `main.cpp`'s `mergeExternalDeclarations`) — `yaml_node.clg`'s own
   standalone transpile (defining `begin`/`end`/`Clone`) never contains a
   `for`-loop or a `Clone(...)` call itself; those live in a SEPARATE driver
   `.clg` that only `#include`s the header. The existing forcing logic
   (Step 27's range-for forcing; Step 29's read-only-parameter forcing)
   only fires when the triggering AST shape is found in the same program
   being analyzed, so `yaml_node.h`'s already-emitted signature stayed
   `unique_ptr` regardless of what the OTHER file's analysis concluded.
   Fixed by forcing both unconditionally instead: any free function
   literally named `begin`/`end` taking one class-typed parameter, or any
   free function parameter already classified read-only, is forced SHARED
   regardless of local call-site evidence — these are structural
   requirements of the function's own PURPOSE, not usage-count heuristics.

### A second identity-comparison consequence: `==`/`!=` needed re-deriving

RB16 (`==`/`!=` already working via `shared_ptr<Node>`'s own built-in
identity comparison, zero CeeLing changes) was correct ONLY because `Node`
WAS its own payload — one handle meant one node. Once the handle/payload
split landed, two DIFFERENT `Node` handles can legitimately alias the SAME
`NodeData` (e.g. two separate `root["name"]` calls resolving to the same
map entry), which yaml-cpp's real `is()` semantics say must compare equal
— but `shared_ptr<Node>::operator==` compares HANDLE identity, which these
two objects do NOT share. Confirmed as a real regression via a
`root["name"] == root["name"]` probe wrongly returning false.

Fixed with an explicit `Node::operator==`/`operator!=(Node other) { return
data == other.data; }`, comparing the payload's own `shared_ptr<NodeData>`
identity — but this needed two further CeeLing extensions, since `==`/`!=`
are ordinary `BinaryExpression`s comparing `shared_ptr<T>` VARIABLES at the
generated-C++ level, never reaching a class's own `operator==` at all
through plain operator resolution:

- **Grammar**: `operator_method_name` gained `'operator=='` (previously
  only `[]`/`=`/`*`/`++`/`!=` were legal class-method operator names).
- **`AstEmitter` routing**: `emitExpression`'s `BinaryExpression` handling
  gained a branch that, for `a == b`/`a != b` where both operands are the
  SAME class and that class declares the matching `operator==`/`!=`, emits
  `a->operator==(b)` explicitly instead of the plain `(a == b)` `shared_ptr`
  comparison. Two more bugs found getting this right:
  - The method-name LOOKUP key needed the full `"operator=="` string
    (`classMethodParams_` keys by `method->name` verbatim), not the bare
    `"=="` symbol — confirmed via probe after the check first silently
    never matched.
  - **Iterator's own internal `current != other.current`** (comparing its
    own `Node`-typed field for RANGE-FOR TERMINATION, needing to work when
    `current` is a genuinely null `shared_ptr`, i.e. the `end()` sentinel)
    had to be EXCLUDED from this routing — calling `this->current->
    operator!=(...)` on a null receiver is undefined behavior, confirmed as
    a real segfault. The correct exclusion signal is "the left operand is a
    bare FIELD of the class currently being emitted" (`isCurrentField`),
    not merely "inside some class" — a genuine external comparison like
    `root == other` in `main()` is unaffected.
- **`SmartPointerAnalyzer` forcing**: `operator==`/`!=`'s own parameter is
  forced `shared_ptr` unconditionally (same reasoning as begin/end/Clone
  above), and the RHS argument at each `==`/`!=` call site is forced too
  (mirroring the existing `<<`-BinaryExpression forcing pattern for
  read-only parameters).

### Status: COMPLETE, full pipeline green

Core mechanism verified end-to-end via minimal probe (two `Node`-shaped
handles sharing one payload, reassigning one through `operator=` makes
BOTH observe the new value — compiled and RUN). All 13 real CeeLing core
bugs/extensions found this step (5 structural + 2 pipeline-closing + the
operator==/!= consequence's own 3-part fix, plus the original operator=
mechanism itself) are fixed and verified.

**Final regression**: CeeLing's `ctest` (13 tests including the
safety-fixture baseline) stayed green after every single fix, re-run
after each change, zero regressions. yaml-cling's full 12-script bash
smoke suite is green except `run_node_smoke.sh`, a PRE-EXISTING (confirmed
present before this step's changes, not caused by them), already-known
CRLF-only shell-comparison artifact — the actual program output is
byte-identical to the expected file once line endings are normalized
(`diff` after `tr -d '\r'` shows zero difference). Two test fixtures
(`tests/iterator_smoke_main_rust_backend.clg`,
`tests/loadfile_smoke_main_rust_backend.clg`) needed a mechanical `.key` →
`.Key()` rename to match `Node`'s new public accessor (the field itself
moved under the internal `NodeData` payload).

## Step 34 — `convert<T>` customization point: out-of-class method specializations, plus two real pre-existing analyzer bugs

yaml-cling wanted yaml-cpp's `convert<T>` mechanism: a LIBRARY CONSUMER
extends `Node::as<T>()` to their OWN type from OUTSIDE `yaml_node.clg`,
without editing yaml-cling itself. This is a genuinely new grammar shape —
`rust_backend` had no out-of-class method-definition syntax anywhere before
this (every method body lives inside its `class_body`).

### The grammar/AST shape

New production `external_method_specialization`: `ReturnType
[Namespace::]ClassName::methodName<Type>(params) { body }` at TOP LEVEL,
specializing an EXISTING primary generic method (Step's return-type-generic
mechanism, `<T> T methodName() {...}`) declared inside `ClassName`'s own
class body — possibly in a DIFFERENT `.clg` file reached via `#include`.
`ClassName` is `choice($.identifier, $.scoped_identifier)`, so a namespaced
target (`YAML::Node::decodeInto<Point>(...)`) parses.

The parsed `MethodDeclaration` (`isExternalSpecialization = true`) is
SPLICED directly into its target `ClassDeclaration::members` — the same
"flatten into an existing collection, leave ~40 downstream lookups
unchanged" pattern namespace-flattening and inheritance already established.
This is what let `EscapeHatchChecker`/`SmartPointerAnalyzer`'s ownership
analysis/arity checking work with **zero changes**, confirmed via probe
(arity-mismatch call correctly rejected; valid call correctly accepted and
ran) before any emitter work began. Splice matching accepts either the
target class's bare or namespace-qualified name (`classMatchesSpecializationTarget`).

### The emission-ordering problem — the actual hard part

The first, natural-seeming design — emit a specialization's
declaration+definition immediately after its target class's own body — is
wrong whenever the specialization references a type declared LATER in the
program. Confirmed via probe: a same-file `class Node {...}` (with the
primary) followed by `class Point {...}` followed by `Point
Node::as<Point>() {...}` produced `'Point' was not declared in this scope`,
because the specialization was emitted right after `Node`'s body, BEFORE
`Point` existed in the generated file at all.

Worse, this is not just a same-file ordering nuance — it is structurally
impossible to fix by reordering in the `#include` case: `main.cpp`'s
`mergeExternalDeclarations` always inserts merged/external statements at
the FRONT of `program.statements` (confirmed by reading it directly), so a
`#include`d target class is invariably positioned before the consumer's own
code. Worse still, a merged class is `isExternalDeclaration = true` and is
therefore **never re-emitted at all** in the consuming translation unit
(`emitTopLevel` returns immediately for it) — so "emit right after the
target class's body" has no meaningful position to even attach to.

**Fix**: a new `ExternalSpecializationMarker` statement type (`ast.h`) — a
non-owning placeholder (raw `MethodDeclaration*`, real ownership stays in
`target->members`) inserted into `Program::statements` at the
specialization's own ORIGINAL top-level source position. This is the ONE
position ordinary source-order rules guarantee is safe: CeeLing has no
forward declarations, so anything the specialization's signature/body
references must already have been declared before it, in the consumer's
own source, by construction. `AstEmitter::emitTopLevel` dispatches the
marker to a new `emitExternalSpecializationMarker`, which emits BOTH
declaration and definition together (`template<> ReturnType
ClassName::method<Type>(...) {...}`) at that position; every other pass
(checkers) doesn't recognize the marker type and simply skips it, the same
way `IncludeStatement` is already invisible to them.

### Cross-file resolution needed a two-phase splice

`ASTBuilder::buildProgram`'s own splice pass runs BEFORE `main.cpp` calls
`mergeExternalDeclarations` — so a same-file target resolves immediately,
but a `#include`d one genuinely cannot yet (the class doesn't exist in
`program->statements` at splice time). Fix: an unresolved specialization is
deferred onto a new `Program::unresolvedExternalSpecializations` list
instead of erroring immediately inside `buildProgram`; `main.cpp` calls a
new `ASTBuilder::spliceExternalSpecializations` a SECOND time, right after
`mergeExternalDeclarations` — at which point "still not found" really does
mean unknown, and is the same hard
`error[semantic/unknown-specialization-target]` as before.

### `T` in parameter position — a new genericity slice

yaml-cpp's real `convert<T>::decode(const Node&, T&)` shape needs an
OUT-PARAM, not a return value. CeeLing's generics previously supported
ONLY `<T> T method()` (T as the return type). Relaxed
`ASTBuilder::buildMethodDeclaration`'s validation to also accept `<T>
ReturnType method(T param)` — T in exactly one parameter, and NEVER both
return type and a parameter (ambiguous, rejected) — while the pre-existing
`<T> T as(T fallback)` shape keeps working unchanged, since there `T` is
the return type and the parameter is an ordinary same-typed value, not the
new out-param case (`method->returnType != method->typeParameter` gates
which shape a given `T`-typed parameter means).

### The mutable out-parameter mechanism (deliberately not called "borrow")

The one parameter whose PRIMARY declares as literal `T` (in the
param-position slice above) needs to be WRITTEN to by the callee but
SURVIVE at the caller — the opposite of CeeLing's default move-in-by-value
parameter semantics. Initially implemented and internally named around the
word "borrow" before a mid-session correction: the design notes' roadmap lists a
general, user-facing `borrow<T>` ownership annotation as a **future,
optional** feature that has never been designed — this mechanism is NOT
that. It is narrow and purely mechanical: it only ever fires for a
generic's own `T` used in parameter position, is never exposed as an
annotation a user can apply to an arbitrary parameter, and shares none of
`borrow<T>`'s intended design space. Every identifier/comment was renamed
`outParamIndex_`/`outParamKey` (an ordinary cross-language term for
"parameter used to convey a result back to the caller") specifically to
avoid implying the roadmap feature had been built.

Mechanism: lowered exactly like an existing `ref` observer local — a raw,
non-owning pointer (`T* pname` in the signature; `Point* pname` once `T` is
substituted for a concrete specialization, since both share ONE index by
construction), `.get()` at the call site (including an IMPLICIT-`this`
call, e.g. `as<Point>()`'s own body calling `decodeInto(out)` with no
explicit receiver — a real gap found via probe, since the emitter's
receiver-class resolution for the borrow-index lookup originally only
handled the explicit-`MemberAccess`-callee shape), `->` access in the body.
`DanglingChecker::applyCallArgMoves` skips the move for this one argument
index — `outParamIndex_` is a `(class, methodName) → paramIndex` map in
both `DanglingChecker` and `AstEmitter`, dual-registered under the
namespace-qualified class name (the same dual-registration pattern
`classMethodParams_`/`classMethodReturnType_` already use).

### The return-side mismatch: a specialization returning its own concrete type

A specialization whose return type IS its own concrete specialized type
(`Point Node::as<Point>()`, unlike `bool`-returning `decodeInto<T>`) hits
the mirror-image of the out-param problem: real C++ requires an explicit
specialization's return to match the primary's T-substituted type EXACTLY
— bare `Point`, never `unique_ptr<Point>`/`shared_ptr<Point>`. Confirmed
via probe (`g++` error: `template-id 'as<Point>' ... does not match any
template declaration`, listing the primary's `T Node::as()` as the only
candidate, when the specialization's return was smart-pointer-wrapped).

Fix: `AstEmitter::currentSpecializationValueType_`, set only while emitting
such a specialization's own signature+body (`emitExternalSpecializationMarker`
detects `method->returnType == method->specializationArg`). `isValueType`
treats that ONE concrete type as bare/unwrapped while the flag is set —
governing the specialization's own return type, its own body's local
declarations of that type (`Point out = Point(0, 0);`, not
`std::make_shared<Point>`), and its own `return out;`. The flag is cleared
the instant emission of that one function finishes, so the SAME class used
elsewhere (a field, a different function's local) is completely
unaffected — confirmed by re-running the FULL `ctest` safety-fixture suite
after this change with zero regressions.

This ripples one step further: a CALL SITE assigning such a specialization's
result to a local (`Point p = doc.as<Point>();`) also needs bare emission,
not the ordinary "class-typed method return → smart pointer" rule. New
`AstEmitter::isBareValueReturningSpecializationCall` detects this via
`classMethodReturnType_`'s existing specialization-keyed entries (`"as<Point>"
→ "Point"`) — a call is bare-value-returning iff the registered return type
equals the call's own `templateArg`. Gated `isClassType(decl->type) &&
isBareValueReturningSpecializationCall(...)`, checked BEFORE the ordinary
`isClassType` smart-pointer branch — the `isClassType(decl->type)` guard
matters: without it, `int x = n.as<int>()`/`string s = n.as<string>()`
(pre-existing, working fixtures) would ALSO match (since `"int" ==
"int"`/`"string" == "string"` trivially satisfies the same condition,
`classNames_` correctly excludes primitives), a real regression caught by
re-running `template_as_basic_accepted.clg` before landing this fix.

### `this` used as an explicit expression — a small, separate, real gap

A specialization's body naturally wants `this["key"]` (subscript on the
implicit receiver) or to store `this` into a local. `AstEmitter::
exprObjectClass` had no case for a bare `this` `Identifier` used this way
(only `return this;`, a different code path, was special-cased) — so
`this["key"]` emitted as a literal, invalid C++ pointer-subscript instead
of routing through `Node`'s own `operator[]`. Fixed with one new early
branch: `id->name == "this"` resolves to `currentClass_` directly, before
the field/`varClass_`/`observerClass_` checks — `this` is never itself a
variable name recorded in any of those maps, so this cannot collide with
anything.

### No fallback to the unspecialized primary — a real, accepted API-surface limit

Unlike real C++, CeeLing's specialization-call resolution
(`EscapeHatchChecker::isKnownCallable`) requires an EXACT `as<Point>`
specialization to exist for `doc.as<Point>()` to be callable at all — it
does not fall back to the unspecialized primary the way real template
resolution does (`isKnownCallable`'s own comment already documented this
as intentional, predating this feature: "a specialization must exist for
the exact requested type... there is no implicit conversion between
specializations"). Consequence: `decodeInto<Point>` alone is NOT enough to
make `.as<Point>()` callable — a consumer must ALSO write a thin
`as<Point>` wrapper (`Point out = new Point(...); if (!decodeInto(out))
throw ...; return out;`). Documented as the required two-specialization
pattern rather than a gap to close: closing it would mean teaching the
checker real C++ template-partial-ordering/fallback rules, a materially
bigger change than this feature needed.

### Two real, PRE-EXISTING analyzer bugs, found only because this feature exercised paths nothing else had

**Bug 1 — bare-method-name key collisions in `SmartPointerAnalyzer`.**
Several passes (`promoteReturnsOfSharedLocals`, `inferAliasReturningMethods`,
`collectCalleeParams`, `classifyReadOnlyFreeFunctionParams`) key
`methodReturnKinds_`/equivalent maps by BARE method name
(`makeMemberKey(cls->name, method->name)`), with no notion of
`specializationKey` at all. Confirmed via bisection: adding `Point
Node::as<Point>() { Point out = ...; return out; }` alone flipped
`Node::as`'s registered return-kind to SHARED (the specialization's own
`out` local, analyzed by `promoteReturnsOfSharedLocals` as if it belonged to
the PRIMARY `as`, fell through `getPointerTypeForVariableInScope`'s
not-found-defaults-to-SHARED quirk) — which in turn corrupted an entirely
UNRELATED call chain (`YAML::Load` → `Parser::parseDocument`) that happened
to read the same bare `"Node::as"` key. Fixed by excluding
`!method->specializationArg.empty()` members from every one of these
passes — a specialization's own return/param lowering is already handled
by the narrower, correct mechanisms described above, so these bare-name
passes have nothing legitimate to contribute for one anyway.

**Bug 2 — `symbolTypesByScope`'s `kGlobalScope` fallback has no real
scoping, and roughly a dozen read sites relied on it exclusively.**
`registerSymbolType` writes every local under BOTH its precise scope
(free-function name / `"main"` / `"Class::method"`) AND, unconditionally,
a `kGlobalScope` fallback entry keyed by BARE VARIABLE NAME ALONE — a
deliberate best-effort fallback, but one that is last-write-wins with zero
scoping. `inferAliasReturningFreeFunctions`, `promoteSharedCallArguments`
(and its several nested closures), and `scanContainerElementKindsInStatement`
all queried ONLY that fallback. Confirmed via direct bisection and a debug
trace (`getMethodReturnPointerType`'s query logged): a consumer's own
`main()` declaring `Point p = doc.as<Point>();` silently corrupted
yaml_parser.clg's `Load`'s UNRELATED `Parser p = new Parser(text); return
p.parseDocument();` — `Load`'s resolution of its own `p.parseDocument()`
call picked up `main`'s `p: Point` instead of its own `p: Parser`, purely
because both got registered under the SAME global, unscoped fallback key
and `main`'s registration happened to run later. This is a genuinely
pre-existing bug (predates `convert<T>` entirely — any two functions
anywhere sharing a locally-scoped variable name of different types could
collide), just never previously triggered. Fixed by threading the precise
scope through every one of these call sites (a new `currentScope`/
`scopeName` variable, set at each function/method/`main` entry point) and
trying it BEFORE the `kGlobalScope` fallback everywhere — `registerSymbolType`
itself and its precise-scope write path were already correct; only the
read sites needed the fix.

### Status: COMPLETE, full pipeline green

Verified end-to-end with real separate compilation: `yaml-cling/tests/
convert_smoke_main_rust_backend.clg` declares `class Point`, both
`YAML::Node::decodeInto<Point>`/`YAML::Node::as<Point>` external
specializations, and calls `YAML::Load("x: 3\ny: 4\n").as<Point>()` — each
of `yaml_node.clg`/`yaml_scanner.clg`/`yaml_parser.clg`/the driver
transpiles to its own `.cpp`, compiles independently, links, and RUNS,
printing `x=3 y=4` (`run_convert_smoke_rust_backend.sh`).

**Final regression**: CeeLing's `ctest` (13 tests including the
safety-fixture baseline) green after every fix, re-run after each change.
yaml-cling's full 13-script bash smoke suite green (the pre-existing
`run_node_smoke.sh` CRLF-only artifact, documented in Step 33, is the sole,
unrelated, already-known exception).

## Step 35 — Closures: by-value capture only, `[&]` sealed by construction, plus two real pre-existing bugs on the escape path

Driven by `imgui-cling`'s recurring need for a Begin/End-shaped API
(`GuiWindow.beginGroup()`/`endGroup()`, guarded only by the advisory-only
`PairingChecker`) — every closure-shaped ImGui API (Group, Table, Tree, Menu)
maps to a real egui closure (`ui.group(|ui| {...})`) that CeeLing had no
equivalent for. The actual design question was narrower and harder than
"add lambda syntax": can a closure be added **without opening a new
dangling-reference hole**, since the design notes' threat #7 is literally "lambda
captures `[&]` local, then escapes."

### Step 0: a mandatory probe, run BEFORE any implementation, that surfaced a real bug

The whole design depends on passing a derived-class instance to a
base-typed PARAMETER (a closure passed to a function expecting its
interface base type is exactly this shape). No existing fixture covered
this — `inherit_virtual_dispatch_accepted.clg` only covers a base-typed
LOCAL. The probe (`class Drawer { Drawer() {...} virtual call... }; class
MyDrawer : public Drawer {...}; void invoke(Drawer d, int) { d.call(...);
}`) failed to compile: `Drawer`'s own explicit `Drawer() { }` collided with
`AstEmitter`'s UNCONDITIONAL synthesis of a second `protected: Drawer() =
default;` for any class used as a base (the synthesis existed to give a
derived constructor something to implicitly chain through — see the
base-class-injection block in `emitClass`/`emitClassHeader` — but never
checked whether the user had already written one). Real g++ error:
`'Drawer::Drawer()' cannot be overloaded with 'Drawer::Drawer()'`. This is a
general, pre-existing bug, unrelated to closures, confirmed to affect any
hand-written base class with its own no-arg constructor. Fixed with
`AstEmitter::hasUserDefinedDefaultConstructor` (walks `cls.members` for an
existing zero-arg `ConstructorDeclaration` before synthesizing one) —
covered by `tests/safety/inherit_base_own_default_ctor_accepted.clg` and
`inherit_base_param_accepts_derived_accepted.clg` (the original probe, made
permanent, since it's load-bearing regardless of closures).

### Syntax: a leading keyword, chosen specifically to need zero grammar conflicts

`closure IFace [captures] (params) { body }`. `new_expression` was the
model: a leading keyword needs no `prec` wrapper because it's unambiguous at
the exact position `$.expression` already appears. A leading `[` (mimicking
C++ lambda syntax) was rejected before writing any grammar: `[` is already
live as `subscript_expression`'s infix operator, which is the *exact* class
of ambiguity `template_call`'s `<` hit against `binary_expression`'s
comparison operator (needing two `conflicts:` entries and GLR
disambiguation). Confirmed after adding the grammar rule: zero new
`conflicts:` entries needed, exactly as predicted.

Capture list is bare identifiers only — no `&`, no `=`. This is the single
decision that makes the rest of this feature tractable: by-reference
capture is not *detected and rejected*, it is **unrepresentable**, the same
"sealed by construction" strategy already used for `ref` fields (no
`ref`-qualified field production exists in the grammar at all).

### Desugaring: eliminated before any checker runs, not analyzed by any checker

`closure IFace [count] (int x) { body }` becomes, via a NEW dedicated pass
(`src/core/generator/closure_desugar.cpp`) run immediately after
`ASTBuilder::spliceExternalSpecializations` and BEFORE
`EscapeHatchChecker`:

```cpp
class __closure_0 : public IFace {
public:
    int count;
    __closure_0(int p_count) { count = p_count; }
    <IFace's sole virtual method> override { body }
};
// the closure expression itself is replaced in-place by:
new __closure_0(count)
```

Modeled on `ExternalSpecializationMarker`'s own stated rationale (Step 34
and earlier in this chapter): "every downstream pass sees an ordinary
`MethodDeclaration` ... with zero changes to any checker." Here, every
downstream pass sees an ordinary `ClassDeclaration` — `DanglingChecker`'s
existing `cls->members` walk analyzes the closure's body with **zero new
checker code**, `EscapeHatchChecker` and `SmartPointerAnalyzer` likewise
need no closure-specific branch. `EscapeHatchChecker` running BEFORE this
pass would have hard-rejected the raw closure syntax as an unknown
construct (it hard-fails on anything unrecognized), which is why ordering
matters here, not just correctness of the desugar itself.

The interface (`IFace`) must be an ordinary CeeLing class with EXACTLY ONE
virtual method — validated by `desugarClosures` itself (not a new grammar
rule), so "which method does the closure override" always has one
unambiguous answer, and the closure's own parameter list is checked to
match that method's signature exactly (a mismatch would otherwise make the
synthesized method an OVERLOAD instead of an OVERRIDE, since `AstEmitter`
emits `override` unconditionally on the synthesized method).

**Constructor parameter naming avoids a real collision found via probe**:
the constructor's own parameter is named `p_<capture>`, not `<capture>`.
Confirmed by probe that CeeLing's `AstEmitter` has no parameter/field
shadowing concept at all (unlike real C++, where a constructor parameter
shadows a same-named field inside its own body) — an
`AssignmentStatement`'s RHS `Identifier` is resolved via the SAME
`isCurrentField` check as the LHS, so a body reading `count = count;` (same
name on both sides) emitted BOTH sides as `this->count`, producing a
nonsensical self-initializing `count(this->count)` in the generated member-
initializer list. Every existing hand-written CeeLing constructor in the
whole codebase already avoids this by using a different parameter name
(`age = a;`, never `age = age;`) — this is not a special case invented for
closures, it's the established convention, applied to a synthesized name.

### The genuinely new piece: a third, dedicated scope-type stack

Resolving each capture's type (`[count]` → is `count` an `int` or a
`string`?) needed a real lookup this compiler did not have anywhere.
Confirmed via direct inspection that neither existing symbol table could
serve it:

- `ASTBuilder` has **no symbol table at all** — it is a syntax-directed
  translator with no per-instance state tracking local variable types.
- `SmartPointerAnalyzer`'s own `symbolTypesByScope` (populated in its first
  pass) is unusable for two independent reasons: (a) it is not populated
  until AFTER `EscapeHatchChecker` already ran in `main.cpp`'s pipeline,
  which would reject the raw closure syntax first, and (b) it unconditionally
  SKIPS every non-class type (`if (classNames.find(ptype) ==
  classNames.end()) continue;`, in both `registerParameters` and
  `analyzeVariableDeclaration`) — because the analyzer only cares about
  object OWNERSHIP, primitives were never worth tracking there. Primitives
  are exactly what closures capture most (`int`/`float`/`string`).

So `closure_desugar.cpp` maintains its own minimal `ScopeStack` (a
`vector<map<string, ScopeEntry>>`, pushed/popped per block, populated from
`VariableDeclaration`/`ContainerDeclaration`/parameters/fields as a
dedicated recursive walker descends `if`/`while`/`for`/`try` bodies) —
new, but deliberately narrow: it only ever answers "what is the declared
type and is-`ref`-observer status of this name, here," nothing more.

### Why by-value capture makes escape SAFE, not just simpler

Design decision (binding, from the project owner): closures may safely
ESCAPE their creating function — stored in a field, returned, outlive the
stack frame that built them. This is exactly what by-value-only capture
buys for free: a captured name becomes an ordinary OWNED field (or, for a
`shared_ptr`-backed capture, a SHARED field), so there is nothing left
pointing at a dead stack frame the way a live reference capture would leave
behind. Concretely, capturing an object by value is EXACTLY "pass this by
value into a constructor" — already-existing move-tracking
(`applyCallArgMoves`) records the move with **zero new lifetime-analysis
code**; using the captured-from variable afterward is already rejected by
the existing use-after-move check. Verified end-to-end
(`tests/safety/closure_escapes_via_field_accepted.clg`): a closure stored
in a field and invoked on TWO separate calls after its creating function
had already returned reads its captured value correctly both times.

### Two more real, general, pre-existing bugs found on the escape path — neither is closure-specific

**Bug 1 — a bare `new Derived(...)` passed directly as a call argument
constructed the WRONG CLASS.** `AstEmitter::emitMovedArg`'s own
`NewExpression` branch called `factoryFor(targetParam.first, ...)` —
passing the CALLEE'S DECLARED parameter type (e.g. `"Animal"`) as
`factoryFor`'s CONSTRUCTED-type argument, silently discarding
`NewExpression::className` (`"Dog"`) entirely. `useIt(new Dog())` into
`void useIt(Animal a)` produced `useIt(std::make_shared<Animal>())` —
compiles, links, runs, and constructs the wrong object (confirmed via
probe: prints "generic" instead of "woof"). This is exactly the shape a
closure passed inline as a call argument has (a `NewExpression`, not a
named local), so it was directly load-bearing for this feature, not a
tangential find. Fixed by passing `neww->className` as the constructed
type and `targetParam.first` as `factoryFor`'s existing `declaredType`
parameter — the SAME declared-vs-constructed split `factoryFor` already
implements correctly for a named local (`Animal a = new Dog();`), now
applied to the inline-argument case too. Covered by
`tests/safety/inherit_inline_new_arg_constructs_declared_type_accepted.clg`.

**Bug 2 — a field storing a constructor parameter, with no other ownership
signal, could disagree with the SHARED default every inline-constructed
argument gets.** `SmartPointerAnalyzer::getPointerType`'s own "not found"
fallback is unconditionally SHARED (confirmed: both of its own internal
branches return `PointerType::SHARED`) — so `new Holder(new Dog())` always
emits a `shared_ptr` argument. But `secondPassDeterminePointerTypes`'s base
case for the FIELD that argument gets stored into is `PointerType::UNIQUE`
("default unique ownership"), a decision made with zero knowledge of how
instances of the class are constructed at call sites. Left alone, `Holder`'s
constructor becomes `Holder(std::unique_ptr<Animal>)` while every real
caller passes a `shared_ptr` — a genuine compile error (`no known
conversion from shared_ptr<Dog> to unique_ptr<Animal>`), reproduced with
plain `Animal`/`Dog`, no closures involved. Fixed in
`SmartPointerAnalyzer::promoteParamsInBody`: when a field has no real
signal of its own — checked by comparing `pointerReasons[key]` against the
literal string `"default unique ownership"`, since `pointerDecisions` has
an entry for EVERY `variableReferences` key unconditionally (confirmed:
`secondPassDeterminePointerTypes`'s loop writes one on every iteration, so
mere presence in the map cannot distinguish "a real rule fired" from
"nothing did," which was the first, broken version of this fix) — and it's
populated from a stored parameter, promote the field to SHARED to match.
Covered by `tests/safety/field_stores_param_matches_inline_new_default_accepted.clg`.

A third, smaller instance of the same class of gap surfaced integrating
this into `imgui-cling`: a BRAND NEW parameter name
(`GroupBody::call`'s own `ctx`) that the SHARED-forcing data-flow heuristic
had never seen before defaulted to UNIQUE, while every OTHER `ctx` in that
codebase already resolves SHARED organically. Fixed with an explicit
`//@ptr shared ctx` hint (CeeLing's pre-existing bare-name annotation
mechanism, not a new feature) rather than by generalizing the inference
further — two independently-named parameters across two different files
agreeing is not something whole-program name-based inference can promise
without a real per-type "this is inherently shared" concept, which is out
of scope for this feature.

### What's explicitly NOT supported, on purpose

- **Nested closures were initially rejected outright at launch, then
  supported shortly after — see the dedicated sub-section below.**
- **Capturing `this`** is rejected — `this` is a borrow of the enclosing
  object, not an owned value; capturing it would need by-reference
  semantics, which this feature does not support by design.
- **Auto-capture (`[=]`/`[&]`)** does not exist — every capture is named
  explicitly, matching CeeLing's existing "no `auto`, every declaration
  states its type" convention.

### Status (initial launch): COMPLETE, full pipeline green, real end-to-end consumer

11 new fixtures in `tests/safety/closure_*.clg` (basic dispatch — must be
`//@ run:`, not just compiled, since virtual dispatch is a runtime property
the same way Step "inherit_virtual_dispatch" already established; escape via
a stored field; multiple mixed-type captures verifying constructor-parameter
ordering and the pre-existing member-initializer-list hoisting applies
automatically to a synthesized class; a constructor-argument use site
exercising `NewExpression`'s own argument list, found to need its own
`rewriteExpr` case since it's a different AST shape than a plain function
call's arguments; and all 8 rejection cases above), run with `--strict` to
confirm each `_rejected.clg` fixture triggers its intended check rather than
merely XFAILing silently. Full safety suite: 117/117. `ctest`: 13/13.

Real end-to-end consumer: `imgui-cling/src/gui_window.clg`'s
`GroupBody`/`GuiWindow.group()` replaced that project's `beginGroup()`/
`endGroup()` Begin/End pair (previously guarded only by the advisory-only
`PairingChecker`, since CeeLing had no closures when that pair was designed)
with a single call taking a closure — no second call exists to forget, a
strictly stronger guarantee than a warning. Required moving `imgui-cling`'s
own `enabled`/`enabled2`/`volume` locals into `Context`'s state maps first,
since a closure can capture a `shared_ptr`-backed handle (by value, sharing
the same live object) but can never capture-and-mutate a plain local and
have that mutation observed by the caller. Verified via direct interaction
with the rebuilt demo: both checkboxes and the slider behave identically to
the pre-closure version.

### Follow-up — nested closures: the blanket ban was an implementation shortcut, not a wall

Driven by `imgui-cling`'s next planned widget (a closure-based Table,
mirroring real `egui_extras::Table`'s own `header(|h| { h.col(|ui| {...});
})` / `body(|b| { b.row(18.0, |r| { r.col(|ui| {...}); }); })` — genuinely
NESTED closures, not just one level). Investigating whether the initial "no
nesting" rule was a hard limit or a scope cut, before designing around it,
found it was the latter: `closure_desugar.cpp`'s `desugarOne` never called
`walkBody` on the closure's OWN body at all — it moved `closure.body`
verbatim into the synthesized override method, unwalked. `walkBody` itself
already fully supported recursive descent through `if`/`while`/`for`/`try`
(pushing/popping scope at each level); the ONLY reason a `ClosureExpression`
sitting inside another closure's body was never found is that the one
function that finds and desugars one (`rewriteExpr`) was never invoked on
that body in the first place. The explicit `containsNestedClosure`
scan-and-reject-up-front check existed purely to catch this gap cleanly
(matching the "unknown node echoed as literal text" failure mode
`NewExpression` arguments hit before their own fix, rather than let it
recur here) — not because a real design obstacle had been hit.

**The one genuine semantic question, resolved before writing any code**:
when an inner closure's capture list names something that was itself part of
the OUTER closure's OWN capture list (now a field of the outer's synthesized
class, reachable only through the outer's own `this`), what should happen?
Two options existed: (a) implicitly let the inner closure capture the
outer's `this` on its behalf — rejected immediately, since it would silently
reintroduce exactly the borrow-capture this whole feature exists to forbid;
(b) copy the value out of the outer's field at desugar time into a field of
the inner's own — a real, narrower option, deferred rather than built,
since it was not needed for the actual motivating use case (Table's own
header/row/col nesting captures loop variables and formatting parameters,
not an outer closure's own captured state).

**Decided instead**: an inner closure may capture ONLY its immediately
enclosing closure's own PARAMETERS — never that closure's own CAPTURES. A
parameter is an ordinary local already in scope inside the method body, not
reached through `this` at all, so capturing it needs zero new borrow/
ownership machinery: it's captured by value exactly like any top-level
function's own parameter already is.

**The fix, once the design was settled, was three lines of consequence, not
three lines of code**: in `desugarOne`, immediately before `closure.body` is
moved into the synthesized `MethodDeclaration`, build a fresh `ScopeStack`
seeded ONLY from `closure.parameters` (mirroring exactly how
`desugarClosures`' own top-level driver seeds a `FunctionDeclaration`'s scope
from `fn->parameters`) and call `walkBody(closure.body, innerScope,
classesByName, pendingClasses)`. `pendingClasses` had to be threaded into
`desugarOne` as a new parameter (previously only `rewriteExpr` held it) so a
nested closure's synthesized class lands in the SAME accumulator the outer
call is using. `containsNestedClosure` and its call site were deleted
entirely — the ban it enforced no longer exists. **No change was needed to
`rewriteExpr`, `walkBody`, or `ScopeStack` themselves** — the recursion
`desugarOne` → `walkBody` → `rewriteExpr` → `desugarOne` (for a nested
closure) falls out automatically once the one missing call was added; depth
is unbounded (3+ levels work identically) since nothing caps the recursion
except a body eventually containing no further closure.

**Ordering falls out for free.** `pendingClasses` is accumulated bottom-up:
because the OUTER closure's own `desugarOne` call walks its body (which
recursively desugars any INNER closure) BEFORE returning its own
`DesugarResult`, the inner closure's synthesized class is always pushed onto
`pendingClasses` strictly before the outer's — required, since the outer's
own synthesized method body references the inner class by name
(`new __closure_N(...)`) and CeeLing has no forward declarations (the same
constraint `ExternalSpecializationMarker`'s own placement logic already
works around, elsewhere in this chapter). No explicit ordering code was
needed for this; it was a consequence of the existing bottom-up accumulation
shape, confirmed by inspecting real generated output
(`class __closure_1 : ...` — the inner one — emitted before `class
__closure_0 : ...` — the outer one — in the compiled probe used to verify
this).

**What still rejects, and why it needed no new rejection code**: an inner
closure capturing an outer closure's own CAPTURE (as opposed to its
parameter) falls straight through to the PRE-EXISTING `closure-unknown-
capture` rejection — the inner closure's scope, seeded only from the outer's
parameter list, simply never contains the outer's captured name, so
`scope.find(name)` returns `nullptr` exactly as it already does for any
genuinely out-of-scope name. Only the `help` text needed a one-line addition
explaining the nested case specifically, so a user hitting this error isn't
left guessing whether it's a typo or a structural rule.

**The existing `closure_nested_rejected.clg` fixture's verdict is unchanged,
its meaning is not**: its inner closure captures `total` (the OUTER's own
capture, not the outer's parameter `x`), so it still correctly rejects —
now via `closure-unknown-capture` instead of the deleted `closure-nested`
check. It was rewritten from "the negative control for a blanket ban" into
"the negative control proving nesting has real, principled limits rather
than none at all." Two new fixtures verify the positive cases end-to-end
(`//@ run:`, not just compile, since dispatch through nested indirection is
a runtime property): `closure_nested_captures_outer_param_accepted.clg` (2
levels) and `closure_nested_three_levels_accepted.clg` (3 levels, confirming
depth isn't artificially capped at 2 — each level captures only its
immediate parent's own parameter, the same rule applied recursively).

**Status**: full safety suite 119/119 (`--strict`), `ctest` 13/13. No
`imgui-cling` changes were made in this pass — the closure-based Table
design that motivated this investigation is a separate, not-yet-started
follow-up; this change only makes nesting possible, it does not design or
build that consumer.

## Step 36 — Chained method calls (`a.b().c()`): a real gap found by comparing CeeLing's own idiom against real ImGui/egui's

Prompted by a design discussion (not a bug report): does CeeLing support the
`if (ui.button("Save").clicked())`-style inline chaining real egui/ImGui code
is normally written in? `imgui-cling`'s own widgets already returned a
`Response` struct and always bound it to a named local first
(`Response r = button(...); if (r.clicked) {...}`) — never confirmed whether
that was a deliberate choice or a real language limitation.

### The investigation, done as probes before touching any source

1. **One-level "call, then field access" already worked**: `ui.button("Save").clicked`
   (a method call immediately followed by reading a FIELD on its return
   value, no second method call) transpiled, compiled, and ran correctly
   with zero changes needed — `member_access`'s grammar rule
   (`tree-sitter-ceeling/grammar.js`) already allows `$.function_call` as its
   left operand, and nothing downstream rejected it.
2. **Two-level "call, then ANOTHER method call" failed**:
   `outer.getInner().button("Save")` (i.e. `expr.method()` where `expr`
   is itself a call, not a bare identifier) hit
   `error[semantic/forbidden-construct]: call to unknown function/method`.
   Confirmed via reading `EscapeHatchChecker::resolveObjectType`
   (`escape_hatch_checker.cpp`) that its own doc comment said exactly why:
   *"Calls, `new`, literals, etc: not resolved by this lightweight pass"* —
   only a bare `Identifier` or a `MemberAccess` chain of FIELDS was ever
   resolved to a class; a `FunctionCall` receiver always returned `{}`,
   which `isKnownCallable` then treats as an unresolvable, thus unknown,
   thus rejected call. A real, narrow, previously-undocumented gap — not an
   architectural wall (the grammar already parses the shape fine).

### The safety question this raises, checked before writing any fix

Widening `resolveObjectType` to resolve a call receiver's return type sounds
like it could open threat #8 (dangling temporary bound to `const&`): in
plain C++, `getInner()`'s receiver `makeHolder()` produces a temporary that
dies at the end of the full expression — if `getInner()` returned a raw
reference into it, `i` would dangle immediately. Checked via a standalone
probe (a hand-written `.cpp` with tracing destructors, not a `.clg` file):
`Holder`'s destructor logs BEFORE `i->v` is read, proving the `Holder`
temporary really does die mid-expression — but `i` (an `Inner`) is still
safely readable afterward, because `getInner()`'s declared return type
(`Inner`, not `ref Inner` — `ref` return types don't exist in CeeLing) makes
CeeLing generate `std::shared_ptr<Inner> Holder::getInner() { return
this->inner; }` — a COPY of the shared_ptr, not a raw alias. The copy's own
refcount keeps `Inner` alive independent of `Holder`'s lifetime. This is the
general reason threat #8 doesn't apply to CeeLing's own classes at all:
every CeeLing object is compiler-wrapped in a smart pointer, so "return an
object" always means "return an owning/shared handle," never a raw
reference into the callee's own storage. Threat #8 is only a live concern
for classes CeeLing does NOT compile itself (Eigen, matched by
`isKnownExternalClassType`) — those keep plain C++ value semantics on
purpose (an expression-template return truly can dangle, per the existing
`auto` vs concrete-type provenance rule, #6, still open). **Deliberately did
NOT widen `resolveDeclaredType`** (the function `isKnownCallable`'s
external-class branch uses, a DIFFERENT function from `resolveObjectType`)
— so a chained call into an external/opaque class's borrowing method
(`getMatrix().block(...)`) remains exactly as rejected as before this
change. Confirmed via probe: `(new Picker()).pick(...)` (a `NewExpression`
receiver, a different unresolved shape from a chained call) is also
untouched by this fix and stays correctly rejected.

### The two-checker requirement: found by asking "what if I only fix one?"

`EscapeHatchChecker::resolveObjectType` is not the only place with this
exact shape. `DanglingChecker::resolveCallReturnAlias` — the function that
detects "this call's return value ALIASES one of its arguments," the
mechanism `summary_return_alias_dangles.clg` exists to test — had the
IDENTICAL restriction: `auto recvId = dynamic_cast<const Identifier*>
(member->object.get()); if (recvId == nullptr) return {};`. Probed this
directly: `(new Picker()).pick(x, y)` (attempting to reach this exact
path with a receiver `EscapeHatchChecker` didn't yet allow) confirmed the
ONLY thing currently preventing the gap from being exploitable was
`EscapeHatchChecker`'s own rejection running first. Had `EscapeHatchChecker`
been widened alone, `factory.makePicker().pick(x, y)` would have passed the
escape-hatch gate while `DanglingChecker` SILENTLY skipped alias tracking
(returning `{}`, not a conservative reject) — `r` would have been wrongly
registered as a fresh, unobserved owner instead of an observer of `x`,
reopening exactly the dangling scenario `summary_return_alias_dangles.clg`
was written to close, just reachable through a chained receiver instead of
a plain one. Both checkers needed the same fix, applied symmetrically.

### The fix: a return-type table in each checker, plus one recursive resolver each

**`EscapeHatchChecker`**: added `classMethodReturnType_` (className ->
methodName -> declared return type) and `freeFunctionReturnType_`
(function name -> declared return type), populated in the SAME base-first
loops that already build `classMethods_`/`knownFreeFunctions_` (one extra
line each: `returnTypes[key] = method->returnType;` /
`freeFunctionReturnType_[fn->name] = fn->returnType;`, inheriting down the
class hierarchy the same way `classMethods_` already does). Added a
`FunctionCall` branch to `resolveObjectType`: for a plain `f()`, look up
`freeFunctionReturnType_`; for `recv.method()`, recursively resolve
`recv`'s own class first (so `a.b().c().d()` — any depth — falls out of
plain recursion, not special-cased per level), then look up
`classMethodReturnType_[recvClass][method]`. Either way, the resolved
return-type string is only accepted if `classNames_.count(...)` — an
external/opaque type, or a type this checker never saw a `ClassDeclaration`
for, still resolves to `{}` and falls through to the existing "unknown ->
reject" behavior, preserving the default-reject posture exactly.

**`DanglingChecker`**: same shape, independently (this checker does not
share tables with `EscapeHatchChecker` — they are two separate passes over
their own state). Added `functionReturnType_`/`classMethodReturnType_`,
populated in `deriveSummaries`'s existing collection loops. Extracted a new
`resolveClassOfExpr` helper (previously this logic was inlined at the top
of `resolveCallReturnAlias` and stopped at a bare `Identifier`) that both
`resolveCallReturnAlias`'s method-call branch and (via the same function)
any future caller can use — handles `Identifier` (via the pre-existing
`localType_`), `FunctionCall` (new, recursing into its own receiver exactly
like `EscapeHatchChecker`'s version), and conservatively returns `{}` for
anything else (a bare `MemberAccess` field chain is NOT resolved here,
since this checker's own `classFields_` only tracks field NAMES, not
declared TYPES — a separate, pre-existing gap this fix does not touch).
`resolveBorrowingMethodReceiver` (the Eigen-style external-class borrow
path) was deliberately left UNCHANGED: it needs to return a stable
IDENTIFIER NAME to watch for a later reassignment, which a chained
receiver's temporary never has, and it is only ever reached through
`resolveDeclaredType`, which (see above) was deliberately not widened —
so this path stays fully gated exactly as before.

### Verification

`tests/safety/chained_method_call_accepted.clg` (`//@ run: 7`): the positive
control — `makeHolder().getInner()` (free-function-call-then-method-call),
transpiles, compiles, and prints the expected value, proving the widened
resolution produces correct generated C++, not just a passing type check.
`tests/safety/chained_method_call_dangling_rejected.clg`: the safety-critical
negative control — `factory.makePicker().pick(x, y)` (method-call-then-
method-call) where `pick`'s summary aliases a parameter; `x` is reassigned
afterward, and the fixture must still be rejected exactly like
`summary_return_alias_dangles.clg`'s plain-receiver original. Both new
fixtures pass; full safety suite 121/121 (`--strict`, up from 119), `ctest`
13/13 — both re-run after the fix, not just the two new fixtures in
isolation, since this touches a resolution path shared by every existing
class-method-call fixture.

## Step 37 — `this` as a real keyword, `this` as a call argument, and three unrelated pre-existing bugs a real end-to-end consumer surfaced

Motivated by `imgui-cling` wanting the real egui/ImGui inline idiom
(`if (ctx.button("Save", x, y, 2.0).clicked) {...}`) instead of its own
two-step `Response r = button(ctx, ...); if (r.clicked) {...}`. Step 36's
chained calls made `.clicked` work on a call's own return value; the
remaining blocker was turning `button()` and friends into `Context`
METHODS. The natural, low-churn shape is a one-line forwarding method
(`Response button(...) { return buttonImpl(this, ...); }`) — which needs
`this` usable as a plain call ARGUMENT, not just in `return this;` (the
only position that worked before this step).

### Part A: promoting `this` to a real keyword

Investigating "can `this` be passed as an argument" surfaced that `this`
was never a grammar keyword at all — it parsed as an ordinary `identifier`,
with every consumer (`ast_emitter.cpp`, `smart_pointer_analyzer.cpp`,
`closure_desugar.cpp`) special-casing it via a **string comparison** on the
name `"this"`. Nothing stopped a user from declaring a variable, parameter,
field, or class literally named `this`, which would collide with every one
of those string checks in confusing ways.

Redirected mid-implementation to fix this properly: added
`this_expression: $ => 'this',` to `primary_expression`'s choice
(`grammar.js`), and a matching `ThisExpression : public Expression` AST
node (trivial marker, no fields). Tree-sitter's lexer already prefers an
explicit string-literal token over the general `identifier` regex whenever
both could match — the same mechanism that already makes `new`/`class`/etc.
unusable as identifiers, with zero exclusion regex needed — so this one
grammar addition makes `this` unusable as a declared name anywhere
`identifier` is expected, for free.

**Every existing `id->name == "this"` string comparison had to migrate to
`dynamic_cast<const ThisExpression*>` in the SAME pass** — a full inventory
via `grep -rn '"this"'` found 8 call sites across
`ast_emitter.cpp`/`smart_pointer_analyzer.cpp`/`closure_desugar.cpp`.
Leaving even one on the old shape would silently stop matching once `this`
parsed as a `ThisExpression` instead of an `Identifier` — confirmed
concretely by a regression: after the grammar change alone (before
migrating the emitter), `shared_return_this_accepted.clg`'s `return this;`
stopped becoming `shared_from_this()` and started emitting a raw pointer
into a `unique_ptr<Node>` return — exactly the class of silent breakage
this project's own safety-suite discipline exists to catch immediately, not
discover later. `closure_desugar.cpp`'s `closure-captures-this` rejection
became **dead code**, not just changed: `capture_list`'s own grammar rule
uses `$.identifier` directly, so `[this]` no longer lexes as an identifier
at all once `this` is reserved — `[this]` is now a PARSE ERROR, caught
before that check would ever run. `closure_captures_this_rejected.clg`
still passes (the test runner only checks exit code, never error text), but
its own doc comment needed updating to describe the new failure mode.

### An unrelated, pre-existing grammar bug this uncovered: bare `return;` was never real

Re-running the full safety suite after the grammar change (before touching
anything else) surfaced a SECOND regression: `tests/linked_list.clg` (an
existing ctest fixture) failed to compile with "return-statement with a
value, in function returning 'void'". Root cause, confirmed via
`tree-sitter parse` on a minimal `void f() { return; }`:
`return_statement`'s grammar rule (`seq('return', $.expression, ';')`) has
**never** supported a genuinely bare `return;` — the rule required an
expression unconditionally. `linked_list.clg`'s own bare `return;`
statements were always relying on tree-sitter's GLR error-recovery silently
inserting a zero-width MISSING node to satisfy the required slot — an
already-fragile, heuristic-dependent behavior that happened to synthesize
an empty-name `identifier` before `this_expression` existed (harmless:
emitted `return ;`, valid C++ for void) and started synthesizing a
`this_expression` node instead the moment that alternative existed
(confirmed via `tree-sitter parse`'s own
`(MISSING this_expression [1, 10] - [1, 10])` output) — which the new
`return this;` lowering then wrongly fired on.

Fixed the root cause rather than the symptom: `return_statement` becomes
`seq('return', optional($.expression), ';')`. `ASTBuilder::
buildReturnStatement` no longer blindly indexes `ts_node_child(node, 1)`
(which, for a bare `return;`, used to be whatever the parser's error
recovery happened to insert there) — it checks `ts_node_named_child_count`
first and returns a `ReturnStatement` with a genuinely null `expression`
when there is none. `ReturnStatement::expression` becoming nullable needed
one real new guard (`AstEmitter`'s `ReturnStatement` emission now checks
`!ret->expression` and emits a bare `return;` before any of the
value-lowering branches, which all assumed a real expression) — every other
consumer (`checkExpression`, `checkExpressionUses`, `visitExpr`,
`collectReturnExpressions`) was already null-safe by construction
(`dynamic_cast` on `nullptr` returns `nullptr`, and most either explicitly
guard at their own top or naturally fall through to an existing
conservative default). Verified via `tree-sitter parse` that a bare
`return;` now produces a real, childless `return_statement` with no MISSING
node at all, and both `ctest` (`cling_test_linked_list_compile`) and the
full safety suite re-passed.

### Part B: `this` as a call argument

With `this` now a real, unambiguous node type, the actual feature was
small. Two gaps, both narrow:

1. **`AstEmitter::emitMovedArg`** (the shared lowering point for a call
   argument, a `VariableDeclaration` RHS, AND an `AssignmentStatement`
   RHS — not call-argument-only, a real correction to the original plan's
   premise) had no `this` handling at all — it fell through to
   `emitExpression`'s Identifier branch, which for `this` degrades to
   emitting the bare token `this`, a raw pointer, into a
   `shared_ptr<T>`/`unique_ptr<T>` parameter slot. Fixed by adding a
   `ThisExpression` branch emitting `shared_from_this()` — exactly the
   already-shipped `return this;` lowering, just reachable from three more
   positions.
2. **`SmartPointerAnalyzer`**'s existing `anyReturnsThis` detection
   (`inferAliasReturningMethods`) only ever scanned RETURN expressions.
   A class passing `this` as an argument but never writing `return this;`
   needs the identical `enable_shared_from_this` + forced-shared-
   construction-root treatment, or `shared_from_this()` either fails to
   compile (missing base) or throws `std::bad_weak_ptr` at runtime (object
   never actually shared_ptr-owned). Added a new, separate walker
   (`methodPassesThisAsArgument`, modeled on `collectReturnExpressions` but
   covering try/catch — which that helper misses — and recursing into
   nested call arguments, not just top-level statement shapes) scanning
   EVERY method in a class, independent of whether that method is already
   classified `aliasReturning`. **A real gap found while writing this
   walker's first draft**: it initially had no `ReturnStatement` case at
   all (reasoning "that's the separate anyReturnsThis scan's job"), which
   is true only for the return's OWN top-level expression — a NESTED call
   argument inside a return (`return r.readVal(this);`) needs the SAME
   argument-position scan and was silently missed until an end-to-end probe
   (`Node.registerSelf` calling `r.readVal(this)` inside a `return`) showed
   `Node` never got `enable_shared_from_this` at all. Fixed by adding a
   `ReturnStatement` branch that scans the return's expression for a
   NESTED `this` argument (deliberately NOT re-checking whether the whole
   return expression IS `this` — that stays the original scan's job).

**Constructor rejection**: `shared_from_this()` throws `std::bad_weak_ptr`
UNCONDITIONALLY when called before the object's `shared_ptr` control block
exists — and inside a constructor, it never does yet. Not a sometimes-risk;
a guaranteed crash. Added `EscapeHatchChecker::rejectThisEscapeInConstructor`,
mirroring the existing virtual-call-from-constructor check's `inConstructor_`
gating, called from exactly the three positions `emitMovedArg` handles (a
call argument, a `VariableDeclaration` RHS, an `AssignmentStatement` RHS) —
each checking that the expression IS DIRECTLY the bare `ThisExpression`, not
merely containing it (so `this.foo()`'s ordinary, always-safe receiver usage
is never caught by this check).

**Why passing `this` cannot dangle**: CeeLing has no non-owning reference to
store it in. `ref` fields are banned by construction (no grammar production
exists) and `ref` parameters are banned too, so a passed `this` can only
land in a plain OWNING field, which lowers to `shared_ptr` — genuine shared
co-ownership, the same already-blessed shape `field_stores_owner_accepted
.clg` documents for ordinary parameter storage. Verified end-to-end with a
destructor-order probe (a hand-written `.cpp`, not a `.clg`): the object
`this` pointed at stayed alive and correctly readable after the ORIGINAL
variable holding it was reassigned to something else entirely.

New fixtures: `this_as_call_argument_accepted.clg` (`//@ run:`, a same-class
bare-call shape avoiding an unrelated forward-declaration ordering issue —
see Part C), `this_argument_forces_shared_root_accepted.clg` (every method
returns `void`/`int`, so there is no possible `return this;` anywhere —
proves the argument-position scan fires independently), `this_stored_in_
field_accepted.clg` (the safety-argument positive control: `this` stored
into another object's field, original variable reassigned afterward, field
still reads the ORIGINAL value), `this_escapes_constructor_rejected.clg`
(the guaranteed-crash case, rejected at compile time). Full safety suite
125/125 (`--strict`), `ctest` 13/13.

### Part C: three more real bugs found ONLY by an actual end-to-end consumer

Compiling CeeLing's own safety fixtures never exercises real multi-file
`.clh` header generation the way an actual multi-file project does. Wiring
one real forwarding method into `imgui-cling`'s `Context` (`Response
button(...) { return button(this, ...); }`, requiring `context.clg` to
`#include "button.clh"` for the very first time — every OTHER consumer in
that project had `#include "context.clh"`, never the reverse) surfaced
three more pre-existing gaps, none related to `this` itself:

1. **Circular `.clh` includes duplicate a class's definition.**
   `context.cpp`'s OWN generation always emits its class's FULL body
   directly (not just `ClassName::method(...)`-qualified definitions) —
   it does not rely on including its own header at all. Adding `#include
   "button.clh"` to `context.clg` meant `context.cpp` now also emitted
   `#include "button.h"` — and `button.h` (needing `Context`'s type in ITS
   own signatures) `#include`s `context.h`, which ALSO has the full class
   body. Two definitions, one translation unit. Fixed generally: `.cpp`-mode
   generation now `#define`s its OWN companion header's include-guard
   symbol (derived from the SOURCE `.clg`'s own base name, not the
   requested output filename, so it matches exactly what `emitHeader` would
   independently compute for the SAME `.clg`) before any `#include` line —
   so a transitively-reached copy of its own header is correctly skipped by
   that header's own pre-existing `#ifndef` guard.
2. **That fix alone was incomplete — headers had no forward-declaration
   mechanism at all.** With `context.h`'s guard now pre-set from
   `context.cpp`, `button.h`'s OWN transitive `#include "context.h"`
   correctly no-ops — but that left `Context` undeclared by the point
   `button.h`'s own signatures needed it (`'Context' was not declared in
   this scope`). CeeLing headers contain ONLY signatures, never bodies that
   dot/arrow-access another type's members, so a forward declaration is
   ALWAYS sufficient for any externally-referenced CLASS (never a
   value-type/struct, which needs its real size even for a declaration).
   Added `AstEmitter::collectForwardDeclarableClasses`: walks every
   field/parameter/return-type across a file's OWN (non-external)
   declarations, and for each referenced class that is (a) not declared by
   this file itself, (b) not namespace-qualified (`class YAML::Node;` isn't
   valid C++; conservatively skipped, no current consumer needs it), and
   (c) not a value-type (`SmartPointerAnalyzer::isValueTypeClass`), emits
   `class X;` right after the file's own `#include` lines. This is a
   STRICTLY ADDITIVE change — existing `#include` lines are untouched, the
   forward declaration is just insurance that the type is known even if the
   "real" include gets skipped by guard timing.
3. **A member method hides a same-named free function in real C++,
   unconditionally — CeeLing's own call resolution didn't match.**
   `Context::button()`'s forwarding body, `return button(this, ...);`
   (5 args), compiled to a HARD ERROR: `no matching function for call to
   'Context::button(...)' — candidate expects 4 arguments`. Confirmed via a
   minimal, CeeLing-independent probe (`struct S { int foo(int); }; ...
   s.foo() calls foo(1,2,3) from inside S::foo` — same error) that this is
   ordinary C++ name lookup: a member name HIDES an outer name of the same
   spelling, full stop, regardless of whether the member's own signature
   is arity-compatible with the call — lookup finds the member first and
   stops, it does not fall back to outer scope on a signature mismatch.
   `EscapeHatchChecker::isKnownCallable` checks free functions BEFORE a
   same-class method of the same name (confirmed: an early `return true`
   for a `knownFreeFunctions_` match, before the class-method branch is
   even reached) — CeeLing's own ANALYSIS already resolves this exact
   ambiguity to "the free function," but the EMITTED bare-call text relied
   on real C++ agreeing, which it does not. Fixed in `AstEmitter`'s
   `FunctionCall` emission: a bare call is now qualified `::name(...)`
   whenever `currentClass_` has a method of the exact same name AND that
   name is also a known free function — deliberately NOT unconditional for
   every bare free-function call (that would wrongly force GLOBAL namespace
   lookup for a genuinely different, already-working case: a namespaced
   free function called bare from within its own namespace).

**Status after all of Step 37**: full safety suite 125/125 (`--strict`),
`ctest` 13/13, and a real `imgui-cling` demo compiled, linked, ran, and was
visually confirmed interactive (`ctx.button("CLICK", ...).clicked` firing
correctly alongside every other widget, unaffected) — the actual motivating
consumer, not just a synthetic probe. Only ONE call site in
`imgui-cling/src/layout_demo.clg` was converted as the proof of concept;
converting the rest of that project's widgets to `Context` methods is a
separate, not-yet-started follow-up.

### Follow-up — a real fluent chain, and a fourth pre-existing bug it found

`ctx.button(...).clicked` (a method call, then a field read) is not yet the
real egui pattern — real egui's `Response` has methods like
`on_hover_text(self, text) -> Self` (`../egui/crates/egui/src/response.rs`)
that return `Self` again, enabling a true multi-link fluent chain
(`.on_hover_text(...).on_hover_cursor(...)`), pointed out directly by
example rather than accepted at face value. Built the equivalent:
`Response.onHoverText(string text)` (`imgui-cling/src/response.clg`) draws a
small tooltip label when hovered and does `return this;`, enabling
`ctx.button(...).onHoverText("Click me!").clicked` — a genuine two-link
chain (the SECOND method call's own return value is what `.clicked` finally
reads), not just one call plus one field.

Verified the underlying mechanism first with a standalone probe before
touching `imgui-cling`: a value-type `struct` with a method returning
`this` (`Resp.onHover(...) -> Resp`), chained two levels
(`r.onHover("test").clicked`) — transpiled, compiled, ran, printed the
correct value. This is `return this;`'s PRE-EXISTING value-type handling
(`*this` dereference, unrelated to Step 37's own shared_from_this() work)
composed with Step 36's chained-method-calls, not a new mechanism.

**Found a real, fourth bug while building this**: the generated `Response`
class picked up `class Response : public std::enable_shared_from_this
<Response>` — a spurious base for a type that is, by this project's own
explicit design, NEVER smart-pointer-wrapped (`response.clg`'s own doc
comment: "Phase 0.5's `struct` keyword is what makes this a bare value...
instead of an owned/reference class instance"). Root cause: `SmartPointer
Analyzer`'s `anyReturnsThis` scan and the newer `methodPassesThisAsArgument`
scan (both from earlier in this same Step) never checked `isValueTypeClass`
before flagging a class into `classesNeedingSharedFromThis_`/
`classesNeedingSharedRoots` — both scans only look at the SHAPE of a
`return this;`/`this`-as-argument, never at whether the enclosing class is
even eligible for shared-pointer treatment at all. Confirmed via
`-fsyntax-only` that this did NOT cause a hard compile error (an unused,
never-instantiated-via-shared_ptr base compiles fine) — but it is
semantically wrong regardless of whether it happens to crash, and this
project's own testing discipline treats "compiles but is not what the
design promises" as a real bug, not a pass. Fixed by adding `!isValueType
Class(cls->name)` to both scans' insertion sites — `classifyValueTypeClasses`
already runs before `inferAliasReturningMethods` in the pipeline, so the
data was already available, no ordering change needed.

New fixture: `value_type_return_this_no_shared_from_this_accepted.clg` (the
exact `Resp`/`onHover` probe above, made permanent, `//@ run:`-asserted).
Full safety suite 126/126 (`--strict`, up from 125), `ctest` 13/13.
`imgui-cling`'s own demo re-verified end-to-end after the fix: transpiled,
compiled, linked, ran, and confirmed BOTH the click counter incrementing
AND the "Click me!" tooltip rendering on hover — the real chain, working.

**Deliberately not fixed this round**: `AstEmitter::emitMovedArg`'s
`ThisExpression` branch still unconditionally emits `shared_from_this()`
whenever `currentClass_` is non-empty, with no `isValueType` check of its
own. This is currently harmless because nothing in this project (or any
fixture) passes a value-type struct's `this` as a call ARGUMENT (only as a
`return this;`, which has its own, separate, already-correct `*this`
lowering) — but it is a latent gap, not a proven-safe case, should that
shape ever be written. Left as a known, narrower follow-up rather than
fixed speculatively without a failing test to fix against.

### Follow-up — converting an ENTIRE consumer, and the guard fix's own bug

`imgui-cling` converting its remaining ~15 widgets (not just `button()`) to
`Context` methods exercised something the single-widget proof-of-concept
never did: `context.clg` now `#include`s TWELVE sibling `.clh` files (one
per widget), each of which `#include`s `context.clh` right back — a much
larger, denser circular-include graph than the original context.clg
<-> button.clh pair.

**Found the Step 37 guard fix (`main.cpp` unconditionally `#define`-ing a
`.cpp`'s own companion-header guard before any `#include`) was WRONG in
general**, not just incomplete — it only happened to work for the narrow
case that first surfaced it. Compiling `drag_float.cpp` (needing `Context`,
hence `#include "context.h"`) exposed the real flaw: `context.h`'s own
preamble needs `#include "drag_float.h"` (for `Context::dragFloat`'s
`DragFloatResult` return type) — but `drag_float.cpp` had ALREADY
pre-defined `CEELING_GEN_DRAG_FLOAT_H` at ITS OWN file-top (the guard fix),
so THAT specific inclusion silently no-op'd, and `DragFloatResult` was
never actually declared by the point `context.h` needed it — the exact
"declared too late" failure the original fix was meant to prevent, just
relocated to a different file pair. Confirmed by reading `drag_float.h`'s
own generated content directly: it DOES carry `DragFloatResult`'s full
struct definition (unlike a plain class, a struct return type needs its
real layout even in a declaration-only header, not a forward declaration)
— so the type WAS available, just suppressed by a guard the wrong file had
already claimed.

**Root cause: a FILE-level guard is the wrong granularity.** Pre-defining
one guard symbol for "this whole file's declaration content" assumes
nothing else in the same translation unit will ever legitimately need that
content to expand for real later — false the moment TWO OR MORE files
mutually depend on each other's types, not just one pair.

**Fixed with a PER-CLASS guard instead**: `AstEmitter::perClassGuardName`
(`"CEELING_DECL_" + uppercased class name`) wraps ONLY the `"class X {
... };"` declaration block — never the out-of-line method bodies, which
must always compile regardless of guard state, since header-mode never
emits them and they exist nowhere else to fall back to. Applied
symmetrically in `emitClass` (`.cpp` mode) and `emitClassHeader` (`.h`
mode), using the IDENTICAL guard-naming scheme, so whichever of "this
class's own local declaration" or "a transitively-included copy of its
header" is reached FIRST in a translation unit wins, and the other
correctly no-ops — with NO assumption about which FILE that happens in,
unlike the file-level version. This also correctly handles a file
declaring MULTIPLE classes (each gets its own independent guard, not one
shared guard that would wrongly suppress the SECOND class in the same
file) and needed no `main.cpp` involvement at all once implemented purely
inside `AstEmitter` — the earlier file-level `#define`-injection code in
`main.cpp` was removed entirely, not just adjusted.

**Also found and fixed a real, narrower naming collision** while adding the
15 new forwarding methods: `Context` already had its own, previously-shipped
`openModal(string id, float x, float y, float width, float height)` method
(a plain state setter, from this project's earlier Modal work) with the
EXACT SAME parameter types a forwarding wrapper for `modal.clg`'s free
function `openModal(ctx, id, screenWidth, screenHeight, width, height)`
would need — a genuine redefinition, not the harmless same-name-different-
scope case every other widget was. Resolved by NOT forwarding `openModal`/
`drawPendingModal` at all — `Context` already has an adequate native API
for modal state, and callers needing the free function's screen-centering
convenience still call it directly, unchanged. Documented directly in
`context.clg` rather than worked around by renaming already-shipped,
already-verified code for a naming nicety.

**Verified end-to-end**: all ~15 remaining `imgui-cling` widgets (checkbox,
slider, dragFloat, comboBox, comboBoxPopup, drawPendingPopup, listBox,
menuButton, drawPendingMenu, radioButton, selectable, tabBar, treeNode,
colorEdit3) converted to `Context` methods; every call site in
`layout_demo.clg` updated to the `ctx.widget(...)` form. Full project
transpiled, compiled (every translation unit, zero errors), linked, ran;
direct interaction confirmed the checkbox pair, radio buttons, and list box
all still fire correctly (console log), and the user separately confirmed
every other widget (slider, color edit, drag float, tree node, tab bar,
combo box, table, delete modal) still works visually. CeeLing's own safety
suite (126/126, `--strict`) and `ctest` (13/13) both re-confirmed green
after the guard-mechanism rewrite.
