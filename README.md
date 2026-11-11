# CeeLing

![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg) ![Status: pre-1.0](https://img.shields.io/badge/status-pre--1.0-orange.svg)

**A Rust-inspired, memory-safe dialect of C++ that compiles to plain C++17.**

CeeLing makes existing C++ code safer without leaving the C++ ecosystem. You can continue using your current C++ libraries while gaining Rust-inspired guarantees: dangling references and use-after-move are rejected at compile time. Its Java-like object model eliminates manual pointer and move syntax—there is no *, &, delete, std::move, or lifetime annotation. The compiler infers ownership and inserts moves automatically. The generated code is standard C++17 and can be built with an ordinary C++ toolchain, without CeeLing.

> **Status: pre-1.0.** Works on Windows (MinGW-w64) and Linux (Ubuntu 22.04, g++ 11); no release packages yet, build from source. The language is small on purpose: code outside it is rejected, not guessed at. It does **not** give Rust's full guarantees; the table below states the boundary.

## What it guarantees, and what it does not

| CeeLing guarantees (in the code it checked) | CeeLing does not guarantee |
|---|---|
| No dangling references and no use after move, rejected at compile time | Rust-level safety in general, or full data-race freedom (only the `spawn` / `Channel` model is checked) |
| Ownership and smart-pointer choice inferred; no raw pointers, `delete` or casts | Anything about `ext_fn` shims and the C++ libraries you link: they are trusted boundaries (`cling --report-facts` lists what a program trusted) |
| Out-of-bounds `v[i]` and null object handles abort with the `.clg` line | Logic errors, resource limits (stack depth, CPU time) or algorithmic complexity |
| Output is plain C++17 that calls C++ libraries directly | Stable syntax or API: it is pre-1.0 |

The contract, the table of threats and what is still open are in [guide chapter 13](docs/language-guide/13-contract-and-threats.md) and [chapter 18](docs/language-guide/18-status.md).

## Quick start

Requirements: CMake 3.16+, a C++17 compiler (g++, clang++ or MinGW-w64) and Git. To run the tests you also need Python 3 and bash. No releases are published; CeeLing is built from source.

**Linux / macOS**

```bash
git clone --recursive https://github.com/AI4Engr/CeeLing && cd CeeLing      # vendor/tree-sitter is a submodule
cmake -S . -B build && cmake --build build --parallel
./build/cling examples/hello.clg --cpp hello.cpp
g++ -std=c++17 hello.cpp -o hello && ./hello
```

**Windows (PowerShell, MinGW-w64)**

```powershell
git clone --recursive https://github.com/AI4Engr/CeeLing; cd CeeLing
cmake -S . -B build -G Ninja; cmake --build build --parallel      # or -G "MinGW Makefiles"
.\build\cling.exe examples\hello.clg --cpp hello.cpp
g++ -std=c++17 hello.cpp -o hello; .\hello.exe
```

`examples/hello.clg`:

```
class Counter {
public:
    int total;
    Counter() { total = 0; }
    void add(int n) { total = total + n; }
};

int main() {
    std::vector<int> scores;
    scores.push_back(3);
    scores.push_back(5);

    Counter c = new Counter();
    int i = 0;
    while (i < scores.size()) {
        c.add(scores[i]);
        i = i + 1;
    }
    std::cout << "total = " << c.total << std::endl;
    return 0;
}
```

Output: `total = 8`. Run the whole test suite with `ctest --test-dir build`. Verified: on Windows with MinGW-w64 a clean build passes all 394 safety fixtures and the guide's 565 examples; on Ubuntu 22.04 with g++ 11 a clean clone built, `hello.clg` ran and the 385 fixtures then existing passed (not repeated since). Two guide examples quote the diagnostics of g++ 16 and differ on older compilers. macOS and MSVC have not been tried.

## What the compiler rejects

**A dangling reference** (`ref` observes, it does not own):

```
Node owner = new Node(1);
ref Node view = owner;
owner = new Node(2);            // the first object is gone
std::cout << view.key;
```
```
error[semantic/use-after-move]: use of dangling observer `view` (its owner `owner` is gone)
  help: `view` observes `owner`, which no longer has a live owner
```

**A view that outlives what it looks into** (`std::span` and `string_view` die with the vector or string):

```
std::vector<int> v;
std::span<int> s = v;
v.push_back(2);                 // may reallocate
std::cout << s.size();
```
```
error[semantic/use-after-move]: use of dangling observer `s` (its owner `v` is gone)
```

**A use after move** (passing an object or container hands it over; the compiler inserts the move):

```
std::vector<int> w = consume(v);
std::cout << v.size();
```
```
error[semantic/use-after-move]: use of moved-from value `v`
  help: `v` was moved earlier; assign a new value before using it
```

**What cannot be proved is checked at run time.** Every `v[i]` and `s[i]` is bounds-checked and every object-handle dereference is null-checked; a failure prints the `.clg` line and aborts:

```
cling: index out of bounds at line 5 (index 3, size 1)
```

Both checks can be switched off (`--no-bounds-checks`, `--no-null-checks`).

## Ownership is inferred

`list.clg`:

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

`list.cpp` (generated, abridged):

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

Several owners become `shared_ptr`; a back link that closes a cycle becomes `weak_ptr` ([guide chapter 4](docs/language-guide/04-ownership.md)). Threads share nothing: `spawn` takes only plain values or an object moved in, and `Channel<T>` is the one shared queue ([chapter 10](docs/language-guide/10-threads.md)).

## Relation to C++ and Rust

CeeLing is C++ with a restricted, checked surface, not a new runtime: it compiles to C++ and links to existing C++ libraries as they are. It covers several of the mechanisms that make Rust's safe subset safe, in a weaker form (inferred instead of declared with lifetimes, partly checked at run time instead of compile time), and leaves others out on purpose. The feature-by-feature comparison is in [guide chapter 17](docs/language-guide/17-comparisons.md#feature-by-feature-rusts-safe-subset-and-ceeling). Where the strongest guarantee is required, use Rust. The objections that come up most (is it a C replacement, is an inferred `shared_ptr` just garbage collection, why not sanitizers and hardened libraries) are answered in [the same chapter](docs/language-guide/17-comparisons.md#questions-people-ask).

## Evidence: libraries written in CeeLing

These are libraries written for their own sake, each in its own repository. [Chapter 18](docs/language-guide/18-status.md) says what each one shows and what it does not.

| Project | What it is |
|---|---|
| **[cJSON-cling](https://github.com/AI4Engr/cJSON-cling)** | All of cJSON 1.7.19 and its Utils: 2,907 lines of CeeLing and a 52-line number shim |
| **zlib-cling** | DEFLATE and the zlib format, about 3,400 lines of CeeLing (about 2,000 without comments) and a 248-line shim; verified in both directions against real zlib on a 15.7 MB corpus |
| **yaml-cling** | A YAML library with the yaml-cpp API, used by a plain C++ project through generated headers |
| **app-cling** | A desktop application shell whose commands are written in CeeLing; exercises the threading model under sanitizers |

**cJSON's 33 published issues.** Each issue published against cJSON 1.7.19 was reproduced in C and examined in the port (these are numbered issues, not CVE identifiers):

| Outcome in the port | Issues | Count |
|---|---|---|
| Cannot happen: the unsafe operation cannot be written | 1, 2, 3, 4, 7, 12, 13, 30, 32 | 9 |
| Stopped at run time: aborts with the source line | 5, 11 | 2 |
| Partly: one route closed, another still open | 6, 8, 9 | 3 |
| Not prevented: CPU exhaustion | 10 | 1 |
| Avoided by the library's own discipline, not by the language | 31 | 1 |
| Same bug, same behaviour: a logic error, memory-safe in C too | 14 to 29, 33 | 17 |

The memory errors (1 to 13) are mostly removed or turned into a located abort; the 17 logic errors remain, because a language that checks memory cannot know that `~1` must decode to `/`. The defective C, a C program that triggers each one, the port's code and the full 33-row table are in [chapter 15](docs/language-guide/15-case-study-cjson.md), with the speed and memory figures.

**Tests.** 394 safety fixtures that must be rejected, or accepted, compiled and run with their output asserted; 565 guide examples checked by the test suite; seeded dangling programs for every scanned `std::vector` and `string` method, with negative controls that must fail.

## Using C++ libraries

**From C++ into CeeLing code.** The output is a normal `.cpp` and `.h`, so a C++ project links it like any other library and needs no CeeLing at build time:

```
cling stats.clg --cpp stats.cpp          # implementation
cling stats.clg --cpp stats.h            # header
g++ -std=c++17 main.cpp stats.cpp -o app # main.cpp does #include "stats.h" and calls total(v)
```

**From CeeLing into C++ libraries.** What the compiler assumes about a library call is data, not code: plain-text *fact packs* say which methods borrow, which invalidate a container's views and which return fresh values. The facts for `std::vector`, `string`, `string_view`, `optional` and `map` are generated from the real standard library declarations. For your own library, `tools/cling-scan` drafts a pack from a header, `--report-facts` lists every fact a program relied on and `--strict` refuses facts no person reviewed. An unlisted method is assumed to borrow its receiver, so the default is the cautious one.

**Example: Eigen.** A block expression refers to the storage of the matrix it came from. With the real Eigen headers and a two-line pack for `Eigen::VectorXd`:

```
Eigen::VectorXd v = Eigen::VectorXd::Ones(4);
Eigen::VectorXd head = v.head(2);        // refers to v's storage
v = Eigen::VectorXd::Ones(8);            // v is replaced
std::cout << head.size();
```
```
error[semantic/use-after-move]: use of dangling observer `head` (its owner `v` is gone)
  help: `head` observes `v`, which no longer has a live owner
```

The pack for `VectorXd` was scanned from the Eigen headers (69 facts, bound to the headers by SHA-256) and checked against the real library under AddressSanitizer. Expression templates held through `auto` cannot be written at all: `auto` is a compile error. Eigen, jsoncpp and yaml-cpp are the libraries the scanner has been tried on; the facts are a scanner's draft until a person reviews them. See [chapter 11](docs/language-guide/11-talking-to-cpp.md), [chapter 12](docs/language-guide/12-many-files.md) and the [pack format](docs/internals/facts-format.md).

## Who it is for

**A good fit:** teams that already write C++ and want fewer memory errors in new code, libraries that must stay usable from plain C++, and anyone who wants to see how far inference can go without lifetime annotations.

**Not a fit (yet):** projects that need Rust's full guarantees, a stable 1.0 language, or to move a large existing C++ code base over unchanged. CeeLing is for new code that sits beside existing C++, not a converter for it, and not a smaller language to replace C: it adds checking to C++, it does not shrink it.

## Documentation

* **[Language Guide](docs/language-guide/README.md)**: 18 chapters, every example checked by the test suite. Start with [why CeeLing](docs/language-guide/01-why-ceeling.md) and [the language](docs/language-guide/02-language.md).
* [Safety contract and threats](docs/language-guide/13-contract-and-threats.md), [how the compiler works](docs/language-guide/14-how-it-works.md), [status](docs/language-guide/18-status.md).
* [All documentation](docs/README.md), including the [design records](docs/design/): Eigen views, inferring the facts of third-party libraries.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md): building, running the tests, the rule that every change to what the compiler accepts comes with a fixture, and the source layout.

## License

MIT, see [LICENSE](LICENSE).
