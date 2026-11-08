# CeeLing

**Rust-style memory safety for C++, with no annotations and no rewrite of your libraries.**
CeeLing is a memory-safe dialect of C++ that transpiles to plain C++. You write Java-like code with no `*`, `&`, `delete`, `std::move` or lifetime annotations. The compiler infers who owns what, chooses `unique_ptr` / `shared_ptr` / `weak_ptr`, inserts the moves, and **rejects at compile time** any program that could contain a dangling reference or a use after move. Out-of-bounds indexing and null dereferences are caught at run time with the source line, raw pointers and casts are sealed, and the one escape hatch (`ext_fn`) is explicit, like `unsafe`.

```
 hello.clg ──► cling ──► hello.cpp ──► g++ / clang++ / MSVC
              checks +      plain C++17
              inference
```

The output is ordinary C++17: your code calls `std`, Eigen or any C++ library directly (no bindings), and a library written in CeeLing is used from C++ as a normal header and source file, so its users do not need CeeLing. It has been used to rewrite [real libraries](#real-programs-written-in-ceeling): all of cJSON, DEFLATE/zlib, a YAML library and a desktop shell.

> **Status.** A working compiler, pre-1.0, developed and tested on Windows (MinGW-w64) with the generated code checked under g++. The language is deliberately small: code outside it is rejected, not guessed at. Read [what it promises](#what-it-promises) before relying on it.

---

## Examples

### Ownership without annotations

```
class Node {
public:
    int key;
    Node next;
    Node(int k) { key = k; }
};

int main() {
    Node head = new Node(1);
    head.next = new Node(2);
    std::cout << head.next.key << std::endl;
    return 0;
}
```

becomes

```cpp
class Node {
public:
    int key{};
    std::unique_ptr<Node> next;          // one owner: unique_ptr
    Node(int k);
    ~Node();                             // iterative: a long chain cannot overflow the stack
};

int main() {
    std::unique_ptr<Node> head = std::make_unique<Node>(1);
    cling::nn(head, "line 10")->next = std::make_unique<Node>(2);     // null check that names the .clg line
    std::cout << cling::nn(cling::nn(head, "line 11")->next, "line 11")->key << std::endl;
    return 0;
}
```

Several owners become `shared_ptr`; a back link that closes a cycle becomes `weak_ptr`. The choice is made from how the objects are used ([guide chapter 4](docs/language-guide/04-ownership.md)).

### A dangling reference is a compile error

```
Node owner = new Node(1);
ref Node view = owner;          // `ref` observes; it does not own
owner = new Node(2);            // the first object is gone
std::cout << view.key;          // would read freed memory in C++
```

```
error[semantic/use-after-move]: use of dangling observer `view` (its owner `owner` is gone)
  help: `view` observes `owner`, which no longer has a live owner
```

The same analysis covers views of containers: a `std::span` or `string_view` dies with the vector or string it looks into.

```
std::vector<int> v;
v.push_back(1);
std::span<int> s = v;
v.push_back(2);                 // may reallocate
std::cout << s.size();
```

```
error[semantic/use-after-move]: use of dangling observer `s` (its owner `v` is gone)
```

### Use after move is a compile error

Passing an object or container hands it over. The compiler moves it for you and rejects a later use:

```
std::vector<int> w = consume(v);
std::cout << v.size();
```

```
error[semantic/use-after-move]: use of moved-from value `v`
  help: `v` was moved earlier; assign a new value before using it
```

### What cannot be proved is checked at run time

Every `v[i]` and `s[i]` is bounds-checked and every object-handle dereference is null-checked. A failure prints the `.clg` line and aborts; it does not continue:

```
cling: index out of bounds at line 5 (index 3, size 1)
```

Both checks are on by default (`--no-bounds-checks`, `--no-null-checks`).

### Threads that share nothing

`spawn` hands a thread only plain values (copied) or an object moved in; `Channel<T>` is the one shared, mutex-guarded queue. A capture that could be shared is a compile error ([guide chapter 10](docs/language-guide/10-threads.md)).

---

## What it promises

> **If a program compiles under CeeLing, it contains no dangling references and no use after move**, plus the two run-time checks above.

That covers the code CeeLing checked. It does **not** claim Rust-equivalent safety, thread safety in general (only the `spawn` and `Channel` model is checked), or anything about the C++ that CeeLing calls: `ext_fn` shims and the libraries you link are trusted boundaries, and `cling --report-facts` lists every library fact a program relied on. Logic errors are not memory errors and are not touched. The contract and the table of threats, with what is closed and what is open, are in [chapter 13](docs/language-guide/13-contract-and-threats.md) and [chapter 18](docs/language-guide/18-status.md).

The positioning is deliberate: safer than hand-written C++ while keeping the C++ ecosystem (std, Eigen, your own libraries) and requiring no annotations. Where the strongest guarantee is needed, use Rust.

## Compared with Rust

Most of what makes Rust's safe subset safe is implemented. The difference is *how* it is established (inference over C++ instead of lifetime annotations) and what is deliberately left out. This is a feature-by-feature list, not a percentage; [chapter 17](docs/language-guide/17-comparisons.md) gives the reasoning.

| Rust guarantee | CeeLing |
|---|---|
| No use after free / dangling references | **Yes, at compile time**, by tracking what each reference observes and when its owner dies. Inferred, no lifetimes in signatures. The open gaps are listed in [chapter 13](docs/language-guide/13-contract-and-threats.md) |
| Ownership and move semantics; use after move is an error | **Yes.** The compiler inserts the moves and rejects a use after one, across calls, branches, loops and nested expressions |
| No double free, no manual `delete` | **Yes.** `delete`, raw `new` and raw pointers cannot be written; cycles become `weak_ptr` |
| No iterator invalidation | **Mostly.** A view or reference into a container dies at every call that can reallocate it, and iterators cannot be held at all (whole-vector methods such as `sort` replace them). A loop that grows the container it is walking is not caught yet |
| Bounds-checked indexing | **Yes, at run time** (`v[i]`, `s[i]`), aborting with the `.clg` line |
| No null | Not by type; **a run-time check on every object handle**, aborting with the line |
| No uninitialised reads | **Yes** for locals; number, bool and char fields start at zero |
| `unsafe` is explicit and bounded | **Yes.** Casts, `union` and `.get()` are sealed; `ext_fn` is the declared boundary, listed by `--report-facts` |
| `Send` / `Sync`, data-race freedom | **Narrower, on purpose:** `spawn` accepts only plain values and moved objects, `Channel<T>` is the one shared queue. General data-race freedom is not claimed |
| Exclusive `&mut` / shared `&` aliasing rules | Not attempted. CeeLing tracks lifetimes, not aliasing |
| Use of existing C++ libraries | **Native.** The output is C++; what each library does with borrowed data comes from fact packs |

Where the strongest guarantee, full data-race freedom or lifetime-checked APIs are required, use Rust. Where the code already is, or must be, C++, CeeLing gives most of the memory-safety benefit without leaving it.

## Case study: cJSON's 33 published issues

To test the claim on a real library, all of **cJSON 1.7.19** was ported to CeeLing (`cjson-cling`, 2,907 lines of `.clg`, a sibling project) and each of the 33 issues published against it was reproduced in C and examined in the port. Chapter 15 of the guide shows, for every issue, the defective C code, a C program that demonstrates it, the port's code and the mechanism. (These are the 33 numbered issues of the published list, not CVE identifiers.)

| Outcome in the port | Issues | Count |
|---|---|---|
| **Cannot happen**: the unsafe operation cannot be written | 1, 2, 3, 4, 7, 12, 13, 30, 32 | 9 |
| **Stopped at run time**: aborts with the source line instead of corrupting memory | 5, 11 | 2 |
| **Partly**: one route closed, another still open | 6, 8, 9 | 3 |
| **Not prevented**: CPU exhaustion | 10 | 1 |
| **Avoided by the library's own discipline, not by the language** | 31 | 1 |
| **Same bug, same behaviour**: a logic error, memory-safe in C too | 14 to 29, 33 | 17 |

Of the 13 memory errors and crashes (issues 1 to 13), 9 are removed or turned into a located abort, 3 are partly open and 1 is not prevented. The 17 logic errors remain, reproduced faithfully: a language that checks memory cannot know that `~1` must decode to `/`.

<details>
<summary><b>All 33 issues</b></summary>

| # | cJSON function | Defect in C | In the port |
|---|---|---|---|
| 1 | `overwrite_item` | frees fields, ignoring the reference flags | cannot happen |
| 2 | `ApplyPatches` | reads the next patch after deleting it | cannot happen |
| 3 | `merge_patch` | walks nodes it already deleted (aliasing) | cannot happen |
| 4 | `ReplaceItemViaPointer` | frees a referenced item; a node can replace its own parent | cannot happen (the cycle is refused by library code) |
| 5 | `DetachItemViaPointer` | write through a `NULL` `parent->child` | stopped at run time |
| 6 | `ReplaceItemInArray` | an array placed inside itself | partly: direct form refused, indirect cycle still overflows the stack |
| 7 | `ParseWithOpts`, `GetErrorPtr` | stale error pointer | cannot happen |
| 8 | `Duplicate` | depth cap too high for the stack | partly (unchanged): stack-dependent |
| 9 | `Delete`, patch `copy` | unbounded recursion | partly: delete fixed, print still recurses |
| 10 | `Compare` | exponential time in nesting depth | not prevented |
| 11 | `apply_patch` | `NULL` object dereference | stopped at run time |
| 12 | `apply_patch`, `detach_path` | string node with a `NULL` value | cannot happen as a crash; reads as an empty path |
| 13 | `cJSON_Utils` allocations | unchecked `malloc` | cannot happen as an unchecked write |
| 14 | `sort_list` | a later append is dropped | same bug (leak gone) |
| 15 | `decode_pointer_inplace` | `~1` decoded in the wrong position | same bug |
| 16 | `decode_pointer_inplace` | invalid escape ignored | same bug |
| 17 | `decode_array_index_from_pointer` | index wraps around | same bug |
| 18 | `decode_array_index_from_pointer` | empty token is index 0 | same bug |
| 19 | `get_item_from_pointer` | no leading `/` resolves to the root | same bug |
| 20 | `detach_path` | mixed case sensitivity | same bug |
| 21 | `generate_merge_patch` | sort and walk use different comparators | same bug |
| 22 | `generate_merge_patch` | recursion through the case-insensitive wrapper | same bug |
| 23 | `apply_patch` | removes the target before validating | same bug |
| 24 | `overwrite_item` | wipes sibling links | same bug (leak gone) |
| 25 | `Minify` | ignores backslash parity | same bug |
| 26 | `print_number`, `compare_double` | imprecise 15-digit output | same bug |
| 27 | `parse_hex4` | invalid digit reads as 0 | same bug |
| 28 | `parse_number` | no overflow check | same bug |
| 29 | `Compare` | duplicate keys, infinities | same bug |
| 30 | `SetValuestring` | pointer comparison across allocations | cannot happen |
| 31 | `CreateNumber` | `NaN` cast to `int` | avoided by the library (explicit saturation), not by the language |
| 32 | `Version`, `global_error` | shared `static` state | cannot happen (no file-scope variables) |
| 33 | `parse_string` | off-by-one error position | same bug |

The triggers are reconstructions from the descriptions (the smallest program that does what each issue says), not the original proofs of concept. [Chapter 15](docs/language-guide/15-case-study-cjson.md) says for every issue how the C behaviour was reproduced.

</details>

Safe is not free. On a 20 MB document the port parses at 86 MB/s against 81 MB/s for upstream, prints at 105 against 69 and frees the tree at 947 against 168, but holds the tree in 1.8 times the memory (bounds and null checks on, `--pool-shared`). These are the project's own figures from one program on one machine (chapter 15).

## Real programs written in CeeLing

The language is exercised by libraries written for their own sake, not as tests. Each lives in its own repository; [chapter 18](docs/language-guide/18-status.md) says what each one shows and what it does not.

| Project | What it is | Result |
|---|---|---|
| **cjson-cling** | All of cJSON 1.7.19 and its Utils (JSON Pointer, Patch, Merge Patch, sorting): 2,907 lines of CeeLing and a 52-line number shim | The 33-issue case study above; differential tests against upstream; clean sanitizer runs. Built with both run-time checks on |
| **zlib-cling** | DEFLATE and the zlib format, about 3,500 lines | Verified in both directions against real zlib on the 15.7 MB Silesia corpus; compress 0.97 times and decompress 1.10 times the time of zlib-rs on the author's machine, with bounds checks off (a checked build is one flag away) |
| **yaml-cling** | A YAML library with the yaml-cpp API, used by a plain C++ project through generated headers | A graph-shaped library (nodes, vectors and maps of nodes, strings) written without raw pointers, consumed without CeeLing |
| **app-cling** | A desktop application shell: a web page calls commands written in CeeLing over a small native layer | The threading model (`spawn`, `Channel`) in real use, under AddressSanitizer, UBSan and ThreadSanitizer, with lock-removal controls; a prototype |

Writing them found compiler defects, one of them a silent miscompilation (`1.0e30` compiled as `1.0`); each was fixed with a regression fixture ([chapter 15](docs/language-guide/15-case-study-cjson.md)).

## Using C++ libraries

What the compiler assumes about a library call is data, not code: plain-text *fact packs* (`facts/*.clfacts`) say which methods borrow, which invalidate a container's views and which return fresh values. The `std::vector`, `string`, `string_view`, `optional` and `map` facts are generated from scans of the real standard library declarations and checked by seeded dangling programs. For your own library:

```bash
python tools/cling-scan/cling_scan.py --header grid.h --class Grid --out grid.clfacts   # draft a pack from the declaration
cling --facts grid.clfacts program.clg --report-facts                                   # compile, and list every fact the program trusted
cling --strict --facts grid.clfacts program.clg                                         # refuse facts no person reviewed
```

An unlisted method of a library class is assumed to borrow its receiver and to invalidate its observers, so the default is the cautious one. Packs are bound to the library headers by SHA-256. See [chapter 11](docs/language-guide/11-talking-to-cpp.md), the [pack format](docs/internals/facts-format.md) and [cling-scan](tools/cling-scan/README.md) (text, libclang and LibTooling backends; a probe checks the facts against the real library under AddressSanitizer).

## How it is tested

| What | How |
|---|---|
| The safety contract | 385 fixtures in `tests/safety/`: each must be rejected, or accepted, compiled, run and its output asserted. The suite is also run split across files and compiled separately |
| The language guide | 565 examples are compiled, run or rejected as their tags say, and every error message quoted in a chapter is what the compiler prints |
| Library facts | seeded dangling programs for every scanned `std::vector` and `string` method (no false accepts, no false rejects), a usage corpus compiled with g++, and negative controls that must fail |
| Real programs | ports of zlib, cJSON, yaml-cpp and a desktop shell are kept in sibling repositories; what each one shows, and what it does not, is in [chapter 18](docs/language-guide/18-status.md) |

## Build

Requirements: CMake, a C++17 compiler (MinGW/GCC/Clang/MSVC) and Git. The generated parser (`tree-sitter-ceeling/src/parser.c`) is checked in, so the tree-sitter CLI is needed only after editing `tree-sitter-ceeling/grammar.js`. The tests also use Python 3 and bash.

```bash
git clone --recursive <this repository>      # vendor/tree-sitter is a submodule
cmake -S . -B build                          # add -G Ninja for a faster build
cmake --build build --parallel
ctest --test-dir build                       # a few tests skip themselves when g++, WSL, Eigen or jsoncpp are missing
```

```bash
build/cling file.clg --cpp out.cpp           # transpile; -g adds #line directives so gdb steps through the .clg
g++ -std=c++17 out.cpp -o program
```

## Documentation

* **[Language Guide](docs/language-guide/README.md)**: 18 chapters, every example checked by the test suite. Start with [why CeeLing](docs/language-guide/01-why-ceeling.md) and [the language](docs/language-guide/02-language.md). The safety mechanisms are chapters 4 to 9, the threat table is chapter 13, how the compiler works is chapter 14, and the comparison with Rust, cppfront and the C++ profiles is chapter 17.
* [Design records](docs/design/): Eigen views, inferring the facts of third-party libraries.
* [Contributing to the guide](docs/language-guide/CONTRIBUTING.md): what the guide may claim.

## Source layout

| path | what |
|---|---|
| `src/syntax/` | the AST and the tree-sitter based AST builder |
| `src/passes/` | closure and struct-JSON desugaring, the escape-hatch, dangling / move and channel checks, smart-pointer inference |
| `src/facts/` | what the compiler knows about library calls (fact tables, `library_summary`) |
| `src/emit/` | the C++ emitter and the runtime helper code it adds to generated files |
| `tree-sitter-ceeling/` | the grammar and the generated parser |
| `facts/` | the library facts shipped inside the compiler (`std`, Eigen, `span`) |
| `tests/` | the safety fixtures and the fact, scan, separate-compilation and debugging tests |
| `tools/` | `cling-scan` (fact scanners), the fact gauge and probes |

## License

MIT, see [LICENSE](LICENSE).
