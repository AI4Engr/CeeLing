# 13. The contract and the threat model

[Chapter 1](01-why-ceeling.md) gave the promise in one sentence. This chapter says exactly what it covers, what it trusts, what it does not claim, and, for every threat on the list
CeeLing was designed against, what the compiler does about it today. Where the answer is "nothing yet", it says so and shows a program that proves it.

## The promise, in exact words

**Static half.** If a program compiles under CeeLing, then in the code CeeLing checked:

- no reference (`ref` observer) can be used after the object it observes is gone, and
- no object is used after it was moved away, and
- none of the escape hatches that would reintroduce a raw pointer exists (raw pointers, raw `new`/`delete`, `.get()`, `reinterpret_cast`, `const_cast`, C-style casts, raw `union`, calls into code the compiler has no facts about).

**Run-time half.** The compiler inserts two checks. Both abort the program (exit code 3 on this build) and print the `.clg` line, so no `catch` can swallow them:

- every `v[i]` and `s[i]` on a `std::vector` or `string` is range-checked (index and size are printed);
- every member access, method call or class-typed `operator[]` through an object handle checks that the handle is not empty. A `weak_ptr` field is read through `lock()` and checked the same way.

Both are on by default and can be switched off per compile (`--no-bounds-checks`, `--no-null-checks`). **A claim about a program is a claim about how it was built.**

**Threads.** `spawn` lets only plain values (copied) and provably unshared flat objects (moved in) cross into a new thread, and `Channel<T>` is the one piece of state a job shares with its
creator: a mutex-guarded queue of plain values. The claim, exactly: *in CeeLing code that was checked, threads share only `Channel` state, so there are no data races through `spawn` or
`Channel`.* Chapter [10](10-threads.md) has the rules.

Put together, the contract is a falsifiable sentence, and a test suite exists to falsify it:

> **If a program compiles under CeeLing, it contains no dangling references and no use of a moved-from object. Out-of-range indexing and null dereference of an object handle stop the
> program on the spot and name the line. Threads share only `Channel` state.**

Here is the dangling half, in a program that is accepted and the same program with one change that is not:

<!-- example: 13_contract_accepted.clg -->
```cpp
class Item {
public:
    int id;
    Item(int i) { id = i; }
};

int main() {
    Item inner = new Item(7);
    ref Item first = inner;            // the observer is used while its owner is alive
    std::cout << first.id << std::endl;
    return 0;
}
```
<!-- output: 13_contract_accepted.clg -->
```
7
```

<!-- example: 13_contract_rejected.clg -->
```cpp
class Item {
public:
    int id;
    Item(int i) { id = i; }
};

int main() {
    ref Item first = nullptr;
    if (1) {
        Item inner = new Item(7);
        first = inner;                 // first observes inner
    }                                  // inner's scope ends: the object is destroyed
    std::cout << first.id << std::endl;
    return 0;
}
```
<!-- error: 13_contract_rejected.clg -->
```
error[semantic/use-after-move]: use of dangling observer `first` (its owner `inner` is gone)
  --> 13_contract_rejected.clg:13:18
  help: `first` observes `inner`, which no longer has a live owner
```

(The rule label says "use-after-move" for both kinds of rejection: dangling observers and moved-from values share one lifetime table, see [chapter 14](14-how-it-works.md).)

## What is trusted

The promise holds for code CeeLing parsed and analysed. Four things sit outside it, and none of them is closed by a later phase, because CeeLing cannot analyse what it never reads.

| Trusted | Why | What a mistake there costs |
|---|---|---|
| **`ext_fn` declarations** | `ext_fn int cheapHash(int seed);` is a signature the compiler believes. The body is hand-written C++ linked in separately. It is the source-level counterpart of Rust's `unsafe`, without that name. | A wrong signature, or a body that secretly keeps a pointer to an argument, is believed and emitted. |
| **Shims** (the hand-written C++ behind `ext_fn`) | They are ordinary C++: raw pointers, no bounds checks. | Any C++ bug. The checks CeeLing inserts do not reach inside, and a handle passed to a shim is not inspected. |
| **Libraries** | What `std`, Eigen or another library does with what it is given is not checked. Their borrow facts come from a table on CeeLing's side, and a call with no entry is rejected rather than trusted. | A wrong table line silently voids the guarantee for that call. |
| **The C++ compiler and standard library** | The generated C++ is compiled by `g++` or another compiler. | A compiler bug is not detected. |

<!-- example: 13_trusted_ext_fn.clg -->
```cpp
ext_fn int cheapHash(int seed);        // the body is C++ the compiler never sees

int main() {
    std::cout << cheapHash(5) << std::endl;
    return 0;
}
```

The declaration is restricted on purpose (primitive and primitive-vector parameters only, an optional `borrows N` for a returned borrow). [Chapter 11](11-talking-to-cpp.md) covers it. The point here
is only that this line is the one place where the checker says "I believe you".

## What is never claimed

- **Not Rust's guarantee.** No lifetime annotations means fewer facts, a different (smaller) set of things proved, and no `Send`/`Sync` machinery. [Chapter 17](17-comparisons.md).
- **Not thread-safety.** Only what `spawn` and `Channel` carry is checked. Deadlock, waiting forever, shared state behind an `ext_fn`, the file system and the number of threads are not.
- **Not the safety of the caller's C++, of shims, or of the libraries called.**
- **Not "no memory errors at all".** The open gaps below are real and listed.
- **Not that rejected code is wrong.** The analysis is conservative; rejected code is outside the language, not necessarily buggy.
- **Not logic correctness.** 17 of cJSON's 33 published issues are logic errors; CeeLing reproduces them faithfully ([chapter 15](15-case-study-cjson.md)).

## Strategy decisions

Five decisions shape everything else.

1. **Static where it can be proved, a run-time check where it cannot.** Dangling, use-after-move and the escape hatches are caught at compile time. Bounds and null cannot be proved without
   rejecting most real programs, so the compiler inserts a check that aborts, the same pragmatic path other safe dialects take. Pure static proof is not required, and would over-reject
   to the point of being unusable.
2. **Default-reject, never default-allow.** Anything the analyser cannot prove safe, including a call into a library with no entry in the facts table, is a compile error. There is no "unknown
   call" through which a dangling reference can leak past the checker.
3. **Ambiguous inference becomes an error, not a guess.** If two branches leave an observer pointing at different owners, the observer is treated as dangling. If ownership cannot be resolved to one answer, the compiler asks you to rewrite.
4. **Seal, do not analyse, where sealing is possible.** `ref` fields, `[&]` capture, `.get()`, raw pointers and `delete` are not detected; they cannot be written. A construct that does not exist cannot be misused.
5. **Threads: creation and what crosses are checked; sharing is not generalised.** Only values and uniquely owned flat objects cross into a job, and `Channel` is the one sanctioned shared
   state. Complete data-race checking is not attempted: it needs exclusivity tracking and a read/write distinction, which are exactly the lifetime and mutability machinery that dropping `borrow<T>` gave up.

## The threat table

The 19 threats CeeLing was designed against, each with what happens today. "Static" means rejected at compile time, "by construction" means the grammar or the checker leaves no way to write it,
"run time" means a compiler-inserted check, "open" means a program that has the problem is accepted.

| # | Threat | Today | Where |
|---|---|---|---|
| 1 | An observer outlives its object | **Static.** Provenance table, death events (move, reassignment, scope exit), use-site check, branch merges. A `ref` may only observe a local, an element `v[i]` of a local container, another `ref` or `nullptr`; one bound to a call result or a field is rejected. | [6](06-dangling-references.md) |
| 2 | Return a reference to a local | **By construction.** There is no reference return type, so a return is always owning; returning a `ref` observer is rejected. | [6](06-dangling-references.md) |
| 3 | A reference stored in a longer-lived field | **By construction.** The grammar has no `ref` field, and `ref` parameters are rejected, so an observer has no place to be stored. | [6](06-dangling-references.md), [8](08-sealed-escape-hatches.md) |
| 4 | Iterator invalidation | **Partly.** A `ref` into a vector element dies at `push_back`, `resize` and `pop_back`. An iterator held in a variable (`auto it = v.begin();`) and mutation inside a range-for body are **open**, see below. | [6](06-dangling-references.md) |
| 5 | `string_view` / `span` of a temporary | **Open.** A type name the compiler does not know is passed to C++ unchanged, so views are accepted and invisible to every check. | [6](06-dangling-references.md), below |
| 6 | Expression-template dangle (Eigen `auto e = A + B`) | **Open, narrow exception.** The old claim that CeeLing has no `auto`, so the danger cannot be written, is stale: `auto` is accepted and not tracked. A short hand-written list of Eigen view methods (`block`, `row`, `col`, `transpose`, `segment`, `head`, `tail`, `diagonal`) is tracked; its fixtures live outside `tests/safety`, in a separate Eigen spike. | below |
| 7 | A lambda captures `[&]` and escapes | **By construction.** Closures capture by value only; there is no `&` to write. | [9](09-closures.md) |
| 8 | A temporary bound to a `const&` | **Listed open; the C++ shape cannot be written.** There is no `const T& x = f();` local (it is a syntax error), and a `ref` bound to a call result is rejected. Views returned from library calls fall under row 5. | [6](06-dangling-references.md) |
| 9 | Null dereference | **Run time.** Every object-handle dereference is checked and aborts with the line. | [7](07-run-time-checks.md) |
| 10 | Use after move | **Static.** Moves are inserted by the compiler, so tracking them is not optional. | [5](05-moves.md) |
| 11 | Out-of-bounds `v[i]` | **Run time.** `cling::at` aborts with line, index and size. | [7](07-run-time-checks.md) |
| 12 | `shared_ptr` cycle, a leak | **Not enforced.** The compiler picks `weak_ptr` for a back link from a naming rule or a hint comment, not from cycle detection. A leak is not a dangling reference. | [4](04-ownership.md), below |
| 13 | `reinterpret_cast`, C-style casts | **Static, sealed.** The names are rejected as unknown calls; the only conversions are the primitive `int(x)`, `double(x)` and so on, which refuse objects. | [8](08-sealed-escape-hatches.md) |
| 14 | `.get()` | **Static, sealed.** Rejected on a CeeLing object. | [8](08-sealed-escape-hatches.md) |
| 15 | Raw `new` / `delete` / `T*` | **Static, sealed.** `delete` has no syntax; `T*` is rejected; `new` is CeeLing's own constructor syntax. | [8](08-sealed-escape-hatches.md) |
| 16 | `const_cast` | **Static, sealed.** Rejected as an unknown call. | [8](08-sealed-escape-hatches.md) |
| 17 | `union` / `variant` confusion | **Union: static, sealed.** Raw `union` is a parse error with an explanation. **`variant`: not guarded.** A `std::variant` is accepted like any library type (a probe with `std::get<int>` compiled); the guard was never started. `std::get` itself checks and throws. | [8](08-sealed-escape-hatches.md), below |
| 18 | Read of an uninitialised variable | **Partly.** A local must be initialised where it is declared (`int n;` does not parse). A primitive class **field** with no initialiser is not set, see below. | [8](08-sealed-escape-hatches.md), below |
| 19 | Data races | **Partly, narrowly.** What `spawn` and `Channel` carry is checked; deadlock, waiting forever, `ext_fn` shared state, the file system and thread count are not. Never "thread-safe". | [10](10-threads.md) |

Eight of the rows (2, 3, 7, 13 to 17, the last for `union` only) are closed by removing the syntax. Rows 1 and 10 are the analysis proper. Rows 9 and 11 are the run-time half. Rows 4, 5, 6, 12, 17 (`variant`), 18 and 19 are where the
table is not yet fully green, and that is stated here rather than hidden in a footnote.

One more program for each kind of row. Use after move (row 10), a sealed hatch (row 15) and a run-time check (row 11):

<!-- example: 13_use_after_move_rejected.clg -->
```cpp
class Order {
public:
    int id;
    Order(int i) { id = i; }
};

void ship(Order o) { }

int main() {
    Order order = new Order(5);
    ship(order);                       // row 10: passing the object hands it over
    std::cout << order.id << std::endl;
    return 0;
}
```
```

<!-- example: 13_raw_delete_rejected.clg -->
```cpp
class Order {
public:
    int id;
    Order(int i) { id = i; }
};

int main() {
    Order order = new Order(5);
    delete order;                      // row 15: there is no manual delete
    return 0;
}
```
```

<!-- example: 13_bounds_abort.clg -->
```cpp
int main() {
    std::vector<int> v;
    v.push_back(1); v.push_back(2); v.push_back(3);
    int i = 3;
    std::cout << v[i] << std::endl;    // row 11: one past the end
    return 0;
}
```
```

Row 18 is the one a reader can try at once. The grammar has no production for a local declared without a value:

<!-- example: 13_uninitialized_rejected.clg -->
```cpp
int main() {
    int total;                         // a local must be given a value where it is declared
    std::cout << total << std::endl;
    return 0;
}
```
<!-- error: 13_uninitialized_rejected.clg -->
```
Error: Syntax error at line 2, column 5, node type 'ERROR', near: int total;                         // a local must be given a value where it is declared
    std::cout << total << std::.... Suggestion: check member-access syntax and make sure the full statement is supported.
```

The message is a generic parse error: the grammar has no production for a declaration without `= expression`. That is the "by construction" rule at work, and also its price: the error does not say why.

## The open gaps, in plain words

Three of the rows below (4, 5, 6) have one cause: **a type name the compiler does not know is passed to C++ unchanged.** `auto`, `std::string_view`, `std::span` and any made-up name are emitted as written,
and every check in the compiler follows the types it knows (its own classes, `string`, vectors and maps, a short list of external classes). A value of any other type is invisible to the dangling analysis.

These are the places where a program with the problem is accepted today. Each one is something I ran against the compiler; none is a proof that the program is wrong in every build, only that CeeLing
does not stop it.

**Iterators and range-for (row 4).** A `ref` into a vector is tracked. An iterator is not.

<!-- example: 13_gap_iterator.clg -->
```cpp
int main() {
    std::vector<int> v;
    v.push_back(1);
    auto it = v.begin();               // an iterator into v
    v.push_back(2);                    // may move the storage: it now dangles
    std::cout << *it << std::endl;     // accepted today: iterators are not tracked
    return 0;
}
```

A `push_back` inside the body of a range-for over the same vector is accepted too. The run-time bounds check does not help: `*it` is not a subscript.

**Views (row 5).** `std::string_view` is accepted and nothing records that it points into a string.

<!-- example: 13_gap_string_view.clg -->
```cpp
std::string make() { return "abc"; }

int main() {
    std::string_view v = make();       // a view of a temporary string
    std::cout << v << std::endl;       // accepted today: views are not tracked
    return 0;
}
```

**Expression templates (row 6).** `auto e = a * b;` over Eigen matrices is accepted. In this form the danger needs a matrix that dies before `e` is used; nothing watches for it. The Eigen view
methods listed in the table are the narrow exception (from `TODO.md`; I did not re-run those fixtures, and I found none for them in `tests/safety`).

**Reference cycles (row 12).** A class whose field points back at its owner leaks if the compiler infers `shared_ptr` for it. The inference gives `weak_ptr` to fields named `prev`, `previous`,
`parent`, `back` or `owner` in classes with several members of their own type, or when a `//@ptr weak Class.field` hint says so. It does not look for cycles:

<!-- example: 13_gap_cycle.clg -->
```cpp
class Emitter {
public:
    std::vector<Listener> listeners;
    Emitter() { }
    void subscribe(Listener l) {
        l.boss = this;                 // the listener points back at its emitter
        listeners.push_back(l);
    }
};

class Listener {
public:
    Emitter boss;                      // not named parent/owner/prev, so not seen as a back link
    Listener() { }
};

int main() {
    Emitter e = new Emitter();
    e.subscribe(new Listener());
    return 0;
}
```
<!-- generated: 13_gap_cycle.clg "class Listener {" -->
```cpp
class Listener {
public:
    std::shared_ptr<Emitter> boss;
    Listener();
};
```

`boss` became `shared_ptr` and the emitter holds the listener, so both stay alive forever. This is a leak, not a corruption: it is outside the contract, and it is the reason `owner` and `parent`
are good names to use. A probe with a counting destructor, recorded in `TODO.md`, found all three listeners of a similar program still alive after the last outside handle was released.

**`variant` (row 17).** `std::variant` is accepted as any other library type. Reading the wrong alternative is the type confusion the row is about; `std::get` throws `std::bad_variant_access`, which
`catch (string e)` does not catch, so the program terminates rather than misreads. The row's "guard variant" part was never built.

**Uninitialised fields (row 18).** The rule is for locals. A class field of primitive type that the constructor does not set is not initialised by the generated C++:

<!-- example: 13_gap_uninit_field.clg -->
```cpp
class Counter {
public:
    int n;                             // never given a value
    Counter() { }
};

int main() {
    Counter c = new Counter();
    std::cout << c.n << std::endl;     // accepted today: prints whatever the memory held
    return 0;
}
```

The compiler accepts it, and the generated constructor body is empty. Object-typed fields are safe (an empty handle, which the null check catches). `CLAUDE.md` lists row 18 as "static, require
definite-init" without this qualification; `TODO.md` says the same for fields ("safe: the emitter always default-constructs the smart pointer"), which is true for handles and not for
primitives. I report it as a gap.

**Smaller gaps, all documented where they live.**

- *Run-time checks* ([chapter 7](07-run-time-checks.md)): `pop_back()` on an empty vector, iterators past `end()`, `substr` with a bad position (libstdc++ throws `std::out_of_range`, which `catch (string e)`
  does not catch, so the program terminates), anything an `ext_fn` indexes, and `this`. The bounds message carries the line but not the file name.
- *`ref` observers are still raw pointers inside the generated C++.* The static analysis keeps them safe; an observer that dangles in a way the analysis misses is not caught by a run-time check.
  Making them self-checking is an open item.
- *An `ext_fn ref std::vector<T>` argument that is a local is emitted as `std::move(local)`*, although the parameter is `const&`. The checker does not mark the local moved-from, so a later read sees whatever
  the move left. Every call site in zlib-cling passes a field, which is not affected. This is a known, unfixed bug, not a design choice.
- *Across files* ([chapter 12](12-many-files.md)): the dangling and use-after-move checks are the single-file analysis over the merged source; a C++ header, an `ext_fn` or a library is a trusted boundary.
- *Resources, not memory:* stack depth and CPU time are not bounded (cJSON issues 8, 9 and 10 in [chapter 15](15-case-study-cjson.md)); an index and generation pair that goes stale is valid memory with a wrong meaning,
  and nothing flags it.
- *Threads* ([chapter 10](10-threads.md)): `wait(-1)` can wait forever, every pool worker can block waiting for a producer queued behind it, nothing says who closes a channel, and a worker can call an `ext_fn` that touches shared state.

## Accepted by CeeLing is not the same as it builds

CeeLing does not type-check the way C++ does; it checks the safety facts and hands the rest to `g++`. Several mistakes therefore pass the CeeLing checks and are refused by the C++ compiler afterwards. They are
not safety holes, because no program is produced, but "CeeLing accepted it" does not mean "it builds". Examples found while writing this guide, each described where it belongs:

- `auto a = new Node(3);` and `Color d = 7;` (an int into an enum), `new Label(1, 2, 3)` with no such constructor, a typo in `override`, a comparison of two handles whose inferred pointer kinds differ ([chapter 2](02-language.md));
- a closure assigned to a field of an existing object outside a constructor, a name repeated in a capture list, a capture list that leaves out a name the body uses ([chapter 9](09-closures.md));
- `ref` applied to a primitive or a vector, a `ref` passed as an argument, or a `ref` copied into an owner ([chapter 6](06-dangling-references.md)).

The reverse also holds, and is the safer direction: a hole in CeeLing's checks is only exploitable if the C++ it produces compiles. Do not rely on that; the gaps above all produce C++ that compiles.

One more place where CeeLing is not C++: a shift next to a comparison. Here `4 >> 1 == 2` is `4 >> (1 == 2)` (comparison binds tighter), the opposite of C++ ([chapter 2](02-language.md)). The generated C++
is parenthesised to match, so the program means what CeeLing says, not what a C++ reader expects.

## How the contract is tested

The promise is checked by programs, not by reading the checker.

- `tests/safety/` holds 306 fixtures, each tagged `//@ expect: accept` or `//@ expect: reject`: 186 that must compile and 120 that must be refused. For each rejected scenario there is an accepted
  neighbour (a "positive control") so that a check cannot pass by rejecting everything.
- **The compile gate.** An accepted fixture only counts if the C++ it generates is legal C++: `run_safety_tests.sh` compiles every one with `g++`. 154 fixtures also carry `//@ run:` and are *executed*, their output
  compared; 7 carry `//@ run-fail:` and must die at run time with the stated text on stderr (the bounds, null and weak-field checks), compiled at `-O2` so that the library's own assertions cannot be the thing that stopped them.
  An accepted program that runs wrongly is a failure the gate can see; one that only compiles is not.
- **A compiler crash is not a rejection.** An exit code of 128 or more fails the fixture; it used to count as a reject and could satisfy an `expect: reject` fixture.
- **XFAIL** is the runner's word for a missed reject: a fixture that expects rejection but is accepted. It was the state of every reject fixture before the analysis existed. It is reported but
  does not fail an ordinary run; with `--strict` it is a failure. The CTest target (`cling_safety_contract_baseline`) does not pass `--strict`, so a regression that starts accepting a program it used to reject would show as
  XFAIL, not as a red test. I did not run the suite for this chapter, so I cannot report the present XFAIL count; the runner prints it on its `Summary:` line.
  An over-rejection (an accept fixture refused) is always a hard failure.
- **Across files.** `tests/separate/run_cross_file_test.sh` splits each of the 239 splittable fixtures into a library file and a `main` file and requires the same verdict: 94 rejects stay rejects, 141 accepts stay
  accepts, and four are rejected only when split, each with a stated reason. 65 fixtures are all `main` and cannot be split.
- **The guide itself.** Every example in this book is compiled by the same compiler each time the guide is checked, and the messages quoted are the ones printed.
- **Real libraries** exercise it from the outside: [chapter 15](15-case-study-cjson.md), and the other projects in [chapter 18](18-status.md).

What this is: a regression suite and evidence. What it is not: a proof. A program the fixtures do not resemble can still find a hole, and two were found in two days by probing the checker by hand (an unknown call inside
`try`; a `ref` bound to a field, a getter or a temporary), both fixed. "Narrow the language so that the analysis can be complete" is a bet, and the case study is how it is being tested.

## Reading a claim

When someone says "this CeeLing program is safe", the honest question is: *safe against what, built how, trusting what?* The answer has this shape:

> No dangling references and no use of moved-from objects in the `.clg` code; bounds and null checked at run time **if** built with the checks on; threads sharing only `Channel` state; trusting these
> `ext_fn`s and shims, these libraries, and this C++ compiler; with the gaps in this chapter open.

Chapter [14](14-how-it-works.md) shows how the checks are built, chapter [18](18-status.md) what is done and what is not.
