# 11. Talking to C++

CeeLing is useful only if it can use the C++ you already have, and if C++ can use what it produces. There are three doors, and each is a trust decision:

| Door | Direction | What the compiler knows about what is behind it |
|---|---|---|
| `ext_fn` | CeeLing calls C++ you wrote | only the signature you declared, and it believes it |
| the library allow-list | CeeLing calls `std` (and a little of Eigen) | facts shipped with the compiler (`facts/*.clfacts`): which calls borrow, move and invalidate |
| the generated C++ | C++ calls CeeLing | nothing: it is ordinary C++17, and the caller is not checked |

The chapter ends with the list of what the guarantee does not cover once C++ is involved. [Chapter 8](08-sealed-escape-hatches.md) shows `ext_fn` as an escape hatch in a few lines; this chapter is the whole story.

## The problem: a safe dialect still calls unsafe code

A CeeLing program that cannot touch a raw pointer will sooner or later need `sqrt`, a window, a socket, a SIMD loop. Those are written in C++, and C++ can do anything with its arguments, including keeping their addresses:

<!-- example: 11_stash_pointer.cpp -->
```cpp
// The same mistake without CeeLing: nothing in C++ says that remember() keeps the address it was given.
// (Not run by the checker: it reads a destroyed vector.)
#include <vector>

static const std::vector<int>* kept = nullptr;

int remember(const std::vector<int>& data) { kept = &data; return 0; }
int firstRemembered() { return (*kept)[0]; }

void fill() {
    std::vector<int> v;
    v.push_back(7);
    remember(v);
}                                    // v is destroyed here

int main() {
    fill();
    return firstRemembered();        // undefined behaviour
}
```

**Why C++ does not stop it.** `remember` takes a `const&`, which promises not to modify the vector. It says nothing about how long the address may be kept. A reference carries no lifetime, so the call site cannot tell
a function that reads its argument from one that stores it. `g++ -Wall -Wextra` is silent.

A language that wants to call such code has three choices: forbid it (useless), allow every call (the guarantee is void), or admit a **declared** door and say plainly that the door is trusted. CeeLing takes the third. The
door is `ext_fn`. The comparison is Rust's `unsafe extern` block, with one difference: here the *caller* writes the declaration, so no library has to be redesigned.

## `ext_fn`: a signature, and a promise

```
ext_fn ReturnType name(params) [borrows N];
```

A pure declaration, ending in `;`. CeeLing emits a forward declaration and treats the name as callable. The body is C++ you write and link yourself.

<!-- example: 11_ext_fn_sqrt.clg -->
```cpp
ext_fn double mySqrt(double x);       // a signature, no body: the body is C++ linked in

int main() {
    double y = mySqrt(2.0);
    std::cout << y << std::endl;
    return 0;
}
```
<!-- output: 11_ext_fn_sqrt.clg -->
```
1.41421
```
<!-- generated: 11_ext_fn_sqrt.clg "line: double mySqrt(double x);" -->
```cpp
double mySqrt(double x);
```

The C++ side is an ordinary file; CeeLing never reads it:

<!-- example: 11_ext_fn_sqrt_shim.cpp -->
```cpp
// The C++ side, written by hand and linked in. CeeLing never reads this file: it believes the declaration.
#include <cmath>

double mySqrt(double x) { return std::sqrt(x); }
```

You build it the way you build any C++ project: transpile every `.clg`, then compile and link all the `.cpp` together (`cling main.clg --cpp main.cpp`, `g++ main.cpp shim.cpp -o program`). A real
project has many modules and one shim layer; app-cling's `scripts/build.sh` does exactly this, transpiling each module to its own `.cpp` and `.h` and linking them with the C++ in `shim/`.

Without the declaration the call is refused, because CeeLing has no fact about it. `std::sqrt` is C++'s own, and it is refused the same way:

<!-- example: 11_unknown_std_call.clg -->
```cpp
int main() {
    double r = std::sqrt(4.0);        // C++ allows it; CeeLing has no fact about it
    return 0;
}
```
<!-- error: 11_unknown_std_call.clg -->
```
error[semantic/forbidden-construct]: call to unknown function/method `std::sqrt`
  --> 11_unknown_std_call.clg:2:16
  help: CeeLing has no borrow/ownership summary for anything it did not compile itself; unknown calls are rejected by default rather than silently trusted (the default-reject rule). Define `std::sqrt` in CeeLing source, or call an existing CeeLing function/method instead.
```

One declaration trusts one name, not its neighbours:

<!-- example: 11_ext_fn_other_rejected.clg -->
```cpp
ext_fn double mySqrt(double x);

int main() {
    double y = myCube(4.0);          // one declaration trusts one name, not the neighbours
    return 0;
}
```
<!-- error: 11_ext_fn_other_rejected.clg -->
```
error[semantic/forbidden-construct]: call to unknown function/method `myCube`
  --> 11_ext_fn_other_rejected.clg:4:16
  help: CeeLing has no borrow/ownership summary for anything it did not compile itself; unknown calls are rejected by default rather than silently trusted (the default-reject rule). Define `myCube` in CeeLing source, or call an existing CeeLing function/method instead.
```

### A header as the shim

Most shims are a header the `.clg` file includes. A line `#include "x.h"` that is not a `.clh` is copied into the generated C++ unread, so the declaration CeeLing sees and the definition g++ sees meet in the output.
This is the pattern cJSON-cling and zlib-cling use (`#include "../shim/cjson_num.h"` above `ext_fn double cjson_strtod_at(...)`).

<!-- source: CeeLing/docs/language-guide/examples/11_clamp_shim.h "inline int clampTo" -->
```
inline int clampTo(int x, int lo, int hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}
```
<!-- example: 11_ext_fn_header.clg -->
```cpp
#include "11_clamp_shim.h"
ext_fn int clampTo(int x, int lo, int hi);

int main() {
    std::cout << clampTo(99, 0, 10) << " " << clampTo(-4, 0, 10) << " " << clampTo(5, 0, 10) << std::endl;
    return 0;
}
```
<!-- output: 11_ext_fn_header.clg -->
```
10 0 5
```

### What may cross

Parameters and the return type must be a primitive (`int`, `int64`, `float`, `double`, `bool`, `char`, `byte`, `string`) or a `std::vector` of one. Never a class, a `std::map` or `void`. The reason is the one the whole
language rests on: a number or a string has no owner and no lifetime, so the checker needs no fact about it. An object would need a statement of who owns it afterwards and what the callee may keep, and nobody has designed a
way to write that. Today the answer is "no":

<!-- example: 11_ext_fn_class_rejected.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

ext_fn int inspect(Node n);       // a class parameter would need a borrow summary nobody can write yet

int main() {
    return 0;
}
```
<!-- error: 11_ext_fn_class_rejected.clg -->
```
Error: error[semantic/forbidden-construct]: ext_fn parameter `n` type `Node` is not supported (line 7, column 1). ext_fn signatures are restricted to primitive types (int, float, double, bool, string, char) and std::vector<primitive> for now -- a class-typed ext_fn parameter or return needs a hand-written borrow-summary annotation that isn't designed yet (see guide chapter 11).
```

<!-- example: 11_ext_fn_void_rejected.clg -->
```cpp
ext_fn void noteIt(int x);        // return something: an int status is the usual shape

int main() {
    noteIt(1);
    return 0;
}
```
<!-- error: 11_ext_fn_void_rejected.clg -->
```
Error: error[semantic/forbidden-construct]: ext_fn return type `void` is not supported (line 1, column 1). ext_fn signatures are restricted to primitive types (int, float, double, bool, string, char) and std::vector<primitive> for now -- a class-typed ext_fn parameter or return needs a hand-written borrow-summary annotation that isn't designed yet (see guide chapter 11).
```

(A `Channel` is refused in an `ext_fn` signature too: [chapter 10](10-threads.md).) Return an `int` status and put any other result in a `ref` parameter.

### `ref` parameters: borrowed, not copied

`ref` is rejected in every parameter list except an `ext_fn`'s. There the type decides what it means, and the two meanings never overlap:

| Written | Emitted C++ | Meaning |
|---|---|---|
| `ref std::vector<int> a` | `const std::vector<int>& a` | read-only borrow for the call |
| `ref int n`, `ref string s` | `int& n`, `std::string& s` | an out-parameter: one function returns two values |
| `ref ClassName x` | rejected | class types stay outside `ext_fn`: the callee could keep the object |
| a mutable container reference | cannot be written | the callee could reallocate it and invalidate every observer |

A writable primitive is safe because an `int` has no lifetime: writing through it cannot dangle.

<!-- example: 11_ext_fn_ref.clg -->
```cpp
ext_fn int sumAll(ref std::vector<int> data);        // read-only borrow: const std::vector<int>&
ext_fn int splitTens(int value, ref int rest);       // out-parameter: int&
ext_fn int fillText(ref string out);                 // out-parameter: std::string&

int main() {
    std::vector<int> v;
    v.push_back(1);
    v.push_back(2);
    v.push_back(3);
    int total = sumAll(v);
    int rest = 0;
    int tens = splitTens(35, rest);
    string text = "";
    int ignored = fillText(text);
    std::cout << total << " " << tens << " " << rest << " " << text << " " << v.size() << std::endl;
    return 0;
}
```
<!-- output: 11_ext_fn_ref.clg -->
```
6 3 5 filled by C++ 3
```
<!-- example: 11_ext_fn_ref_shim.cpp -->
```cpp
#include <string>
#include <vector>

int sumAll(const std::vector<int>& data) {
    int total = 0;
    for (int x : data) total += x;
    return total;
}

int splitTens(int value, int& rest) {
    rest = value % 10;
    return value / 10;
}

int fillText(std::string& out) {
    out = "filled by C++";
    return 0;
}
```

**Fixed 2026-11-10: a local vector passed to a `ref` parameter is not moved.** The call `sumAll(v)` is emitted as `sumAll(v)`, and the example prints `v.size()` after it (3). Until then it was emitted as `sumAll(std::move(v))`, which compiled and gave the right answer for the call, but left the local `v` in the standard's "valid but unspecified" state while the checker still treated it as usable, so a later read of `v` compiled and gave nothing reliable. The emitter now knows which parameters of an `ext_fn` are `ref` and passes those arguments as they are; the checker already did not mark them moved.

### A container is moved in by default

Without `ref`, a vector argument is handed over like any call argument, and the usual rule applies:

<!-- example: 11_ext_fn_move_rejected.clg -->
```cpp
ext_fn int total(std::vector<int> data);     // no `ref`: the vector is handed over, like any call argument

int main() {
    std::vector<int> a;
    a.push_back(1);
    int t = total(a);
    a.push_back(2);                          // a was moved into total()
    return 0;
}
```
<!-- error: 11_ext_fn_move_rejected.clg -->
```
error[semantic/use-after-move]: use of moved-from value `a`
  --> 11_ext_fn_move_rejected.clg:7:5
  help: `a` was moved earlier; assign a new value before using it
```

### `borrows N`: the one fact you can write

If the result aliases an argument, say so. The clause feeds the same table that the compiler fills for its own functions from their bodies, so the result is tracked like any other observer:

<!-- example: 11_ext_fn_borrows_rejected.clg -->
```cpp
ext_fn std::vector<int> sliceOf(std::vector<int> data, int start) borrows 0;   // the result aliases parameter 0

int main() {
    std::vector<int> a;
    a.push_back(1);
    std::vector<int> b = sliceOf(a, 0);
    a.push_back(2);                          // a is moved again: b's owner is gone
    int x = b[0];
    return 0;
}
```
<!-- error: 11_ext_fn_borrows_rejected.clg -->
```
error[semantic/use-after-move]: use of moved-from value `a`
  --> 11_ext_fn_borrows_rejected.clg:7:5
  help: `a` was moved earlier; assign a new value before using it
error[semantic/use-after-move]: use of dangling observer `b` (its owner `a` is gone)
  --> 11_ext_fn_borrows_rejected.clg:8:13
  help: `b` observes `a`, which no longer has a live owner
```

### The caveat, stated as plainly as the rest

The sealed hatches of chapter 8 have no syntax to misuse. `ext_fn` is different in kind: it **disables the default-reject gate for that name**. A wrong declaration (the arity, the types, or a body that does
something its signature does not say) is believed and silently emitted. This program is accepted, links, and reads a destroyed vector, because the C++ keeps the address and the declaration says "a read-only borrow":

<!-- example: 11_ext_fn_wrong_declaration.clg -->
```cpp
// Compiled and linked, not run: the program reads a destroyed vector.
ext_fn int remember(ref std::vector<int> data);     // says "a read-only borrow for the call"; the C++ keeps the address
ext_fn int firstRemembered();

void fill() {
    std::vector<int> v;
    v.push_back(7);
    int ok = remember(v);
}

int main() {
    fill();
    int x = firstRemembered();      // the checker believes the declaration, so it says nothing
    return 0;
}
```
<!-- example: 11_stash_shim.cpp -->
```cpp
// The hand-written side. Its signature below hides that it keeps the address.
#include <vector>

static const std::vector<int>* kept = nullptr;

int remember(const std::vector<int>& data) { kept = &data; return 0; }
int firstRemembered() { return (*kept)[0]; }
```

It is not run by the checker, only compiled and linked. This is the same hole `unsafe` is in Rust, and just as real; the difference is the size of the list. In app-cling the checked code reaches the operating system
only through its `ext_fn` declarations (the head of each `.clg` file, with `shim/app_native.h` the C++ side), so the trusted surface is a list you can read, not the whole program.

## A library nobody wrote facts for: `--facts` and `opaque`

A project can teach the compiler about its own C++ library without touching the compiler: a small text file of facts, passed as `cling --facts mylib.clfacts` (repeatable). `class Grid` makes `Grid` a known type;
`fresh Grid::width` says a method returns a plain value and leaves everything alone; `borrow Grid::window` says its result refers to the receiver. A method nobody listed is refused, as ever.

When even that is too much work, `opaque Grid` declares the class and assumes the worst about every method without a fact: its result borrows the receiver, calling it may change the receiver, and an object
handed to it may be kept. Nothing needs to be listed for a first program to compile, and anything that could dangle is rejected rather than guessed at; writing `fresh` and `borrow` facts for the methods you use
removes the rejections one by one. `cling --report-facts` lists every fact a compile relied on, so you can see exactly what you are trusting. Format and rules: `docs/internals/facts-format.md`; worked examples
(a private class, and a real `Eigen::VectorXd` with a two-line pack) run in CTest as `cling_project_facts`. The guide's own example checker cannot pass `--facts`, so there is no checked example here.

An `opaque` or listed fact is the project's claim about its library. CeeLing never looks inside the library: a wrong claim voids the guarantee for calls that rely on it, like a wrong `ext_fn`.

## What the compiler knows about `std`, and about Eigen

Every call to something CeeLing did not compile itself needs a fact, or it is rejected. The facts ship inside the compiler as plain-text packs (`facts/*.clfacts`, format in `docs/internals/facts-format.md`):

- **`std::vector` and `string`:** no list of names. The packs hold what a scan of the library's own declarations found for each method, and a rule admits a method when what it hands back is something the checker can
  follow (chapter 3 has the rule and today's result). A method that changes the receiver invalidates every observer of it; that is read from the scan too. Hand-written: `push_back` links its argument instead of consuming
  it, and CeeLing's own `copyWithin`.
- **`std::map`** with primitive keys and values.
- **free functions:** `std::stoi`, `std::stod`, `std::to_string`.
- **runtime helpers** the compiler emits into your file: `cling::readFileToString`, `fileExists`, `readFileToBytes`, and the thread helpers of chapter 10.

<!-- example: 11_std_allowlist.clg -->
```cpp
int main() {
    std::vector<int> v;
    v.push_back(1);
    v.push_back(2);
    v.push_back(3);
    v.reserve(10);
    string s = "hello world";
    string head = s.substr(0, 5);
    int at = s.find("llo");
    int n = std::stoi("42");
    double d = std::stod("2.5");
    std::cout << v.size() << " " << head << " " << at << " " << n << " " << d << " n=" << std::to_string(v.size() + 5) << std::endl;
    return 0;
}
```
<!-- output: 11_std_allowlist.clg -->
```
3 hello 2 42 2.5 n=8
```

Everything else is the "unknown call" error above. That is the default-reject rule of [chapter 8](08-sealed-escape-hatches.md): it closes the door through which a dangling reference could otherwise slip past the checker
unseen. The facts live in `facts/*.clfacts`, shipped inside the compiler, and a project adds its own with `--facts` (above); `ext_fn` is the way to declare one of your own functions inside the program. (A few names from an earlier graphics
experiment, `gfx_init` and its relatives, used to be built in; imgui-cling now keeps them in its own `shim/gfx_shim.clfacts`.)

**Eigen** is the one third-party library with facts in the table: `Eigen::MatrixXd`, the factory `Identity`, `operator*`, and the view-returning methods `block`, `row`, `col`, `transpose`, `segment`, `head`, `tail`,
`diagonal`, which are treated as borrowing their receiver. That is [chapter 6](06-dangling-references.md) (a `block` that dangles is rejected; an unlisted method such as `topRows` is refused). The machine this guide
was checked on has no Eigen installed, so no Eigen program is compiled here; only the rejections, which need no Eigen headers, are shown there.

**Not built:** vendor-maintained packs published for download. A library other than the ones above is reached with a project pack you write or scan (`--facts`, `opaque`; the sections above), or through `ext_fn` shims over primitives. What is open is listed at the end of "Who vouches for a fact".

## Who vouches for a fact

A fact is a claim about what a library call does with ownership: its result is fresh, or it borrows the receiver; calling it invalidates every observer of the receiver, or it does not; an object handed to it may be kept. The dangling checker uses a fact for a library call exactly as it uses what it derived itself for a CeeLing function, so nothing in the analysis depends on where a fact came from. What differs is how far you can trust it, and the compiler keeps that visible. Every fact carries a `source` and a review state:

| State | Who vouches | Set by |
|---|---|---|
| `declared`, `review=hand` | a person wrote the fact | by hand |
| `review=reviewed:<name>:<date>` | a named person read it | `tools/cling-scan/review_pack.py --reviewer <name>`; nothing else sets it |
| `review=probed:<date>` | a probe ran the fact against the real library under AddressSanitizer and found no contradiction (evidence, not review) | `tools/fact-gauge/probe_pack.py --mark-probed`, on a passing run, for the facts it exercised |
| `inferred`, `review=unreviewed` | nobody: a scanner read the declaration | `tools/cling-scan` |
| `opaque Name` | nobody wrote any fact; the worst is assumed | you, in one line |

Three flags use this. `cling --report-facts` lists every fact a compile relied on, with its pack and source (the trusted-boundary report). `cling --strict` refuses to compile, with exit status 3 and no output, a program that relied on an inferred fact of a `--facts` pack that no person reviewed; `--strict=probed` also accepts `probed:` facts; `--strict-all` audits the facts shipped inside the compiler too, and fails today, because the scanned `std::vector`, `string` and `map` facts are inferred and nobody has reviewed them line by line. `opaque` always passes, because it can only make checking stricter.

Two rules hold for every source. **An error may only go in the cautious direction:** a scanner may leave a method opaque, or write a fact that is too careful (a safe program is rejected and you add a declaration); it writes "fresh" (borrows nothing) only when the declaration proves it. And **a fact cannot open a sealed construct:** raw pointers, `.get()`, casts and unions are rejected by the escape-hatch checker whatever a pack says. A pack is data: loading one runs nothing and reads no other file, and a project pack can only add facts, never relax how `std` is judged or replace a fact the compiler ships.

### Writing a pack by scanning: `cling-scan`

```text
python -I tools/cling-scan/cling_scan.py --clang clang++ --header grid.h --class Grid --out grid.clfacts
python -I tools/cling-scan/cling_scan.py --clang clang++ --header Eigen/Dense --type Eigen::VectorXd -I eigen --out vx.clfacts
cling --facts grid.clfacts program.clg
```

The scanner reads the **declared signature** of every public method, never the body, and writes a fact only when the signature proves it. A `const` method that returns a `void`, a number, a `bool`, an enum, a `std::string` or a `std::vector` of those is `fresh`: nothing it returns can refer to the receiver. A `const` method that returns a reference, a pointer, a view or an iterator is a `borrow`. A non-`const` method that returns nothing that borrows is a `mutator` (its observers die). A `static` method taking only plain values is a `function`. It writes `opaque Grid` first, so every method it could not decide keeps the cautious default, and it leaves a `#` comment with the reason for each (a class returned by value, overloads that disagree, member templates, operators). Every fact it writes is `inferred` and `unreviewed`.

What it cannot see is what a signature does not say: it takes `const` at its word (a `mutable` member or a pointer the class keeps can make a `const` method not fresh), and "returns a number" is the declared type, not what the number means. A confidently wrong fact is possible, and is the same kind of hole as a wrong `ext_fn`. Three things narrow it:

- **Header hashes.** The pack records the SHA-256 of every non-system header the translation unit included (265 for `Eigen/Dense`), and the compiler refuses a pack whose headers changed since the scan (exit status 2, naming the pack and the header). A header it cannot find is a warning, or an error with `--strict-headers`. A library upgrade cannot be paired silently with last year's facts.
- **Witness types.** A template's facts depend on its arguments. `--witness OtherType` scans a second instantiation and keeps only the facts on which both agree; a method that differs falls back to the cautious default.
- **Three backends, one analysis.** A text backend (over `clang -ast-dump`), a libclang backend (the stable C API; classes, not template instantiations) and a LibTooling program on clang's AST. They are tested to give the same verdict for every method on the same classes.

### Checking the facts: the gauge and the probes

Two tools ask whether facts and checker agree, and whether the facts are true.

`tools/fact-gauge/gauge.py` (CTest `cling_fact_gauge`) makes three independent checks. For every scanned `std::vector` and `std::map` method it generates a program that would dangle if the fact "this method changes the receiver" were ignored (an observer, the call, a use of the observer) and lets the compiler judge it: a **false accept** must be zero, a false reject must be zero. It calls every admitted method validly and compiles the result with `g++ -std=c++17`. And it checks the facts against a hand transcription of the standard's own list of calls that invalidate references. The last is the one that matters: the first two only show that the compiler follows the facts, and the third is what found `reserve` missing from an earlier hand table. Two negative controls (the gauge told a wrong fact on purpose) must fail.

`tools/fact-gauge/probe_pack.py` (CTest `cling_scan_probe`, the slowest test of the suite) checks a scanned pack against the **real library**: per fact, the compiler judges a seeded program, and a plain C++ twin holding a lazy view runs under AddressSanitizer. Results: `Eigen::VectorXd`, 56 facts, no contradiction (54 `fresh` and `borrow` facts run clean; the two mutators that really reallocate, `resize` and `conservativeResize`, are confirmed by `heap-use-after-free`); jsoncpp `Json::Value`, 41 facts, none contradicted (`clear` really frees the node a reference points into); yaml-cpp `YAML::Node`, 13 facts. For each library, declaring a mutator `fresh` makes the probe fail with a `heap-use-after-free` from the real library, and that is the point of the control: a probe that cannot fail proves nothing. The probe also found a bug in the scanner itself (it had read past the end of a class and credited jsoncpp's `Value` with the methods of its nested `CZString`); it is fixed, with a regression test.

What these show, and what they do not: a clean run is evidence, not proof, for one object, one or two views and arguments a script chose. A probe sees storage going away or moving, not a value changing under a view. And none of it makes the library's own code checked: a vendor pack moves a library from "trusted" to "declared", and the guarantee stays "no dangling references in code CeeLing checked, excluding what trusted libraries do with what they are given".

### The standard library's facts are generated

The facts for `std::vector`, `string`, `string_view`, `optional` and `map` are not a list of names. `tools/std-scan-experiment/gen_pack.py` writes them from a scan of the real declarations (the MSVC STL and libstdc++, each reading the other's gaps), and the CTest `cling_facts_generated` fails if the committed pack is not exactly what the script produces. The compiler then **computes** admission: a method is admitted when every overload CeeLing can write returns something the checker can follow (`void`, a number, a fresh `string`, `*this`) and takes no reference it could write through. The ones refused say why: iterators (`begin end insert erase`), element references (`front back at`), raw pointers (`data c_str`). A method that changes the receiver invalidates every observer of it, derived as "a non-const member with no const twin", and checked by the gauge against the standard.

**Not built** (the design, and the reasoning for each, is in [docs/design/third-party-inference.md](../design/third-party-inference.md)): facts derived from a library's own bodies (whether a call stores an argument, whether it reallocates); buffer-size contracts for C-style numerical routines; a scan driven by the names a program actually uses; downloadable, locked packs (`cling fetch-facts`); and a release gate that rescans the standard library against newer toolchains. Until the first of these exists, `opaque` is deliberately blunt about a library's methods, and a call it cannot judge is refused rather than guessed.

## The generated C++ is an ordinary library

CeeLing's output is plain C++17 that includes only the standard library and carries its few runtime helpers (`cling::nn`, `cling::at`, the thread pool) inside the file. A C++ project that uses a library written in
CeeLing therefore needs no CeeLing: not to compile, not to link, not to read. The header comes from the same compiler: `--cpp x.h` writes declarations only, `--cpp x.cpp` writes the definitions.

<!-- example: 11_lib_account.clg -->
```cpp
namespace bank {
    class Account {
    public:
        string owner;
        int balance;
        std::vector<int> history;
        Account(string who, int start) {
            owner = who;
            balance = start;
        }
        void deposit(int amount) {
            balance = balance + amount;
            history.push_back(amount);
        }
        int lastDeposit() {
            return history[history.size() - 1];
        }
    };

    int fee(int amount) {
        return amount / 100;
    }
}
```
<!-- header: 11_lib_account.clg "class Account {" -->
```cpp
class Account {
public:
    std::string owner;
    int balance{};
    std::vector<int> history;
    Account(std::string who, int start);
    void deposit(int amount);
    int lastDeposit();
};
```

A `namespace` becomes a C++ namespace; `string` becomes `std::string`; a field of class type becomes a smart pointer. A C++ program uses it with nothing but `g++`:

<!-- example: 11_lib_account_consumer.cpp -->
```cpp
// An ordinary C++ program. It includes the generated header and links the generated .cpp; CeeLing is not involved.
#include "11_lib_account.h"
#include <iostream>

int main() {
    auto a = std::make_unique<bank::Account>("ann", 100);   // a CeeLing object is built behind a smart pointer
    a->deposit(50);
    std::cout << a->balance << " " << a->lastDeposit() << " " << bank::fee(250) << " ";
    bank::Account* alias = a.get();                         // C++ may do what CeeLing code may not
    alias->deposit(1500);
    std::cout << bank::fee(alias->balance) << std::endl;
    return 0;
}
```
<!-- output: 11_lib_account.clg -->
```
150 50 2 16
```

The build is `cling 11_lib_account.clg --cpp 11_lib_account.cpp`, `cling 11_lib_account.clg --cpp 11_lib_account.h`, then `g++ consumer.cpp 11_lib_account.cpp`.

**Where it stops.** The guarantee covers the library's own code. The caller is ordinary C++ and can do what CeeLing code may not: the consumer above calls `a.get()` and keeps a raw pointer, and nothing complains. A
class's constructors and methods keep CeeLing's conventions (an object parameter is moved in, there are no reference parameters), which can feel odd from C++; and the output is a snapshot, so do not edit it by
hand: the claim belongs to the `.clg` plus the compiler, not to the `.cpp`. More than one `.clg` file is [chapter 12](12-many-files.md).

## Flags that change the output

| Flag | Effect | Where shown |
|---|---|---|
| `--cpp FILE` | write C++ to `FILE`; a `.h` name writes the header | above |
| `--no-bounds-checks` | `v[i]` and `s[i]` are emitted bare, with no `cling::at` | below, [chapter 7](07-run-time-checks.md) |
| `--no-null-checks` | `->` is emitted bare, with no `cling::nn` | below, chapter 7 |
| `--pool-shared` | shared objects come from a per-thread free list instead of the general allocator (faster for node-heavy code) | below, [chapter 4](04-ownership.md) |
| `--ptr-demands-out F`, `--ptr-demands F` | separately compiled files agree on pointer kinds | [chapter 12](12-many-files.md) |
| `--legacy-cpp` | the older text-substitution generator, kept for comparison; the checks still run, but the guarantees are stated for the default generator only | not shown |

The default build checks both. The same program fails at its line, and with both checks off the access is emitted as bare C++:

<!-- example: 11_flags_default.clg -->
```cpp
int main() {
    std::vector<int> v;
    v.push_back(1);
    v.push_back(2);
    v.push_back(3);
    int n = v.size();
    std::cout << v[n] << std::endl;
    return 0;
}
```
<!-- output: 11_flags_default.clg -->
```
cling: index out of bounds at line 7 (index 3, size 3)
```

<!-- example: 11_flags_off.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
    int value() { return v; }
};

int main() {
    std::vector<int> v;
    v.push_back(1);
    int n = v.size() - 1;
    Node a = new Node(5);
    std::cout << v[n] << a.value() << std::endl;
    return 0;
}
```
<!-- generated: 11_flags_off.clg "int main()" -->
```cpp
int main() {
    std::vector<int> v;
    v.push_back(1);
    int n = (v.size() - 1);
    std::unique_ptr<Node> a = std::make_unique<Node>(5);
    std::cout << v[n] << a->value() << std::endl;
    return 0;
}
```

A claim about a program is a claim about how it was built: with a check off, that part of the contract is yours. `--pool-shared` changes the allocation of shared objects only:

<!-- example: 11_flags_pool.clg -->
```cpp
class Item {
public:
    int v;
    Item(int x) { v = x; }
};

int main() {
    Item a = new Item(1);
    Item b = a;                            // two owners: this object is shared
    std::cout << a.v << " " << b.v << std::endl;
    return 0;
}
```
<!-- generated: 11_flags_pool.clg "int main()" -->
```cpp
int main() {
    std::shared_ptr<Item> a = std::allocate_shared<Item>(cling::PoolAllocator<Item>(), 1);
    std::shared_ptr<Item> b = a;
    std::cout << cling::nn(a, "line 10")->v << " " << cling::nn(b, "line 10")->v << std::endl;
    return 0;
}
```

## Debugging: step through the `.clg`, not the generated C++

A debugger on the generated program shows C++: `std::unique_ptr` internals, `cling::nn(...)` calls, a `main` you did not write. The program you wrote is not the one you step through. The flag
`-g` (`--debug-lines`) fixes that the way every source-to-C++ translator does: it puts a `#line` directive before each statement it emits, so the C++ compiler's debug information (and its
error messages) name the **`.clg` file and line**, and `gdb` shows your source, stops at your lines and steps through them.

<!-- example: 11_debug_session.clg -->
```cpp
class Account {
public:
    int balance;
    Account(int start) { balance = start; }
    void deposit(int amount) {
        balance = balance + amount;
    }
};

int main() {
    Account account = new Account(100);
    account.deposit(50);
    std::cout << account.balance << std::endl;
    return 0;
}
```
<!-- generated: 11_debug_session.clg "int main()" -->
```cpp
int main() {
#line 11 "11_debug_session.clg"
    std::unique_ptr<Account> account = std::make_unique<Account>(100);
#line 12 "11_debug_session.clg"
    cling::nn(account, "line 12")->deposit(50);
#line 13 "11_debug_session.clg"
    std::cout << cling::nn(account, "line 13")->balance << std::endl;
#line 14 "11_debug_session.clg"
    return 0;
}
```

Compile the C++ with `-g` and without optimisation, from the directory that holds the `.clg` (gdb finds the source from the path written in the directive, which is the path you gave `cling`):

```
cling -g account.clg --cpp account.cpp
g++ -std=c++17 -g -O0 account.cpp -o account
gdb -x tools/cling.gdb account
```

`tools/clgdebug.sh FILE.clg` (PowerShell: `tools\clgdebug.ps1 FILE.clg`) does the three steps and starts gdb stopped at the first statement of `main` (try it on `examples/account.clg`). `tools/cling.gdb` is five lines: it makes `step` skip the library code the generated C++ calls (`std::`, `cling::`: smart-pointer operators, the bounds and null checks) and turns on pretty printing. A session on the example
above, run by the guide's checker (addresses removed):

<!-- gdb: 11_debug_session.clg -->
```
Breakpoint 1 at 0xADDR: file 11_debug_session.clg, line 12.

Thread 1 hit Breakpoint 1, main () at 11_debug_session.clg:12
12	    account.deposit(50);
Account::deposit (this=0xADDR, amount=50) at 11_debug_session.clg:6
6	        balance = balance + amount;
#0  Account::deposit (this=0xADDR, amount=50) at 11_debug_session.clg:6
#1  0xADDR in main () at 11_debug_session.clg:12
$1 = 50
$2 = 100
7	    }
$3 = 150
0xADDR in main () at 11_debug_session.clg:12
12	    account.deposit(50);
$4 = 150
```

What to know:

* **Names are the C++ names.** `break Account::deposit` works; `break account.clg:12` works; a field of the current object is visible as itself (`print balance`).
* **Objects are smart pointers underneath.** `print account` shows `std::unique_ptr` internals; ask for a field with `print account->balance`. `print *account` does not work, because `operator*` was never instantiated in the program.
* **The check helpers are stepped over.** In `-g` mode `cling::at` and `cling::nn` are marked `always_inline, artificial`, so `step` and `next` treat `v[i]` and `a.b` as part of their statement.
* **A constructor's field assignments have no line of their own.** They are hoisted into the C++ member-initialiser list; break on the first statement of the body, or on the line that calls `new`.
* **`else if`.** In `-g` mode it is emitted as `else { if ... }` so that the nested `if` can carry its own line. Behaviour is the same.
* **Optimisation moves lines.** Use `-O0`. The run-time checks themselves are unaffected by `-g`: an out-of-bounds index still prints its `.clg` line and aborts, and under gdb it stops at the `abort()` where `bt` shows the same line.
* **Several files.** Give every file `-g`; each directive names its own file. Code from an included module is debugged in that module's `.clg`.

## The trusted boundary, as a list

Once C++ is involved, the promise of [chapter 1](01-why-ceeling.md) reads: *no dangling references and no use after move in the CeeLing code that was checked, excluding what trusted code does with what it is
given.* The trusted parts are:

- **`ext_fn` declarations.** A wrong signature, or a body that does more than it says, is believed (above).
- **The bodies behind them,** and any other C++ in the build: not parsed, not checked, no bounds or null checks, free to start threads and share state ([chapter 10](10-threads.md)).
- **Libraries.** `std`, Eigen and anything else are trusted to do what the table says; their internals are never examined. A call the table does not list is refused, not trusted.
- **The C++ compiler** that builds the output, and hand edits to the output.
- **The callers of generated code.** They may hold raw pointers into a library written in CeeLing.
- **How it was built.** A build with `--no-bounds-checks` or `--no-null-checks` lacks that run-time half; `pop_back` on an empty vector, iterators and `substr` were never covered ([chapter 7](07-run-time-checks.md)).

The full scope is [chapter 13](13-contract-and-threats.md).
