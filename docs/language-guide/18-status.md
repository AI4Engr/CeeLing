# 17. Status and roadmap

What is built and tested, what is a stated gap, what is planned, what real programs have been written in CeeLing, and the licence. No phases and no history: this is the state of the language as the
documents and the compiler agree on it, with the places where they disagree named. This page is the list, grouped.

## What exists and is tested

| Area | State | Where |
|---|---|---|
| Parsing, code generation to plain C++, smart-pointer inference (`unique_ptr`, `shared_ptr`, `weak_ptr` from the ownership shape) | working | [2](02-language.md), [4](04-ownership.md) |
| Automatic `std::move` and static use-after-move | working | [5](05-moves.md) |
| Dangling-reference analysis: provenance, death events (move, reassignment, scope exit, container mutation), branch and loop merges, return and parameter rules | working, with the gaps below | [6](06-dangling-references.md) |
| Sealed escape hatches: raw pointers, `new`/`delete`, `.get()`, casts, `const_cast`, raw `union`, file-scope variables, unknown calls | working | [8](08-sealed-escape-hatches.md) |
| Run-time checks: bounds on `v[i]` and `s[i]`, null on every object-handle dereference; both on by default, opt-out per compile | working | [7](07-run-time-checks.md) |
| Closures, by-value capture only | working | [9](09-closures.md) |
| `spawn`, a worker pool, failure handlers, and `Channel<T>` | working, narrow | [10](10-threads.md) |
| `ext_fn` declarations, `borrows N`, `ref` parameters, the library facts table | working, deliberately narrow | [11](11-talking-to-cpp.md) |
| Several files, `.clh` includes, pointer demands for separately compiled files | working | [12](12-many-files.md) |
| Generated JSON decoders for structs (`decodeX`) | working | [3](03-values-and-containers.md) |

The safety contract is tested by `tests/safety/`: 306 fixtures (186 that must compile, 120 that must be refused), 154 of them also run with their output asserted and 7 that must die at run time with a stated message. Every
accepted fixture's generated C++ is compiled by `g++`. A second suite splits each splittable fixture across two files and requires the same verdict (279 fixtures). The guide's own examples are another suite, compiled
and run by the same compiler. How to read these is [chapter 13](13-contract-and-threats.md); the fixture and cross-file counts were re-run on 2026-11-09.

## The run-time checks, and how a program is built

A claim about a program is a claim about how it was built. Both checks can be turned off, and then the output is a plain C++ subscript and a plain `->`:

<!-- example: 18_checks_off.clg -->
```cpp
int main() {
    std::vector<int> v;
    v.push_back(1);
    int i = 0;
    std::cout << v[i] << std::endl;    // built with both checks off: a plain C++ subscript, as in zlib-cling
    return 0;
}
```
<!-- generated: 18_checks_off.clg "int main()" -->
```cpp
int main() {
    std::vector<int> v;
    v.push_back(1);
    int i = 0;
    std::cout << v[i] << std::endl;
    return 0;
}
```

(The example is compiled with `--no-bounds-checks --no-null-checks`.) `zlib-cling` is built with both off for speed parity with its Rust reference; `cJSON-cling` with both on. The cost measured on zlib-cling is 5.5
percent on decompression and 11.3 percent on compression.

## Stated gaps

Grouped; each is described where it belongs and listed in [chapter 13](13-contract-and-threats.md). None of them is a plan to be quiet about.

**Dangling analysis does not see:** a loop that grows the container it walks (a stored iterator is refused at `begin()`, not tracked), every Eigen view method beyond eight on one type. (`string_view` is followed since 2026-11-03 and `std::span` since 2026-11-04; Eigen's lazy `auto` expressions cannot be written: since 2026-11-02 a type name the compiler does not know, `auto` included, is an error. They used to pass through unchanged.) Also: `ref` observers are raw pointers in the
generated C++, so a miss in the analysis is not caught by a run-time check (making them self-checking is an open item); an observer of a container element depends on the whole container, which refuses more than necessary.

**Other static checks that stop short:** `std::variant` (never guarded), reference cycles (a back link becomes `weak_ptr` by a naming rule or a hint, not by cycle
detection, so a leak is possible), `const` on methods (accepted by the grammar and not enforced).

**Run-time checks that stop short:** `pop_back()` on an empty vector, iterators past `end()`, anything an `ext_fn` indexes, `this`, a bad `substr` position. The last is the clearest: it is not a subscript, so nothing of
CeeLing's is involved, and the program is ended by an uncaught C++ exception:

<!-- example: 18_gap_substr.clg -->
```cpp
int main() {
    string s = "abc";
    string t = s.substr(10);           // not a subscript, so no CeeLing check: libstdc++ throws std::out_of_range
    std::cout << t << std::endl;
    return 0;
}
```
<!-- output: 18_gap_substr.clg -->
```
terminate called after throwing an instance of 'std::out_of_range'
  what():  basic_string::substr: __pos (which is 10) > this->size() (which is 3)
```

`catch (string e)` does not catch it, so it ends the program, which is an outcome, not a safe one.

**Threads:** `spawn` admits copies of plain values and flat objects moved in. Everything else is refused, not analysed:

<!-- example: 18_spawn_object_with_object_field_rejected.clg -->
```cpp
class Inner {
public:
    int v;
    Inner(int x) { v = x; }
};

class Outer {
public:
    Inner part;                        // an object-typed field: not a flat object
    Outer(int x) { part = new Inner(x); }
};

class Job {
public:
    Job() { }
    virtual void run() { }
};

int main() {
    Outer o = new Outer(1);
    spawn(closure Job [o] () {
        std::cout << o.part.v << std::endl;
    });
    return 0;
}
```
<!-- error: 18_spawn_object_with_object_field_rejected.clg -->
```
error[semantic/spawn-captures-object]: `spawn` cannot move `o` into a new thread: field `part` of class `Outer` is not a plain value (type `Inner`); only primitives, string, vectors of those and plain structs are allowed
  --> 18_spawn_object_with_object_field_rejected.clg:21:5
  help: an object may be moved into a thread only if nothing else can reach it and nothing about it is hidden: make the data a `struct` (copied), or serialize it to a string and rebuild it inside the thread
```

The guarantee is data-race freedom through `spawn` and `Channel` in checked code. Deadlock, waiting forever, `ext_fn` and shim state, the file system and the thread count are unchecked. Objects with object fields,
objects from calls or parameters and cJSON-style trees cannot be sent.

**Trusted boundaries:** `ext_fn` declarations, shims, libraries, the C++ compiler. One bug is known and unfixed here: a *local* vector passed to an `ext_fn ref std::vector<T>` parameter is emitted as
`std::move(local)` though the parameter is `const&`, and the checker does not mark the local moved-from:

<!-- example: 18_ext_fn_ref_local.clg -->
```cpp
ext_fn int firstOf(ref std::vector<int> v);        // read-only borrow: lowers to const std::vector<int>&

int main() {
    std::vector<int> data;
    data.push_back(4);
    int x = firstOf(data);             // a local passed to a borrowing parameter
    std::cout << x << std::endl;
    return 0;
}
```
<!-- generated: 18_ext_fn_ref_local.clg "int main()" -->
```cpp
int main() {
    std::vector<int> data;
    data.push_back(4);
    int x = firstOf(std::move(data));
    std::cout << x << std::endl;
    return 0;
}
```

The `std::move` is the bug. A later read of `data` would see the moved-from state and the checker would not object.

**Accepted by CeeLing, refused by the C++ compiler afterwards:** a number of mistakes (an int assigned to an enum, a closure assigned to a field outside a constructor, a repeated name in a capture list, a local copied
from a field, `ref` on a primitive or a vector). Not a safety hole, but "CeeLing accepted it" is not "it builds"; see [chapter 13](13-contract-and-threats.md). A shift beside a comparison binds the other way round from C++
([chapter 2](02-language.md)).

**Documents that disagree with the compiler.** These I found while writing the guide, and the guide follows the compiler:

- an earlier statement that `std::string` parameters are "not yet admitted" for the read-only `const&` inference is out of date; the compiler does it ([chapter 3](03-values-and-containers.md), and `cJSON-cling/docs/performance.md`).
- "definite initialization" is a static check for locals only; number, bool and char fields start at zero instead ([chapter 8](08-sealed-escape-hatches.md)).
- `auto` used to parse and pass through; it is refused since 2026-11-02 ([chapter 6](06-dangling-references.md)).
- The `cling_safety_contract_baseline` test is not run with `--strict`, so a missed reject shows as XFAIL, not as a failure.

## Not in the language yet

These are planned or wanted, and rejected today. Two examples that fail on purpose:

<!-- example: 18_not_yet_literal.clg -->
```cpp
int main() {
    std::vector<int> v = {16, 17, 18};     // container literals are on the roadmap, not in the language
    std::cout << v.size() << std::endl;
    return 0;
}
```
<!-- error: 18_not_yet_literal.clg -->
```
Error: Syntax error at line 2, column 26, node type 'ERROR', near: {16, 17,
```

<!-- example: 18_not_yet_constant.clg -->
```cpp
const int LIMIT = 65536;                   // named constants are on the roadmap, not in the language

int main() {
    std::cout << LIMIT << std::endl;
    return 0;
}
```
<!-- error: 18_not_yet_constant.clg -->
```
Error: Syntax error at line 1, column 7, node type 'ERROR', near: int. Suggestion: check whether the statement is missing a trailing ';'.
```

The list, roughly in the order it would help real programs:

- **`auto` is not supported and is not planned.** Every check works from a variable's declared type, and `auto` is how iterators, views of temporaries and Eigen expression templates are written in C++. Supporting it would mean typing every expression of every linked C++ library. The declaration is refused, numbers included ([chapter 2](02-language.md)); write the type.
- **Container literals and named constants.** Both came from zlib-cling, where a table is 19 `push_back` calls and `65535` is written 18 times.
- **Compound assignment, `^`, ternary, `ref int` out-parameters or multiple return values.**
- **Generics beyond the one narrow form** (a generic method whose return type is the type parameter): classes and free functions, analysed per instantiation.
- **Closing the dangling gaps:** iterators and views, tracking a mutation that invalidates a `ref` beyond a container's `push_back`, `ref` observers that check themselves.
- **`const` enforced**, then deeply immutable types, which would also be the simplest "safe to share" category for threads.
- **Cycle detection** in place of the naming rule.
- **Distribution.** Generated C++ is meant to be shipped as an ordinary library; the release standard (a consumer test, self-contained output, a provenance header, safety wording per README, a licence statement for generated code) is
  written down and none of it is implemented.
- **Library declaration packs and a scanner** that drafts the borrow facts for a C++ library for a human to review (below).
- **Generated serialisation to JSON** (`toJson`); decoding exists.

Not planned: lifetime annotations (`borrow<T>`), a Rust backend, Rust-equivalent safety, full data-race checking. [Chapter 17](17-comparisons.md) gives the reasons.

## Real programs written in CeeLing

The language is exercised by libraries that were written for their own sake, not as tests.

| Project | What it is | What it showed |
|---|---|---|
| **cJSON-cling** | All of cJSON 1.7.19 and its Utils (JSON Pointer, Patch, Merge Patch, sorting): 2,907 lines of `.clg` and a 52-line number shim. | The 33-issue case study ([chapter 15](15-case-study-cjson.md)); differential tests against upstream; clean sanitizer runs; and a compiler defect list that was fixed along the way. Built with both run-time checks on. |
| **yaml-cling** | A YAML library with the yaml-cpp API, transpiled and consumed by a plain C++ project through generated headers and sources. | That a graph-shaped library (nodes, vectors and maps of nodes, strings) can be written without raw pointers, and that its users never need CeeLing. Milestone-gated. |
| **zlib-cling** | DEFLATE and the zlib format, about 3,500 lines of algorithm code and a small shim. | Real-corpus parity: verified in both directions against real zlib on the 12-file, 15.7 MB Silesia corpus; compress 0.97 times and decompress 1.10 times the time of zlib-rs on the author's machine (its README). Built with the bounds checks off, which is stated there. It drove `reserve`, `copyWithin`, `int64` and the read-only parameter inference. |
| **app-cling** | A Wails-like desktop shell: a web page calls commands written in CeeLing, over a small native shim. | The threads model (`spawn`, `Channel`) in real use, with ASan, UBSan and ThreadSanitizer runs and lock-removal controls; 982 headless checks (re-run on 2026-11-09); the struct decoders. A prototype. |

These are figures from the projects' own documents; I did not rerun them. Each project's claim is limited as this guide's is: its `.clg` code is checked, its shims and the libraries it calls are not.

## Licence

The compiler and the language are **MIT-licensed** (`LICENSE`, copyright AI4Engr). zlib-cling and yaml-cling carry their own `LICENSE` and a `NOTICE` reproducing the upstream licence text.

## Where to go next

For what the guarantee is: [13](13-contract-and-threats.md). For how the compiler is built: [14](14-how-it-works.md). To judge it against Rust and the rest of the field: [17](17-comparisons.md).
