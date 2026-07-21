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

The new generator is available today as an opt-in mode; the older text-based generator
is still the default while a few remaining gaps are closed (returning objects by
value, and a couple of smaller cases). Both generators agree on what the safety
checker accepts or rejects — this is purely about how accepted code gets turned into
C++.
