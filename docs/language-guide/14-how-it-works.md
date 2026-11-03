# 14. How it works

The earlier chapters say what the compiler refuses. This one says how it decides, so that you can predict a verdict, understand a refusal that looks unfair, and judge how far to trust it.
Every stage described here is shown by a small program that stops (or does not stop) at that stage.

## The pipeline

CeeLing is a source-to-source compiler: one program in, one C++ file out. What happens in between is a fixed sequence, in this order (it is the order of `src/main.cpp`):

| # | Stage | What it does | A program it stops |
|---|---|---|---|
| 1 | Parse | Builds the syntax tree. The grammar has no rule for what the language forbids | [`delete`](#1-parse) |
| 2 | Include | Reads each `#include "x.clh"`, parses `x.clg` and adds its declarations | [two files](#across-files) |
| 3 | Rewrite | Writes struct decoders, checks `Channel`, turns closures into classes | [decoder](#3-rewrite-before-any-check), [channel](#3-rewrite-before-any-check), [closure](#why-closures-and-ref-fields-are-closed-by-construction) |
| 4 | Escape hatches | Refuses the sealed constructs and every call it has no fact about | [`.get()`](#4-escape-hatches-and-default-reject), [unknown call](#why-a-library-needs-facts) |
| 5 | Pointer analysis | Decides which objects are `unique_ptr`, `shared_ptr`, `weak_ptr` | [the choice](#one-analysis-three-outputs) |
| 6 | Dangling and move check | The subject of [chapter 6](06-dangling-references.md) and [chapter 5](05-moves.md) | [a trace](#a-worked-example) |
| 7 | Advisory checks | Begin/End pairing warnings; never an error | (none) |
| 8 | Emission | Writes the C++ from the checked tree | [the output](#how-the-c-is-written) |

Two things follow from the order. A construct that is refused at stage 1 or 4 never reaches the lifetime analysis, so that analysis may assume the sealed constructs are absent. And stage 5
runs before stage 6, because the dangling check needs to know, for example, whether a `push_back` hands its argument over (an owner that is unique) or shares it (one that already has
more than one owner).

### 1. Parse

The grammar is smaller than C++'s. The constructs the language forbids are not caught by a check afterward; they have no production, so the parser has nothing to build:

<!-- example: 14_parse_error.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

int main() {
    Node a = new Node(1);
    delete a;                       // no grammar rule exists for delete: the parser stops here
    return 0;
}
```
<!-- error: 14_parse_error.clg -->
```
Error: Syntax error at line 9, column 5, node type 'ERROR', near: delete a; Suggestion: raw `delete` is forbidden (hatch #15) -- object lifetime is managed automatically by the compiler-chosen smart pointer; there is no manual delete.
```

This is the strongest form of refusal, and the cheapest. `delete`, `union`, a `ref` field, a `ref` return and a `&` in a capture list are all absent this way.

### 3. Rewrite before any check

Three passes change the tree before any safety check looks at it, so that the checks see ordinary code.

* **Struct decoders.** A call `decodePoint(item)` for a `struct Point` the file declares makes the compiler write the decoder as CeeLing source and parse it. If the library it needs is not visible,
  the error appears here:

<!-- example: 14_decoder_stage.clg -->
```cpp
struct Point {
public:
    int x;
    int y;
    Point(int a, int b) { x = a; y = b; }
};

int main() {
    int item = 3;
    Point p = decodePoint(item);    // the compiler writes decodePoint before any check runs, and needs cJSON's names for it
    return 0;
}
```
<!-- error: 14_decoder_stage.clg -->
```
Error: error[semantic/struct-json-needs-cjson]: generated struct decoders need cjson-cling, which is not visible here (missing: isObject, isString, isNumber, isBool, isTrue, isArray, getObjectItem, getArraySize, getArrayItem, class Item). add #include "cjson_api.clh" (and cjson_item.clh)
```

* **`Channel<T>`.** The element type is checked here, before it can be hidden in another type:

<!-- example: 14_channel_check.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

int main() {
    Channel<Node> c;                // refused by the channel check, which runs before the closure desugaring
    return 0;
}
```
<!-- error: 14_channel_check.clg -->
```
Error: error[semantic/forbidden-construct]: `Channel<Node>` would carry `Node`, which is not a plain value (line 8, column 5). a Channel carries what a `spawn` may capture: primitives, string, std::vector of those, and `struct`s made only of those. They are copied through the queue, so no object is shared between threads. Send a `struct`, or serialize the object to a string first
```

* **Closures** become classes. They get their own section below.

### 4. Escape hatches and default-reject

Next the sealed constructs that do have syntax (`.get()` on an object, a raw pointer type, a cast) and every call are examined.

<!-- example: 14_hatch_check.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

int main() {
    Node p = new Node(1);
    int r = p.get();                // refused by the escape-hatch check, before any lifetime analysis runs
    return 0;
}
```
<!-- error: 14_hatch_check.clg -->
```
error[semantic/forbidden-construct]: '.get()' is forbidden on a CeeLing object ('p' is a Node)
  --> 14_hatch_check.clg:9:13
  help: '.get()' would extract a raw pointer, which has no legal use in CeeLing (CLAUDE.md hatch #14) -- use the object directly, or add a named accessor method to the class if you need a specific field.
```

The rule for calls is the important one, and it has its own section below: a call to anything the compiler has no fact about is an error.

## The three parts of the lifetime analysis

The dangling check is a single pass over each function body, with two tables and one set of rules. It is deliberately smaller than a borrow checker.

**A. Borrow summaries.** For each function and method, one fact: *what, if anything, does the result alias?* CeeLing has no `ref` parameters, so an object argument is always handed over, and the only
question left per function is which parameter (if any) the returned object is. The compiler derives it by reading the body once, before any body is checked, so calls may come before definitions.

<!-- example: 14_summary_no_annotation.clg -->
```cpp
// No annotation says that "second" returns its second parameter. The compiler reads the body.
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

Node second(Node a, Node b) {
    return b;
}

int main() {
    Node x = new Node(1);
    Node y = new Node(2);
    Node r = second(x, y);
    std::cout << r.v << std::endl;
    return 0;
}
```
<!-- output: 14_summary_no_annotation.clg -->
```
2
```

Nothing told the compiler that `second` returns its second parameter. For a function that returns a fresh object, the same derivation records "aliases nothing", and the variable that
receives the result is an owner of its own, not an observer. The next section's trace depends on exactly this difference.

**B. The provenance table.** Which owner does each observer depend on? An entry is made by `ref b = a;` (b depends on a), by `ref c = b;` (c depends on whatever b depends on), by `Node r = f(x, y);` when
f's summary says its result is a parameter (r depends on the owner passed there), and by a call to a library method that is known to return a view of its receiver. Reassignment of the observer replaces
the entry; two branches that disagree make it ambiguous.

**C. Death events and the use-site check.** An owner dies when it is moved into an owning call, given a new object, left by scope exit, or when a container call on the table of
[chapter 6](06-dangling-references.md) runs. Death marks every observer that depends on it. Each use of an observer or of a moved-from name then asks one question: is what it depends on still
alive? If not, it is an error.

In the checker the tables are plain sets and maps keyed by variable name: which names are moved-from, which are observers, what each observes, which are already dead. A branch or loop takes a
copy before it starts and merges at the end.

### A worked example

<!-- example: 14_trace.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

Node pick(Node a, Node b) {
    return a;
}

int main() {
    Node x = new Node(1);
    Node y = new Node(2);
    Node r = pick(x, y);
    ref Node o = r;
    x = new Node(3);
    std::cout << o.v << std::endl;
    return 0;
}
```
<!-- error: 14_trace.clg -->
```
error[semantic/use-after-move]: use of dangling observer `o` (its owner `x` is gone)
  --> 14_trace.clg:17:18
  help: `o` observes `x`, which no longer has a live owner
```

The checker's view, line by line (`main` only; `pick` was summarised first):

| After the statement | Moved-from | Observes | Dead observers | Event |
|---|---|---|---|---|
| `Node x = new Node(1);` `Node y = new Node(2);` | | | | `x` and `y` are live owners |
| `Node r = pick(x, y);` | `x`, `y` | `r` -> `x` | | arguments are handed over; the summary says the result is parameter 0, so `r` is not a new owner but depends on `x` |
| `ref Node o = r;` | `x`, `y` | `r` -> `x`, `o` -> `x` | | an observer of an observer inherits the owner |
| `x = new Node(3);` | `y` | `r` -> `x`, `o` -> `x` | `r`, `o` | assignment is a death event for what `x` held; `x` is a live owner again, of a new object |
| `std::cout << o.v` | | | | use of `o`: its owner is dead, so the program is refused |

The message names `x`, not `r`, because `o` was bound through `r`, and the table kept the original owner. The location is the use, not the death, because the use is the point at
which a program does harm.

## One analysis, three outputs

Dangling detection, the automatic `std::move` and the choice of smart pointer are not three features. They read the same ownership information, which is why they cannot contradict each other.

* **Dangling**: refuse a use whose owner is dead (above).
* **The move**: where an owner is handed over at its last use, the emitter writes `std::move`. The condition for writing it is the same as the condition for the death event.
* **The pointer kind**: an object with one owner is a `unique_ptr`; one with several (held by a variable and by a field, say) is a `shared_ptr`; a link that closes a cycle is a `weak_ptr`.

<!-- example: 14_pointer_kinds.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

class Pair {
public:
    Node left;
    Pair(Node a) { left = a; }
};

int main() {
    Node lone = new Node(1);
    Node both = new Node(2);
    Pair p = new Pair(both);
    std::cout << lone.v << " " << both.v << std::endl;
    return 0;
}
```
<!-- output: 14_pointer_kinds.clg -->
```
1 2
```
<!-- generated: 14_pointer_kinds.clg "int main()" -->
```cpp
int main() {
    std::unique_ptr<Node> lone = std::make_unique<Node>(1);
    std::shared_ptr<Node> both = std::make_shared<Node>(2);
    std::unique_ptr<Pair> p = std::make_unique<Pair>(both);
    std::cout << cling::nn(lone, "line 17")->v << " " << cling::nn(both, "line 17")->v << std::endl;
    return 0;
}
```

`both` is stored in a field of `p` as well as held by its own name, so it has two owners and is a `shared_ptr`; `lone` is a `unique_ptr`. Here the same decision decides what is a move:

<!-- example: 14_three_outputs.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

class Pair {
public:
    Node left;
    Pair(Node a) { left = a; }
};

void sink(Node n) { }

int main() {
    Node lone = new Node(1);
    Node both = new Node(2);
    Pair p = new Pair(both);        // both is now held twice: by this variable and by p.left
    sink(lone);                     // lone's last use
    std::cout << both.v << std::endl;
    return 0;
}
```
<!-- output: 14_three_outputs.clg -->
```
2
```
<!-- generated: 14_three_outputs.clg "int main()" -->
```cpp
int main() {
    std::unique_ptr<Node> lone = std::make_unique<Node>(1);
    std::shared_ptr<Node> both = std::make_shared<Node>(2);
    std::unique_ptr<Pair> p = std::make_unique<Pair>(both);
    sink(std::move(lone));
    std::cout << cling::nn(both, "line 20")->v << std::endl;
    return 0;
}
```

`sink(lone)` is the last use of a unique owner, so the emitter writes `std::move`. Because that is a death event, the same fact is what refuses the following program, which would otherwise read a moved-from
pointer:

<!-- example: 14_three_outputs_rejected.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

void sink(Node n) { }

int main() {
    Node lone = new Node(1);
    sink(lone);                     // the same death event that justified std::move
    std::cout << lone.v << std::endl;
    return 0;
}
```
<!-- error: 14_three_outputs_rejected.clg -->
```
error[semantic/use-after-move]: use of moved-from value `lone`
  --> 14_three_outputs_rejected.clg:12:18
  help: `lone` was moved earlier; assign a new value before using it
```

A `std::move` is never written for a value that is still observed afterward, because that would be a death event, and the use of the observer would be refused. The pointer kinds are the subject of
[chapter 4](04-ownership.md).

## Why a library needs facts

The analysis needs, for every call, three things: which arguments are handed over, whether the result aliases one of them, and whether the call changes anything that observers depend on. For a function
CeeLing compiled, it derives them. For a function it did not compile, such as a C++ standard function, it has no way to know, and a wrong guess is the one thing the guarantee cannot absorb.
So a call it has no fact about is an error:

<!-- example: 14_unknown_call.clg -->
```cpp
int main() {
    int a = 1;
    int b = 2;
    std::swap(a, b);   // no fact about what swap does with its arguments
    return 0;
}
```
<!-- error: 14_unknown_call.clg -->
```
error[semantic/forbidden-construct]: call to unknown function/method `std::swap`
  --> 14_unknown_call.clg:4:5
  help: CeeLing has no borrow/ownership summary for anything it did not compile itself; unknown calls are rejected by default rather than silently trusted (CLAUDE.md "default-reject, never default-allow"). Define `std::swap` in CeeLing source, or call an existing CeeLing function/method instead.
```

This is **default-reject**. The alternative, default-allow, would make every unlisted library call a hole in the guarantee, and the list of holes would grow with the library. With default-reject
the guarantee does not depend on how complete the list of facts is, only usefulness does.

Facts come from two places. The compiler ships fact packs for the standard pieces it supports: scanned signatures of `std::vector` and `std::string` (a rule decides what is admitted, [chapter 3](03-values-and-containers.md)), hand-written entries for a few functions and eight Eigen view methods ([chapter 6](06-dangling-references.md)).
For a function of your own C++ you write the one fact yourself, in an `ext_fn` declaration ([chapter 11](11-talking-to-cpp.md)). The `borrows N` clause is exactly the summary of part A, written by hand because there is no body to read.

<!-- example: 14_ext_fn_borrows.clg -->
```cpp
// A library function has no body to read a summary from. The author of the declaration writes the one fact the checker needs.
ext_fn std::vector<int> sliceOf(ref std::vector<int> data, int start) borrows 0;

int main() {
    std::vector<int> a;
    a.push_back(1);
    std::vector<int> b = sliceOf(a, 0);     // borrows 0: b refers to what a held
    a.push_back(2);
    int x = b[0];
    return 0;
}
```
<!-- error: 14_ext_fn_borrows.clg -->
```
error[semantic/use-after-move]: use of dangling observer `b` (its owner `a` is gone)
  --> 14_ext_fn_borrows.clg:9:13
  help: `b` observes `a`, which no longer has a live owner
```

That fact is **trusted**. The compiler cannot check it against a body it never sees, so a wrong declaration is a hole of the same kind as Rust's `unsafe`. This is why the guarantee is worded "excluding what
trusted libraries do with what they are given".

## Why it is conservative, and what that costs

The checker does not evaluate conditions, does not track a variable's value, and does not follow a path it cannot name. At a branch it takes the worse of the outcomes. That keeps it small and its answers
predictable, and it is why a precise merge of control flow, the hardest part of a real borrow checker, is not needed. The price is that it refuses programs that are fine:

<!-- example: 14_false_alarm.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

int main() {
    std::vector<Node> v;
    v.push_back(new Node(1));
    ref Node n = v[0];
    bool grow = false;                      // the push_back below never runs
    if (grow) {
        v.push_back(new Node(2));
    }
    std::cout << n.v << std::endl;          // the checker does not evaluate conditions: it must assume the worse path
    return 0;
}
```
<!-- error: 14_false_alarm.clg -->
```
error[semantic/use-after-move]: use of dangling observer `n` (its owner `v` is gone)
  --> 14_false_alarm.clg:15:18
  help: `n` observes `v`, which no longer has a live owner
```

`grow` is `false`, so the `push_back` never runs, but the checker does not know that and assumes it might. The remedy is to write the program so that the order shows the fact (bind the observer after the last
change to the container):

<!-- example: 14_false_alarm_workaround.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

int main() {
    std::vector<Node> v;
    v.push_back(new Node(1));
    bool grow = false;
    if (grow) {
        v.push_back(new Node(2));
    }
    ref Node n = v[0];                      // take the observer after the last change to v
    std::cout << n.v << std::endl;
    return 0;
}
```
<!-- output: 14_false_alarm_workaround.clg -->
```
1
```

This is the trade the project chose: a smaller language whose analysis can be complete, over a larger one whose analysis has to guess. Rejected code is outside the language, not a bug in it. When a refusal
seems wrong, the question to ask is "what is the path the checker cannot rule out?"

## Why there are no lifetime annotations

Rust answers the questions of the last sections by making the programmer write lifetimes in signatures. CeeLing deliberately does not, for three reasons, in order of weight.

1. **Library compatibility.** A lifetime needs a signature that states which parameter a result borrows from. For a function that is a fixed fact. For a C++ template it is not: the same template can
   return a reference into its argument for one instantiation and a copy for another. No single annotation on the template covers both, so a lifetime-annotated language needs the libraries redesigned around it.
   CeeLing instead analyses each instantiation that is used and takes library facts as separate declarations. Here is the template problem in C++:

<!-- example: 14_borrow_depends_on_instance.cpp -->
```cpp
// One template, two instantiations: with the first, call returns a reference into its argument; with the second, a copy.
// A signature that says which has to be written per instantiation; one annotation on the template cannot.
// Run-time effect: none as written; keeping the first result past the life of the argument would be undefined behaviour.
#include <iostream>

template <class F>
decltype(auto) call(F f, int& x) { return f(x); }

int main() {
    int a = 1;
    auto byRef = [](int& x) -> int& { return x; };
    auto byValue = [](int& x) { return x; };
    int& r1 = call(byRef, a);               // aliases a
    int r2 = call(byValue, a);              // independent of a
    a = 5;
    std::cout << r1 << " " << r2 << std::endl;
    return 0;
}
```

2. **Difficulty.** Lifetime annotations are not a keyword. They are a different analysis, with elision rules, lifetime-generic signatures, a borrow-checking algorithm and diagnostics that make sense
   to readers. The present analysis is built on inference and facts, and would be replaced rather than extended.
3. **Identity.** Having every application author write lifetimes would remove the experience that the language is for, leaving "Rust with C++ syntax", which exists and is stronger.

What is kept is a small, *separate* list of trusted facts at a library boundary (`ext_fn ... borrows N`, the Eigen list), written once by the person who writes a binding. The first would be a type system
for every program; the second is a declaration table. CeeLing is therefore not described as having a borrow system, and the compiler's checks of a program's own code are inference, not annotation:

<!-- example: 14_summary_no_annotation.clg -->
```cpp
// No annotation says that "second" returns its second parameter. The compiler reads the body.
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

Node second(Node a, Node b) {
    return b;
}

int main() {
    Node x = new Node(1);
    Node y = new Node(2);
    Node r = second(x, y);
    std::cout << r.v << std::endl;
    return 0;
}
```

The cost is paid in the checker having fewer facts than Rust's and refusing more, and in having no equivalent of lifetime-scoped threading ([chapter 10](10-threads.md) says what is offered instead).

## Why closures and `ref` fields are closed by construction

Two threats could have been handled by analysis and are not: a closure that outlives what it captured, and a reference stored in a field. Both are removed from the language, so no stage has to reason about them.

A closure is rewritten into an ordinary class *before* any check runs (stage 3): one field and one constructor parameter per captured name, and one method holding the body.

<!-- example: 14_closure_desugar.clg -->
```cpp
class Callback {
public:
    Callback() { }
    virtual void call(int x) { }
};

int main() {
    int base = 5;
    Callback c = closure Callback [base] (int x) {
        std::cout << (base + x) << std::endl;
    };
    c.call(1);
    return 0;
}
```
<!-- output: 14_closure_desugar.clg -->
```
6
```
<!-- generated: 14_closure_desugar.clg "class __closure_" -->
```cpp
class __closure_14_closure_desugar_0;

#ifndef CEELING_DECL_CALLBACK
#define CEELING_DECL_CALLBACK
class Callback {
public:
    virtual ~Callback() = default;
    Callback();
    virtual void call(int x);
};
```

By the time the checks run there is no closure, only a class, and the lifetime analysis needed no new rule, no new death event and no new summary for it. That is sound because capture is by value only:
a captured name is copied into a field, and the capture list is a list of bare names, so a by-reference capture cannot even be written.

<!-- example: 14_closure_by_reference.clg -->
```cpp
class Callback {
public:
    Callback() { }
    virtual void call(int x) { }
};

int main() {
    int base = 5;
    Callback c = closure Callback [&base] (int x) {     // there is no way to write a by-reference capture
        std::cout << (base + x);
    };
    return 0;
}
```
<!-- error: 14_closure_by_reference.clg -->
```
Error: Syntax error at line 9, column 36, node type 'ERROR', near: &base. Suggestion: check whether the statement is missing a trailing ';'.
```

The one thing a by-value capture could still smuggle in is an observer, which would be copied into a field that outlives its frame. The rewrite refuses it:

<!-- example: 14_closure_capture_observer.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

class Callback {
public:
    Callback() { }
    virtual void call(int x) { }
};

void each(Callback cb, int n) {
    cb.call(n);
}

int main() {
    Node owner = new Node(5);
    ref Node observed = owner;
    each(closure Callback [observed] (int x) {
        std::cout << x << std::endl;
    }, 1);
    return 0;
}
```
<!-- error: 14_closure_capture_observer.clg -->
```
Error: error[semantic/closure-captures-observer]: closure captures `observed`, a `ref` observer (line 20, column 10). a `ref` observer is a non-owning reference into its owner's stack frame; capturing it by value would copy that reference into a FIELD that can outlive the observer's own scope, reintroducing exactly the dangling-reference hole this language exists to prevent -- capture the observer's OWNER instead, or restructure so the closure does not need it
```

`ref` fields, `ref` parameters and `ref` returns are absent in the same way ([chapter 6](06-dangling-references.md)): a grammar with no production for them needs no check. The price of by-value capture
is that a closure can never change an enclosing local for the caller to see; see [chapter 9](09-closures.md).

## How the C++ is written

The output is generated from the checked tree, node by node, not by editing the source text. That has a consequence for safety: a construct that was refused cannot appear in the output, because
there is no node for it to be written from. (An older generator worked by text substitution; it can still be selected with a flag, and it does not have this property.)

It also lets the emitter insert what C++ would not: the bounds check on `v[i]`, the null check on `->`, `make_unique` or `make_shared` for `new`, `std::move` at a hand-over, and the pointer type chosen in stage 5.

<!-- example: 14_emission.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

class Box {
public:
    Node item;
    std::vector<int> sizes;
    Box(Node n) { item = n; sizes.push_back(1); }
};

int main() {
    Box box = new Box(new Node(1));
    std::cout << box.item.v << std::endl;
    std::cout << box.sizes[0] << std::endl;
    return 0;
}
```
<!-- output: 14_emission.clg -->
```
1
1
```
<!-- generated: 14_emission.clg "int main()" -->
```cpp
int main() {
    std::unique_ptr<Box> box = std::make_unique<Box>(std::make_shared<Node>(1));
    std::cout << cling::nn(cling::nn(box, "line 16")->item, "line 16")->v << std::endl;
    std::cout << cling::at(cling::nn(box, "line 17")->sizes, 0, 17) << std::endl;
    return 0;
}
```

None of this is written by hand and all of it is readable: the output is meant to be code a person could have written, and it needs only a C++ compiler to build. For a `.h` output name, the compiler
writes only the declarations, so that a separately compiled file can include it.

## Across files

An `#include "x.clh"` has no `x.clh` on disk. It names `x.clg`, which the compiler parses again and adds to the including file's tree, marked as declarations to be skipped by the emitter (stage 2). The
including file therefore sees the bodies of the included file, derives their summaries and checks them again. **The safety analysis across files is the single-file analysis.** A defect that needs a
function from the other file is found:

<!-- example: 14_xf_lib.clg -->
```cpp
// The library half of a two-file program: this file is compiled on its own.
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

void eat(Node n) { }
```

<!-- example: 14_xf_use.clg -->
```cpp
#include "14_xf_lib.clh"

int main() {
    Node a = new Node(1);
    ref Node o = a;
    eat(a);                         // eat is defined in the other file and takes ownership
    std::cout << o.v << std::endl;
    return 0;
}
```
<!-- error: 14_xf_use.clg -->
```
error[semantic/use-after-move]: use of dangling observer `o` (its owner `a` is gone)
  --> 14_xf_use.clg:7:18
  help: `o` observes `a`, which no longer has a live owner
```

Evidence, not proof: a test splits each of the splittable safety fixtures into a library file and a `main` file and requires the same verdict (accepts stay accepts, rejects stay rejects; the few that
differ are listed with their reason in `tests/separate/split_equivalence.py`).

What does need care is the C++ that is generated. Pointer kinds are chosen per file, and a header fixes the kinds of fields and container elements. A file that needs `shared_ptr` where the declaring file,
unaware of that use, chose `unique_ptr`, produces C++ that does not fit. The compiler options `--ptr-demands-out` and `--ptr-demands` write and read what each file needs from the classes it includes,
and a driver repeats the compile until they stop changing. A demand can only make a pointer shared, never the other way. This concerns whether the generated files fit, not what is safe. See
[chapter 12](12-many-files.md).

## Where this stops

* **Stage order matters for what is trusted.** The checks apply to the tree after stage 3. They are not a proof about the C++ compiler's own behaviour on the output.
* **Unknown type names are errors.** The checks follow the types the compiler knows (its classes, structs and enums, `string`, vectors, maps, `Channel`, and the classes a loaded fact pack declares). A
  declaration with another type name, `auto` included, is refused, because no check would see a value of it (until 2026-11-02 such a name was emitted unchanged; [chapter 6](06-dangling-references.md)).
* **The facts are trusted.** The scanned facts are a scanner's draft that nobody has reviewed line by line (`review=unreviewed` in the pack), the hand-written ones are believed, an `ext_fn` is believed, and a library's behaviour behind them is not checked.
* **Per-function.** The analysis does not follow an observer out of a function, which is why no observer can leave one.
* **Granularity and conservatism.** A container is one owner; a condition is never evaluated. Both only ever refuse more.
* **Wording.** The owner named in a message is the checker's record, not always the object that died last.
