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
assignments, `print`, `return`, `if`, and so on — not "just call a function."

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
print(a.v);     // a is read again, but it's moved-from now → ERROR
```

Two small but important details:

- **Reassignment revives a variable.** After `a = new Node(2);`, `a` is a fresh object
  again and comes off the *moved-from* list — using it is fine.
- **`print` reads but does not move.** It is an output sink, so its argument is used,
  not consumed. Only real function calls move their arguments.

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
   print(b.v);       // ERROR: b's owner is gone
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

## What is deliberately not done yet

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
print(b.v);           // ERROR: b's owner is gone
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
