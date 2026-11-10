# 7. Run-time checks

Some mistakes cannot be found by reading the program: the index is computed from input, the pointer comes back empty from a lookup that found nothing. For these CeeLing inserts a check into
the code it generates. The check stops the program on the spot and names the `.clg` line, instead of letting it read or write memory it does not own.

There are two checks, both on by default:

* **Bounds.** Every subscript `v[i]` or `s[i]` on a vector or a string, read or write, is checked against the size.
* **Null.** Every access through an object handle (`n.field`, `n.method()`, `n[key]` on a class) is checked for an empty handle.

> **The claim, exactly.** In a program compiled with the checks on, a vector or string subscript that is out of range, or a member access through an empty object handle, stops the program with
> a message naming the `.clg` line instead of performing the access. This says nothing about a program built with the switches off, and nothing about what "What it does not cover" lists.

Both end in `abort()`. They are a safety net for bugs, not error handling: a program that expects a bad index must test it first.

**Why run time and not compile time.** Dangling references, use after move and the sealed hatches (chapters 6 and 8) are decided by the compiler. These two are not, because it would have to
prove for every `v[i]` that `i < v.size()` and for every `n.field` that `n` is not empty, through every loop and call that led there, and most programs people write would be rejected. So
nothing is proven: every subscript and every member access is checked, and only the C++ compiler that builds the output may drop a check it can show is redundant.

Line numbers in the messages are the line numbers of the files shown here.

## Bounds checks

### Reading past the end

<!-- example: 07_bounds_off_by_one.cpp -->
```cpp
// Compiles with no warning. At run time v[v.size()] reads one element past the end: undefined
// behaviour, and usually no crash, just a stale or garbage number.
#include <iostream>
#include <vector>

int main() {
    std::vector<int> v;
    v.push_back(10);
    v.push_back(20);
    v.push_back(30);
    std::cout << v[v.size()] << std::endl;
    return 0;
}
```

**Why C++ does not stop it.** `operator[]` of `std::vector` is unchecked on purpose, for speed, and the index is a run-time value. Out of range is undefined behaviour: the program may print
a neighbouring heap word, print zero, crash or carry on. `g++ -Wall -Wextra` says nothing. C++ has opt-in tools, none a statement about the source: `v.at(i)` (below), `-D_GLIBCXX_ASSERTIONS`
(I ran this twin with it: it aborts with `Assertion '__n < this->size()' failed`), sanitizers (not installed on this machine, so not run), hardened library modes. One trap I did run:
libstdc++ switches its own assertions on in *unoptimized* builds only, so this twin aborts at `-O0` and prints a stale number at `-O2`. The same source behaves differently by build flag
(measured with g++ on one machine; not kept as a test).

**What CeeLing does.** The subscript becomes a checked access that stops with the line, the index and the size:

<!-- example: 07_bounds_off_by_one.clg -->
```cpp
int main() {
    std::vector<int> v;
    v.push_back(10);
    v.push_back(20);
    v.push_back(30);
    int n = v.size();
    std::cout << v[n] << std::endl;
    return 0;
}
```
<!-- output: 07_bounds_off_by_one.clg -->
```
cling: index out of bounds at line 7 (index 3, size 3)
```

<!-- generated: 07_bounds_off_by_one.clg "int main()" -->
```cpp
int main() {
    std::vector<int> v;
    v.push_back(10);
    v.push_back(20);
    v.push_back(30);
    int n = v.size();
    std::cout << cling::at(v, n, 7) << std::endl;
    return 0;
}
```

`cling::at` is a small helper copied into every generated file that has a subscript:

<!-- generated: 07_bounds_off_by_one.clg "template <typename C, typename I>" -->
```cpp
template <typename C, typename I>
inline decltype(auto) at(C& c, I i, int line) {
    if constexpr (has_key_type<std::remove_const_t<C>>::value) {
        return c[i];
    } else {
        if (static_cast<unsigned long long>(i) >= static_cast<unsigned long long>(c.size())) {
            std::fprintf(stderr, "cling: index out of bounds at line %d (index %lld, size %lld)\n",
                         line, static_cast<long long>(i), static_cast<long long>(c.size()));
            std::abort();
        }
        return c[static_cast<typename std::remove_const_t<C>::size_type>(i)];
    }
}
```

One unsigned comparison covers `i >= size` and every negative `i`. Reads and writes use the same function (it returns a reference). A type with a `key_type`, which is what `std::map` has,
is passed through unchecked (see "A map is not a vector").

**Where it stops.** Nothing is proven: a constant index past the end is not a compile error, it is the same run-time stop.

### Writing past the end

A store is the more dangerous half: it corrupts memory instead of reading it.

<!-- example: 07_bounds_write.cpp -->
```cpp
// Compiles with no warning. The store lands outside the vector's buffer and corrupts the heap:
// undefined behaviour, and the damage usually shows up later, somewhere else.
#include <iostream>
#include <vector>

int main() {
    std::vector<int> v;
    v.push_back(1);
    int i = 100;
    v[i] = 9;
    std::cout << "done" << std::endl;
    return 0;
}
```

**Why C++ does not stop it.** The subscript of a vector is an unchecked pointer addition; the type `std::vector<int>` cannot say that an index is valid.

**What CeeLing does.** The assignment target goes through the same check. `"done"` is never printed, the store did not happen:

<!-- example: 07_bounds_write.clg -->
```cpp
int main() {
    std::vector<int> v;
    v.push_back(1);
    int i = 100;
    v[i] = 9;
    std::cout << "done" << std::endl;
    return 0;
}
```
<!-- output: 07_bounds_write.clg -->
```
cling: index out of bounds at line 5 (index 100, size 1)
```

### A negative index

<!-- example: 07_bounds_negative.cpp -->
```cpp
// Compiles with no warning. The int -1 is converted to size_t, a huge number, and operator[]
// computes buffer + that offset: in practice the 8 bytes before the buffer. Undefined behaviour.
#include <iostream>
#include <vector>

int main() {
    std::vector<int> v;
    v.push_back(1);
    int i = 0 - 1;
    std::cout << v[i] << std::endl;
    return 0;
}
```

**Why C++ does not stop it.** The parameter of `operator[]` is an unsigned `size_t`. A negative `int` is converted silently into a huge number and the address arithmetic wraps to the element
before the buffer. No warning.

**What CeeLing does.** The comparison is unsigned, so `-1` is just a very large number caught by the same test. The message prints the signed value you wrote:

<!-- example: 07_bounds_negative.clg -->
```cpp
int main() {
    std::vector<int> v;
    v.push_back(1);
    int i = 0 - 1;
    std::cout << v[i] << std::endl;
    return 0;
}
```
<!-- output: 07_bounds_negative.clg -->
```
cling: index out of bounds at line 5 (index -1, size 1)
```

### Strings

In C++, `s[s.size()]` is legal on a `std::string` (it is the terminating `'\0'`). CeeLing has no such notion: an index equal to the size is out of bounds.

<!-- example: 07_bounds_string.cpp -->
```cpp
// s[3] on "abc" is legal C++ (it is the terminating '\0'), s[10] is not: undefined behaviour.
// Neither draws a warning.
#include <iostream>
#include <string>

int main() {
    std::string s = "abc";
    int i = 10;
    std::cout << int(s[3]) << std::endl;
    std::cout << s[i] << std::endl;
    return 0;
}
```
<!-- example: 07_bounds_string.clg -->
```cpp
int main() {
    string s = "abc";
    int i = 3;
    std::cout << s[i] << std::endl;
    return 0;
}
```
<!-- output: 07_bounds_string.clg -->
```
cling: index out of bounds at line 4 (index 3, size 3)
```

### Why `abort()` and not an exception

C++ has a checked subscript, `at()`, which throws `std::out_of_range`. Whatever surrounds the call may catch it and carry on:

<!-- example: 07_bounds_at.cpp -->
```cpp
// C++ does have a checked access: .at() throws std::out_of_range. It is opt-in, spelled
// differently from the subscript everybody writes, and being an exception it can be swallowed.
#include <iostream>
#include <vector>

int main() {
    std::vector<int> v;
    v.push_back(1);
    try {
        std::cout << v.at(5) << std::endl;
    } catch (...) {
        std::cout << "ignored" << std::endl;
    }
    std::cout << "carried on" << std::endl;
    return 0;
}
```

After an out-of-range access the program holds a wrong index and probably wrong state; a `catch` written for another failure would hide the bug. CeeLing ends the process, so no `catch`
can swallow the stop. Below, the handler is never entered and `"carried on"` is never printed:

<!-- example: 07_bounds_not_catchable.clg -->
```cpp
int main() {
    std::vector<int> v;
    v.push_back(1);
    int i = 4;
    try {
        std::cout << v[i] << std::endl;
    } catch (string e) {
        std::cout << "caught " << e << std::endl;
    }
    std::cout << "carried on" << std::endl;
    return 0;
}
```
<!-- output: 07_bounds_not_catchable.clg -->
```
cling: index out of bounds at line 6 (index 4, size 1)
```

The exit status is 3 with the MinGW runtime used for this guide; it is whatever `abort()` gives on your platform (a `SIGABRT` kill on Linux).

### A map is not a vector

`m[key] = value` on a `std::map` is insert-or-update by key, not an index; comparing the key with the size would break every map program. The helper tells them apart by type, so correct
programs behave as before:

<!-- example: 07_bounds_in_range.clg -->
```cpp
int main() {
    std::vector<int> v;
    v.push_back(10);
    v.push_back(20);
    v[1] = v[0] + v[1];
    string s = "abc";
    std::map<string, int> m;
    m["k"] = 5;
    m["k"] = m["k"] + 1;
    std::vector<std::vector<int>> rows;
    std::vector<int> r;
    r.push_back(4);
    rows.push_back(r);
    rows[0][0] = rows[0][0] + 1;
    std::cout << v[1] << " " << s[2] << " " << m["k"] << " " << rows[0][0] << std::endl;
    return 0;
}
```
<!-- output: 07_bounds_in_range.clg -->
```
30 c 6 5
```

## Null checks

"All objects are smart pointers" does not mean "never null": a `unique_ptr` or `shared_ptr` can be empty, and `weak_ptr::lock()` is empty once the target is gone. An empty pointer has the
type of a full one.

### A lookup that found nothing

<!-- example: 07_null_lookup.cpp -->
```cpp
// find() returns an empty shared_ptr when nothing matches. The type of the result is the same
// whether it is empty or not, so nothing makes the caller test it. hit->v on an empty pointer is
// undefined behaviour.
#include <iostream>
#include <memory>

struct Node {
    int v;
    explicit Node(int x) : v(x) {}
};

std::shared_ptr<Node> find(std::shared_ptr<Node> n, int key) {
    if (n->v == key) { return n; }
    return nullptr;
}

int main() {
    auto a = std::make_shared<Node>(5);
    auto hit = find(a, 9);
    std::cout << hit->v << std::endl;
    return 0;
}
```

**Why C++ does not stop it.** `find` returns `std::shared_ptr<Node>` whether it found something or not, and `->` on an empty smart pointer is undefined behaviour. The language cannot say
"not empty here". (`std::optional` or `gsl::not_null` help where the author uses them; they do not change what `->` does on a `shared_ptr`.) No warning.

**What CeeLing does.** Every access through a handle goes through `cling::nn(handle, "line N")`. An empty handle prints the line and aborts:

<!-- example: 07_null_lookup.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
    int value() { return v; }
};

Node find(Node n, int key) {
    if (n.v == key) { return n; }
    return nullptr;
}

int main() {
    Node a = new Node(5);
    Node hit = find(a, 9);
    std::cout << hit.value() << std::endl;
    return 0;
}
```
<!-- output: 07_null_lookup.clg -->
```
cling: null dereference at line 16
```

You never write `nn`. This is what `main` became, and `nn` itself:

<!-- generated: 07_null_lookup.clg "int main()" -->
```cpp
int main() {
    std::unique_ptr<Node> a = std::make_unique<Node>(5);
    std::unique_ptr<Node> hit = find(std::move(a), 9);
    std::cout << cling::nn(hit, "line 16")->value() << std::endl;
    return 0;
}
```
<!-- generated: 07_null_lookup.clg "template <typename P>" -->
```cpp
template <typename P>
inline P&& nn(P&& p, const char* where) {
    if (!p) {
        std::fprintf(stderr, "cling: null dereference at %s\n", where);
        std::abort();
    }
    return std::forward<P>(p);
}
```

`nn` takes and returns a forwarding reference: an lvalue handle comes back as the same lvalue, so there is no `shared_ptr` copy and no reference-count traffic.

### A field that is empty

The last node of a list has no `next`.

<!-- example: 07_null_field.cpp -->
```cpp
// The last node of a list has no next. n->next->v dereferences an empty unique_ptr: undefined
// behaviour, and no warning.
#include <iostream>
#include <memory>

struct Node {
    std::unique_ptr<Node> next;
    int v;
    explicit Node(int x) : v(x) {}
};

int main() {
    auto n = std::make_unique<Node>(1);
    std::cout << n->next->v << std::endl;
    return 0;
}
```
<!-- example: 07_null_field.clg -->
```cpp
class Node {
public:
    Node next;
    int v;
    Node(int x) { v = x; }
};

int main() {
    Node n = new Node(1);
    std::cout << n.next.v << std::endl;
    return 0;
}
```
<!-- output: 07_null_field.clg -->
```
cling: null dereference at line 10
```

### Which accesses are checked

Every `->` the emitter produces for a member access, a method call or a class-typed `operator[]`. There is no analysis that proves a handle non-null and skips the check. Here are three sites,
with the generated code (`row[0].v` is checked twice: the subscript, then the element):

<!-- example: 07_null_sites.clg -->
```cpp
class Node {
public:
    int v;
    Node next;
    Node(int x) { v = x; }
    int value() { return v; }
};

int main() {
    std::vector<Node> row;
    row.push_back(new Node(1));
    Node first = row[0];
    std::cout << first.v << std::endl;
    std::cout << first.value() << std::endl;
    std::cout << row[0].v << std::endl;
    return 0;
}
```
<!-- generated: 07_null_sites.clg "int main()" -->
```cpp
int main() {
    std::vector<std::shared_ptr<Node>> row;
    row.push_back(std::make_shared<Node>(1));
    std::shared_ptr<Node> first = cling::at(row, 0, 12);
    std::cout << cling::nn(first, "line 13")->v << std::endl;
    std::cout << cling::nn(first, "line 14")->value() << std::endl;
    std::cout << cling::nn(cling::at(row, 0, 15), "line 15")->v << std::endl;
    return 0;
}
```

A class's own `operator[]` called on a handle is a member access like any other. The second call below is on an empty handle and stops the program before anything is printed:

<!-- example: 07_null_subscript_class.clg -->
```cpp
class Table {
public:
    int base;
    Table(int b) { base = b; }
    int operator[](int i) { return base + i; }
};

Table nothing() {
    return nullptr;
}

int main() {
    Table ok = new Table(100);
    int a = ok[5];
    Table t = nothing();
    int b = t[5];
    std::cout << a + b << std::endl;
    return 0;
}
```
<!-- output: 07_null_subscript_class.clg -->
```
cling: null dereference at line 16
```
<!-- generated: 07_null_subscript_class.clg "int main()" -->
```cpp
int main() {
    std::unique_ptr<Table> ok = std::make_unique<Table>(100);
    int a = cling::nn(ok, "line 14")->operator[](5);
    std::unique_ptr<Table> t = nothing();
    int b = cling::nn(t, "line 16")->operator[](5);
    std::cout << (a + b) << std::endl;
    return 0;
}
```

### A back link whose target is gone

A back reference (`prev`, `parent`, `owner`) must not own its target or two objects keep each other alive (chapter 4), so the compiler makes it a `weak_ptr`. A weak pointer does not keep
its target alive, so reading through it can find nothing. In C++ that is silent undefined behaviour (with a raw `Node*` for the back link it would be a read of freed memory):

<!-- example: 07_weak_expired.cpp -->
```cpp
// A back link kept as weak_ptr does not keep its target alive. When the target is gone, lock()
// returns an empty pointer, and the -> on it is undefined behaviour. (With a raw Node* for the
// back link it would be a read of freed memory instead.)
#include <iostream>
#include <memory>

struct Node {
    int v;
    std::shared_ptr<Node> next;
    std::weak_ptr<Node> prev;
    explicit Node(int x) : v(x) {}
};

std::shared_ptr<Node> tailOf() {
    auto first = std::make_shared<Node>(1);
    first->next = std::make_shared<Node>(2);
    first->next->prev = first;
    return first->next;                // first is destroyed here: only its next survives
}

int main() {
    auto tail = tailOf();
    std::cout << tail->v << std::endl;
    std::cout << tail->prev.lock()->v << std::endl;
    return 0;
}
```

**What CeeLing does.** A `weak_ptr` field is read through `lock()` and the result is checked like any handle. Here the first node dies when `tailOf` returns, and the back link finds nothing:

<!-- example: 07_weak_expired.clg -->
```cpp
class Node {
public:
    int v;
    Node next;
    Node prev;
    Node(int x) { v = x; }
};

Node tailOf() {
    Node first = new Node(1);
    first.next = new Node(2);
    first.next.prev = first;
    return first.next;
}

int main() {
    Node tail = tailOf();
    int own = tail.v;
    int before = tail.prev.v;
    std::cout << own << " " << before << std::endl;
    return 0;
}
```
<!-- output: 07_weak_expired.clg -->
```
cling: null dereference at line 19
```
<!-- generated: 07_weak_expired.clg "class Node {" -->
```cpp
class Node {
public:
    int v{};
    std::shared_ptr<Node> next;
    std::weak_ptr<Node> prev;
    Node(int x);
    ~Node();
};
```
<!-- generated: 07_weak_expired.clg "int main()" -->
```cpp
int main() {
    std::shared_ptr<Node> tail = tailOf();
    int own = cling::nn(tail, "line 18")->v;
    int before = cling::nn(cling::nn(tail, "line 19")->prev.lock(), "line 19")->v;
    std::cout << own << " " << before << std::endl;
    return 0;
}
```

An expired back reference stops the program with a line number instead of reading freed memory. `!prev` and `prev == nullptr` remain valid ways to test the link first; they compile
against the result of `lock()`.

## The two switches, and what they cost

| Flag | Effect |
|---|---|
| `--no-bounds-checks` | `v[i]` and `s[i]` are emitted as plain C++ subscripts |
| `--no-null-checks` | member accesses are emitted as plain `->` |

The first program of this chapter, compiled with `--no-bounds-checks` (the example file carries that flag):

<!-- example: 07_no_bounds_checks.clg -->
```cpp
int main() {
    std::vector<int> v;
    v.push_back(10);
    v.push_back(20);
    v.push_back(30);
    int n = v.size();
    std::cout << v[n] << std::endl;
    return 0;
}
```
<!-- generated: 07_no_bounds_checks.clg "int main()" -->
```cpp
int main() {
    std::vector<int> v;
    v.push_back(10);
    v.push_back(20);
    v.push_back(30);
    int n = v.size();
    std::cout << v[n] << std::endl;
    return 0;
}
```

`v[n]` is the unchecked access again. It reads past the end of the buffer and prints whatever is there (two runs on the machine used for this guide gave two different numbers; an
observation, not a checked claim). Likewise `--no-null-checks`:

<!-- example: 07_no_null_checks.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
    int value() { return v; }
};

Node find(Node n, int key) {
    if (n.v == key) { return n; }
    return nullptr;
}

int main() {
    Node a = new Node(5);
    Node hit = find(a, 9);
    std::cout << hit.value() << std::endl;
    return 0;
}
```
<!-- generated: 07_no_null_checks.clg "int main()" -->
```cpp
int main() {
    std::unique_ptr<Node> a = std::make_unique<Node>(5);
    std::unique_ptr<Node> hit = find(std::move(a), 9);
    std::cout << hit->value() << std::endl;
    return 0;
}
```

`hit->value()` is called on an empty pointer. On this machine the program died with a segmentation fault; undefined behaviour is not required to do even that.

**A claim about a program is a claim about how it was built.** "This program cannot index out of bounds" is true of the default build and false of the same source built with
`--no-bounds-checks`; nothing in the `.clg` file records which was used. The zlib port ships built with both checks off, to match the speed of the Rust zlib it is compared with (a checked
build is one flag away); the cJSON port ships with both on.

**Cost** (measured on sibling projects; not re-run for this guide):

* Bounds: **+5.5 % on decompression, +11.3 % on compression** of the zlib port, over 12 Silesia files with identical output. One program (40 subscript sites in the decoder, 145 in the
  encoder); I have no number for other code.
* Null: one branch per access. On the cJSON port, building with `--no-bounds-checks --no-null-checks` shows what both checks cost together: about 5% on the benchmark document (`cJSON-cling/PERFORMANCE.md`); one program, one machine.

## What it does not cover

These are the known limits. The claim at the top of the chapter stops here.

**`pop_back()` on an empty vector.** It is not a subscript. The compiler accepts it and emits it unchanged, which is undefined behaviour (on this machine the program crashed). C++'s own
`-D_GLIBCXX_ASSERTIONS` does catch this one; CeeLing's check does not.

<!-- example: 07_pop_back_not_covered.clg -->
```cpp
int main() {
    std::vector<int> v;
    v.pop_back();
    std::cout << v.size() << std::endl;
    return 0;
}
```

**`std::string::substr` with a bad position.** libstdc++ throws `std::out_of_range`, which `catch (string e)` does not catch, so the program ends in `std::terminate`. The message names the
problem but not the `.clg` line:

<!-- example: 07_substr_not_covered.clg -->
```cpp
int main() {
    string s = "abc";
    try {
        string t = s.substr(10, 1);
        std::cout << t << std::endl;
    } catch (string e) {
        std::cout << "caught " << e << std::endl;
    }
    return 0;
}
```
<!-- output: 07_substr_not_covered.clg -->
```
terminate called after throwing an instance of 'std::out_of_range'
  what():  basic_string::substr: __pos (which is 10) > this->size() (which is 3)
```

**Also not covered:**

* **Iterators past `end()`.** An iterator is not a subscript; dereferencing one at or past `end()` is not checked.
* **Anything an `ext_fn` shim indexes or dereferences,** and **a handle passed to an `ext_fn`** (not inspected). The shim is hand-written C++, a trusted boundary
  ([chapter 8](08-sealed-escape-hatches.md), [chapter 11](11-talking-to-cpp.md)).
* **`this`** is not checked.
* **A `ref` observer that dangles.** The null check finds an *empty* handle. An observer lowers to a raw pointer whose safety rests on the static analysis of
  [chapter 6](06-dangling-references.md); making observers self-checking is an open item.
* **The file name.** The message carries the line, not the file.
* **Recovery.** Both checks abort. They turn silent corruption into a stop; they do not continue.

The promise of [chapter 1](01-why-ceeling.md) about dangling references and use after move is a compile-time promise and does not depend on the switches; the promise about bounds and null does.
Every threat with its enforcement is in the table of [chapter 13](13-contract-and-threats.md). The next chapter is about constructs that are not checked but removed.
