# 16. CeeLing, Rust, Circle and C++ profiles

CeeLing is not the first attempt at a safer way to write C++-shaped programs, and it is not the strongest. This chapter says what CeeLing chose not to be, where that leaves it behind Rust, and how it
sits among the other approaches. Everything that is a statement about CeeLing's design is checked against the compiler or the design notes. Statements about other projects are from general knowledge, are kept short,
and were **not re-verified** for this chapter; read them as orientation, not as citations.

## The position

The goal, settled in the design notes, is **measurably safer than hand-written C++ while keeping the C++ ecosystem and requiring no lifetime annotations.** Compatibility matters more than guarantee
strength. CeeLing is deliberately not trying to match Rust:

- Rust-style safety for C++ exists as a path already (Circle and the "Safe C++" proposal, P3390): lifetime-annotated references, borrow checking and a rewritten standard library. It gives a stronger guarantee
  and demands migration. Building a second copy of that road gains nothing.
- Whoever needs the strongest guarantee, or full data-race freedom, should use Rust.
- The way success is judged is by counting: for a task, how many errors that hand-written C++ allows are impossible, rejected or caught at run time. For cJSON that count is in [chapter 15](15-case-study-cjson.md).

## Why CeeLing does not use Rust's borrow checker

An obvious shortcut: translate CeeLing to Rust internally and let `rustc`'s borrow checker act as the oracle. It was considered and rejected, for three reasons that are about the design.

**Information.** Dangling analysis does not need a library's internals. It needs each function's *borrow signature*: which parameters are borrowed, what the return value borrows from, which parameters are
moved. For opaque C++ libraries those facts have to be written by hand, and Rust would need exactly the same hand-written declarations. Rust would cover no more than a purpose-built analyser while adding two translation
layers and the work of mapping `'a` and `E0499` diagnostics back to `.clg` lines.

**Ecosystem.** CeeLing's bet is the C++ ecosystem plus memory safety. Rust's borrow checker analyses well-formed Rust; there is no Rust program for `rustc` to check that stands for a C++ class template, or for
Eigen's expression templates. Calling C from Rust works because the caller writes `unsafe extern` declarations, and CeeLing's `ext_fn ... borrows N` is the same idea. What that route cannot do is import C++
templates and class and view semantics at all, whereas CeeLing emits plain C++ and links to them natively.

**The bundle.** Rust sells liveness and aliasing control (`&mut` exclusivity) together. To borrow its liveness analysis you accept its aliasing rules, and CeeLing allows what they forbid. This runs and prints
`3`; the same shape written with Rust references is a borrow error:

<!-- example: 17_aliasing_accepted.clg -->
```cpp
class Counter {
public:
    int n;
    Counter() { n = 0; }
    void bump() { n = n + 1; }
};

int main() {
    Counter a = new Counter();
    Counter b = a;                     // two live handles to one object
    b.bump();
    a.bump();
    b.bump();
    std::cout << a.n << std::endl;     // every handle sees every write
    return 0;
}
```
<!-- output: 17_aliasing_accepted.clg -->
```
3
```

(The Rust claim is the guide's general knowledge of Rust, not something its tools check.) What CeeLing guarantees regardless of aliasing is that every reference the analyser lets you use points at a
live object. Surprising mutation through a second alias is a logic bug, not memory corruption, and CeeLing does not police it; Java programmers live with the same trade. It also gives up Rust's `noalias` optimisations.

The checker CeeLing built is, in effect, the liveness half of a borrow checker, with a per-library facts table in place of lifetime annotations.

## Why there is no `borrow<T>`

Three reasons, in order of weight.

1. **Library compatibility, specifically templates and generics.** A borrow system needs every signature to say which parameter a result borrows from. For ordinary functions that is a fixed fact someone can declare.
   For templates (iterators, ranges, algorithms taking arbitrary callables, Eigen) it depends on the instantiation, so no single signature exists, and the library has to be redesigned around the annotations. CeeLing instead
   analyses each concrete instantiation actually used and takes library facts from tables. (That the Rust-style proposal came with a new standard library is from memory and not re-verified.)
2. **Difficulty.** It is a different analysis, not a keyword: lifetime syntax and elision rules, lifetime-generic signatures, a borrow-checking algorithm, usable diagnostics. The present analysis would be replaced, not
   extended.
3. **Identity.** Making users write lifetimes removes the Java-like experience and leaves "Rust with C++ syntax", which already exists and is stronger.

Distinguish two things. What CeeLing **rejects** is a user-facing borrow *checker*: annotations written by every application author. What it **keeps** is a small list of *trusted borrow facts at a library boundary*
(`ext_fn ... borrows N`, the Eigen view list), written once by a binding author. The first is a type system; the second is a declaration table. CeeLing should not be described as "having borrow".

The cost, stated plainly: the checker has fewer facts than Rust's and refuses more. There is also no equivalent of `thread::scope` (threads borrowing the caller's locals), because `ref` cannot be returned, stored in a field or
passed as a parameter, so an observer can never outlive its block. Good for dangling safety; it also means "borrow this array into N worker threads" cannot be expressed.

## One task, three ways

Keep a handle on the best score, add another score, print the best. In C++ it compiles without a warning and reads freed memory whenever the vector reallocated:

<!-- example: 17_push_while_observing.cpp -->
```cpp
// Run-time effect: undefined behaviour. The first element's address is kept across a push_back that may reallocate.
#include <iostream>
#include <vector>

int main() {
    std::vector<int> scores;
    scores.push_back(10);
    int& best = scores[0];             // a reference into the vector's storage
    scores.push_back(20);              // may move the storage
    std::cout << best << std::endl;    // reads freed memory if it did
    return 0;
}
```cpp
```
<!-- error: 17_push_while_observing.clg -->
```
error[semantic/use-after-move]: use of dangling observer `best` (its owner `scores` is gone)
  --> 17_push_while_observing.clg:12:18
  help: `best` observes `scores`, which no longer has a live owner
```cpp
class Job {
public:
    Job() { }
    virtual void run() { }
};

int main() {
    Channel<int> results;
    int base = 5;
    spawn(closure Job [results, base] () {      // the Channel handle and a copy of base cross; nothing else can
        results.send(base + 10);
    });
    if (results.wait(5000)) {
        std::cout << results.take() << std::endl;
    }
    return 0;
}
```
<!-- output: 17_channel_sum.clg -->
```
15
```

The programming model is "share nothing; send values". It is not "thread-safe": deadlock and waiting forever, shared state behind an `ext_fn`, the file system and the number of threads are unchecked, and any shared
mutable object that cannot be proved flat and unshared is refused rather than analysed. The reason it stops there is structural: complete race checking needs exclusivity tracking, a read/write distinction and
`Send`/`Sync`-style propagation through every type, which are the lifetime and mutability machinery that dropping `borrow<T>` gave up. A data race also breaks the dangling guarantee itself (one thread reassigning a
`shared_ptr` while another reads it), which is why the carve-out is narrow instead of generous. Detail in [chapter 10](10-threads.md).

### 2. Aliasing: not attempted

See above. CeeLing allows aliases and mutation. This is a design choice, not a failure to close a gap, and it is what makes linked lists, caches and observers easy to write.

### 3. Maturity and soundness

`rustc`'s borrow checker is trusted: rarely unsound and validated over years, including by formal work. CeeLing's analysis is new, self-built and tested by fixtures. Each implementation hole is a route to undefined behaviour.
Two ways around the checker were found by hand in two days (an unknown call inside `try`; a `ref` bound to a field, a getter or a temporary) and fixed; the cJSON port found further compiler defects, one of them a silent
miscompilation ([chapter 15](15-case-study-cjson.md)). The list of things the checker does not cover at all (iterators, views, uninitialised fields, cycles) is in [chapter 13](13-contract-and-threats.md). "Measurably safer
than C++" is a claim to be tested, not a certificate.

### 4. Ecosystem safety is by curation

CeeLing reuses `std` and other C++ libraries, which are full of interfaces that can produce undefined behaviour. The guarantee about a call into a library is only as good as the table entry for it. The default is rejection:

<!-- example: 17_unknown_call_rejected.clg -->
```cpp
int main() {
    int a = 1;
    int m = std::max(a, 3);   // returns a reference to one of its arguments; no entry in the library facts: not trusted
    std::cout << m << std::endl;
    return 0;
}
```
<!-- error: 17_unknown_call_rejected.clg -->
```
error[semantic/forbidden-construct]: call to unknown function/method `std::max`
  --> 17_unknown_call_rejected.clg:3:13
  help: CeeLing has no borrow/ownership summary for anything it did not compile itself; unknown calls are rejected by default rather than silently trusted (the default-reject rule). Define `std::max` in CeeLing source, or call an existing CeeLing function/method instead.
```

Rust's crates are safe by default; CeeLing's reach is safe by curation, and what is covered today is `std` containers and strings, a few Eigen calls, and primitive-only `ext_fn` shims. Fact packs
([chapter 11](11-talking-to-cpp.md)) are how that table is extended.

### 5. Move semantics: a patch, not the root fix

In Rust a moved-from value is unusable by the type system. In C++ a move leaves a valid but unspecified object that is still accessible, and the compiler accepts every use of it (and, here, says nothing even at `-Wall -Wextra`):

<!-- example: 17_move_leaves_shell.cpp -->
```cpp
#include <iostream>
#include <utility>
#include <vector>

void consume(std::vector<int> v) { (void)v; }

int main() {
    std::vector<int> data{1, 2, 3};
    consume(std::move(data));          // data was moved from
    std::cout << data.size() << std::endl;   // legal: a valid but unspecified "empty shell"; the compiler does not object
    return 0;
}
```

CeeLing simulates Rust's effect with a static use-after-move check on the same lifetime table as the dangling analysis:

<!-- example: 17_vector_moved_rejected.clg -->
```cpp
void consume(std::vector<int> v) { }

int main() {
    std::vector<int> data;
    data.push_back(1);
    consume(data);                     // the vector is handed over
    std::cout << data.size() << std::endl;
    return 0;
}
```
<!-- error: 17_vector_moved_rejected.clg -->
```
error[semantic/use-after-move]: use of moved-from value `data`
  --> 17_vector_moved_rejected.clg:7:18
  help: `data` was moved earlier; assign a new value before using it
```

It works, but it is an after-the-fact check on a language whose moves are not destructive, not a type-system guarantee, and it relies on the checker being right. See [chapter 5](05-moves.md).

## Feature by feature: Rust's safe subset and CeeLing

CeeLing covers several of the mechanisms that make Rust's safe subset safe, in a weaker form: they are inferred over C++ instead of declared with lifetimes, and some are checked at run time instead of compile time. Others are left out on purpose. This is a list, not a score.

| Rust guarantee | CeeLing |
|---|---|
| No use after free / dangling references | **Yes, at compile time**, by tracking what each reference observes and when its owner dies. Inferred, no lifetimes in signatures. The open gaps are in [chapter 13](13-contract-and-threats.md) |
| Ownership and move semantics; use after move is an error | **Yes.** The compiler inserts the moves and rejects a use after one, across calls, branches, loops and nested expressions |
| No double free, no manual `delete` | **Yes.** `delete`, raw `new` and raw pointers cannot be written; cycles become `weak_ptr` |
| No iterator invalidation | **Mostly.** A view or reference into a container dies at every call that can reallocate it, and iterators cannot be held at all (whole-vector methods such as `sort` replace them). A loop that grows the container it is walking is not caught yet |
| Bounds-checked indexing | **Yes, at run time** (`v[i]`, `s[i]`), aborting with the `.clg` line |
| No null | Not by type; **a run-time check on every object handle**, aborting with the line |
| No uninitialised reads | **Yes** for locals; number, bool and char fields start at zero |
| `unsafe` is explicit and bounded | **Yes.** Casts, `union` and `.get()` are sealed; `ext_fn` is the declared boundary, listed by `--report-facts` |
| `Send` / `Sync`, data-race freedom | **Narrower, on purpose:** `spawn` accepts only plain values and moved objects, `Channel<T>` is the one shared queue. General data-race freedom is not claimed |
| Exclusive `&mut` / shared `&` aliasing rules | Not attempted. CeeLing tracks lifetimes, not aliasing |
| Use of existing C++ libraries | **Native.** The output is C++; what each library does with borrowed data comes from fact packs ([chapter 11](11-talking-to-cpp.md)) |

Where the strongest guarantee, full data-race freedom or lifetime-checked APIs are required, use Rust. Where the code already is, or must be, C++, CeeLing gives much of the memory-safety benefit without leaving it.

## Questions people ask

These are the objections that come up when the project is described in public; a discussion thread about Rust, C++ and Zig prompted this list. Each answer says what the design does and where the evidence is missing. Statements about other projects are from general knowledge and were not re-verified.

### Is it a replacement for C, or for C++?

Neither. It is for new code in a C++ project, next to C++ libraries that stay as they are. Program size and memory safety are separate axes: Rust is large and safe, C is small and unsafe, and CeeLing moves along the safety axis while keeping C++'s libraries. Its visible language is smaller than C++ (code outside it is rejected), but it is not minimal, and it does not try to be. Someone who wants a small language to replace C is better served by Zig or by C itself.

### Rust's cost is its size, not its borrow checker

A common report in that discussion is that the borrow checker itself makes sense and what wears people out is the size of the language and the lifetime annotations it asks for. CeeLing is a bet on that observation: no annotations, ownership inferred, and the price is a weaker guarantee and a checker that refuses more ([chapter 1](01-why-ceeling.md#what-it-costs-honestly)). Whether the bet holds for other people is untested; there has been no user study, only the four projects in [chapter 18](18-status.md).

### Is an inferred `shared_ptr` just garbage collection?

By one common definition reference counting is a kind of garbage collection, and the pointers CeeLing infers include `shared_ptr`. What it does: `unique_ptr` when a single owner can be shown, `shared_ptr` when it cannot ("ambiguous reads as shared", [chapter 4](04-ownership.md)), `weak_ptr` on a cycle. Destruction is still deterministic, there is no tracing and no pause. The cost is real, though: in app-cling 87% of declarations are shared ([chapter 16](16-case-study-app-cling.md)), so for a program shaped like that the model is closer to reference counting than to single ownership. The compiler does not yet print this count for a program; it is on the TODO list.

### Is it the first safe dialect of a C-like language?

No. Cyclone (a research dialect of C with region-based memory and checked pointers, which influenced Rust), CCured (infers a pointer kind for each pointer in C and inserts run-time checks) and Checked C (annotations that give C pointers bounds) all tried to make C-family code safe. As I recall, they asked for porting, annotation or fatter pointers, and none became mainstream. CeeLing shares CCured's mix of inference and run-time checks, and differs in two ways: it does not try to accept existing source, and its output is plain C++ that calls existing C++ libraries directly. That is a difference of approach, not evidence that it will fare better.

### Why not hardened libraries, sanitizers and static analysis?

Hardened standard-library modes check `operator[]` bounds inside the library, which has the same effect as CeeLing's bounds check. AddressSanitizer finds use after free and overflows on the paths a test runs. The compiler's own lifetime warnings find simple dangling references. None of them rejects a program whose dangling reference no test executes; CeeLing rejects it at compile time (the `head` example in the README). CeeLing's own tests use the sanitizers for the same reason: they find what a test reaches. What has not been done is the side-by-side count: running those tools over cJSON's 33 issues and comparing with the table in [chapter 15](15-case-study-cjson.md). Until then this is a design argument, not a measurement.

## The landscape

None of the following is verified against current sources for this chapter. It is the shape of the field as the project's own notes record it, with the hedges they carry.

| | Syntax | C++ libraries | How dangling is handled | Status (as recalled, not re-verified) |
|---|---|---|---|---|
| **Rust** | its own: `&`, `&mut`, lifetimes | by `unsafe extern` for C; no route for C++ templates | borrow checker, lifetimes in signatures | shipped, mature |
| **Circle / "Safe C++" (P3390)** | C++ superset plus a `safe` context | usable; in my understanding calling legacy C++ from `safe` requires `unsafe` | Rust-style lifetimes and exclusivity | not adopted by the ISO committee; neither it nor profiles made C++26 (per the design notes) |
| **C++ profiles** | unchanged C++ | all of it | rules checked on existing code; the committee's lifetime paper aims at catching common dangling | the committee's chosen direction; not in C++26 |
| **cppfront / Cpp2** | a new syntax that transpiles to C++ | all of it | bounds, null and initialisation checks; per its own documentation it does not aim for 100 percent | experimental |
| **Carbon** | a new syntax | via C++ interop | memory safety is a design effort | experimental |
| **Cyclone / CCured / Checked C** | dialects of C | C only | region types, inferred pointer kinds with run-time checks, bounds annotations | research projects or niche; see above |
| **CeeLing** | Java-flavoured, C++-looking | plain C++ output; the facts table limits what is trusted | provenance and death events inferred; no annotations; the gaps in chapter 13 | working compiler, tested on real libraries ([chapter 18](18-status.md)) |

What differs, in one line each.

- **Against Rust and Circle:** they give a stronger guarantee and demand annotations and migration. CeeLing gives a weaker one and demands neither.
- **Against profiles:** the same instinct (restrict instead of rewrite), but profiles try to retrofit arbitrary existing code, which forces them to permit exemptions and settle for catching common dangling. CeeLing drops
  backward compatibility with unsafe *source* so that the accepted language can be small enough for the analysis to be complete. What it keeps is compatibility with *libraries*. Whether that bet holds is what the cJSON
  matrix tests.
- **Against cppfront:** the nearest neighbour in method (a transpiler to C++, bounds and null checks, no claim of full safety). cppfront's value is a cleaner new syntax; CeeLing's is compile-time dangling safety while
  keeping familiar syntax.
- **On the output:** the generated C++ is plain C++, so users of a library written in CeeLing never need CeeLing. This is a property that "a subset of C++ checked by an extra tool" would have, but CeeLing is not that: `Node n = new Node(1);` means something
  different in C++ than here, and the language has its own keywords. Strictly it is a restricted dialect that transpiles. That is why it could not be proposed to the C++ committee as it stands.

A competitor that gave Java-like syntax, C++-library reuse and compile-time dangling safety at once would remove the reason for CeeLing to exist. This chapter does not claim anyone has or has not; it records that the
three together are CeeLing's whole aim, and that it reaches the third only with the gaps listed in [chapter 13](13-contract-and-threats.md).

## The line the design holds

CeeLing checks *liveness*, not *aliasing*. Adding exclusivity rules, function colouring or lifetime annotations would strengthen the guarantee and would end the two things the project is for: the Java-like experience and
the ability to admit unmodified C++ libraries behind the facts table. That is why closures capture by value only (the capture-lifetime analysis that was the alternative would have been exactly the aliasing machinery
the design avoids; see [chapter 9](09-closures.md)), why `ref` cannot be stored or returned, and why `spawn` takes values. When a feature would need the checker to track who else holds a reference, the answer in this
design is to remove the capability, not to add the tracking.
