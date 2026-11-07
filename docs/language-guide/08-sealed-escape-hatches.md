# 8. Sealed escape hatches and default-reject

The promise of [chapter 1](01-why-ceeling.md), no dangling references, is only as strong as its weakest door. If one line of a program can turn a smart pointer back into a raw pointer, every
analysis in chapters 4 to 6 is decorative. So this chapter is about what the language **removes**, and about what it refuses by default.

## The principle

In C++ the unsafe operations are not rare tools in a corner of the language. Raw pointers, `.get()`, `delete`, casts, `union`, `c_str()`, globals are the ordinary vocabulary, used in every
library interface and in most code. C++ cannot forbid them without breaking the code that exists, so it can only document that misusing them is undefined behaviour, and a compiler can only
warn when it happens to see the misuse. Every example below compiles with `g++ -Wall -Wextra`, almost all of them without a word.

A safe dialect therefore has to be a *different language*: it must **remove** the operation, not warn about it. CeeLing can, because it is not asked to accept existing C++. Two rules follow:

1. **Sealed.** A construct that has no safe use in CeeLing has no place in it: using it is a compile error. There is nothing to configure and nothing to annotate.
2. **Default-reject, never default-allow.** The checker knows the functions and methods of your program, a table of library facts, and the `ext_fn` declarations you wrote. A call to
   anything else is rejected, instead of being assumed harmless.

And one consequence, which is why the list below is a closure and not a menu: **one open hole voids the contract.** Section "`.get()`" shows the smallest example.

How a rejection looks depends on where it is caught. Some constructs have no grammar rule, and the parser stops with `Error: Syntax error ... near: ...` (sometimes with an explanatory
suggestion). Others parse as ordinary expressions and are refused by the semantic checks, with `error[semantic/forbidden-construct]` and a `help:` line. Both are compile errors: no C++ is
written. The messages in this chapter are what the compiler printed.

| What C++ allows | In CeeLing | Section |
|---|---|---|
| raw pointer `T*`, address-of `&x` | rejected | Raw pointers |
| `p.get()` on a smart pointer | rejected | `.get()` |
| `delete`, raw `new` | rejected | `delete` and raw `new` |
| `reinterpret_cast`, `const_cast`, C-style cast, `static_cast`, `dynamic_cast` | rejected; conversion between primitives only | Casts |
| `union` | rejected | `union` |
| `s.c_str()` | rejected | `c_str()` |
| `int n;` read before it is set | rejected for locals | Uninitialized variables |
| global variable | rejected | File-scope variables |
| `throw 42;` | rejected; only strings | Throwing |
| virtual call, or `this` passed, from a constructor | rejected | Traps of construction |
| a call to something the compiler has no facts about | rejected | Calls the compiler does not know |
| a `ref` bound to something with no visible owner | rejected | `ref` without an owner |
| a call into hand-written C++ | admitted, and trusted | `ext_fn` |

## Raw pointers

<!-- example: 08_raw_pointer.cpp -->
```cpp
// A raw pointer remembers the address of a local that is about to die. The store goes through a
// function, so the compiler does not see a local and a global side by side; no warning. Reading
// keep->v afterwards is undefined behaviour.
#include <iostream>

struct Node {
    int v;
};

Node* keep = nullptr;

void remember(Node& n) { keep = &n; }

void setup() {
    Node local{42};
    remember(local);
}                                           // local is destroyed here

int main() {
    setup();
    std::cout << keep->v << std::endl;
    return 0;
}
```

**Why C++ does not stop it.** A `T*` carries no lifetime and no owner. `keep = &n` is a perfectly ordinary assignment, and whether the address is still good when it is read is a question about
the whole program. Here the store goes through a function, so `g++` sees no local next to a global and is silent (when both are visible in one function recent g++ does warn, which is a
heuristic, not a rule).

**What CeeLing does.** There is no pointer type. A declaration written as one is rejected:

<!-- example: 08_raw_pointer.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

int main() {
    Node a = new Node(1);
    Node * p;
    return 0;
}
```
<!-- error: 08_raw_pointer.clg -->
```
error[semantic/forbidden-construct]: raw pointer types (`T*`) are forbidden (found `Node*`)
  --> 08_raw_pointer.clg:9:5
  help: pointers have no legal path through CeeLing (guide chapter 13, hatch #15); declare a plain object variable and let the compiler choose the smart-pointer type.
```

(`Node * p;` parses as a multiplication, and a class name is never a legal operand of one, which is how the checker knows what was meant.) The other spellings have no grammar rule at all:

<!-- example: 08_raw_pointer_init.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

int main() {
    Node* p = new Node(1);
    return 0;
}
```
<!-- error: 08_raw_pointer_init.clg -->
```
Error: Syntax error at line 8, column 11, node type 'ERROR', near: p =. Suggestion: check whether the statement is missing a trailing ';'.
```
<!-- example: 08_int_pointer.clg -->
```cpp
int main() {
    int n = 5;
    int* p = &n;
    return 0;
}
```
<!-- error: 08_int_pointer.clg -->
```
Error: Syntax error at line 3, column 5, node type 'ERROR', near: int. Suggestion: check whether the statement is missing a trailing ';'.
```

The message is a generic syntax error, because the parser has no rule to quote; the line and the near text are the useful part.

**Where it stops.** This is about the source you write. The C++ the compiler generates does use raw pointers internally (a `ref` observer is one, chapter 6): it is safe because the dangling
analysis has already proved the observer cannot outlive what it observes, and the pointer is never reachable from CeeLing source.

## `.get()`

<!-- example: 08_get_and_delete.cpp -->
```cpp
// .get() hands out the raw pointer inside a smart pointer; delete on it frees the object a second
// time when p is destroyed. Both are ordinary, well-typed C++. Compiles with no warning; the
// program has undefined behaviour (a double free).
#include <memory>

struct Node {
    int v;
    explicit Node(int x) : v(x) {}
};

int main() {
    auto p = std::make_unique<Node>(1);
    Node* raw = p.get();
    delete raw;
    return 0;
}
```

**Why C++ does not stop it.** `.get()` is an ordinary, well-typed method of `unique_ptr` and `shared_ptr`. It is how libraries take a raw pointer, so it cannot be withdrawn.

**What CeeLing does.** On a CeeLing object it is an error:

<!-- example: 08_get.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

int main() {
    Node p = new Node(1);
    int r = p.get();
    return 0;
}
```
<!-- error: 08_get.clg -->
```
error[semantic/forbidden-construct]: '.get()' is forbidden on a CeeLing object ('p' is a Node)
  --> 08_get.clg:9:13
  help: '.get()' would extract a raw pointer, which has no legal use in CeeLing (guide chapter 13, hatch #14) -- use the object directly, or add a named accessor method to the class if you need a specific field.
```

This is the smallest example of "one hole voids the contract". If `.get()` were allowed, one call would give back a raw pointer, the pointer could be stored anywhere, and nothing in
chapter 6 could see it. Raw pointers have no legal destination in CeeLing, so `.get()` has no purpose, and it is closed rather than analysed.

A class that declares its **own** method called `get` is unaffected; the check rejects only the compiler-generated smart-pointer accessor, not the name:

<!-- example: 08_own_get.clg -->
```cpp
class Box {
public:
    int v;
    Box(int x) { v = x; }
    int get() { return v; }
};

int main() {
    Box b = new Box(7);
    int r = b.get();
    std::cout << r << std::endl;
    return 0;
}
```
<!-- output: 08_own_get.clg -->
```
7
```

## `delete` and raw `new`

The twin above also frees the object a second time with `delete raw;` (a double free, once `p` is destroyed). C++ cannot forbid `delete`: it is how every owning type is implemented.

**What CeeLing does.** Objects are freed by the owner the compiler chose, so `delete` has no meaning and no grammar rule:

<!-- example: 08_delete.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

int main() {
    Node p = new Node(1);
    delete p;
    return 0;
}
```
<!-- error: 08_delete.clg -->
```
Error: Syntax error at line 9, column 5, node type 'ERROR', near: delete p; Suggestion: raw `delete` is forbidden (hatch #15) -- object lifetime is managed automatically by the compiler-chosen smart pointer; there is no manual delete.
```

`new` exists, but only in the form `Node n = new Node(1);`, which the compiler lowers to `make_unique` or `make_shared`. `Node* p = new Node(1);`, a raw allocation, is the syntax error shown under
"Raw pointers".

## Casts

<!-- example: 08_casts.cpp -->
```cpp
// Three ways to say "treat this as something else", all legal C++. The first reads a double's
// bits through an integer pointer (undefined behaviour by the aliasing rules), the second writes
// through a const object (undefined behaviour), the third is a C-style cast, which silently
// picks whichever C++ cast it needs, reinterpret_cast included.
#include <iostream>

struct Config {
    int level;
};

int main() {
    double d = 1.0;
    double* pd = &d;
    long long bits = *reinterpret_cast<long long*>(pd);

    const Config c{1};
    const_cast<Config&>(c).level = 2;

    const char* text = "abc";
    unsigned long long address = (unsigned long long)text;

    std::cout << bits << " " << c.level << " " << address << std::endl;
    return 0;
}
```

**Why C++ does not stop it.** Casts are how C++ code says "I know better than the type system". `reinterpret_cast` and `const_cast` are legal for any types, the C-style cast silently chooses
whichever of the C++ casts it needs, and all three compile without complaint. The first line is a type-confusion read, the second a write to a `const` object; both are undefined behaviour.

**What CeeLing does.** There is no cast syntax. A cast spelled the C++ way is not a call to anything the checker knows, and is refused:

<!-- example: 08_reinterpret_cast.clg -->
```cpp
int main() {
    double d = 1.0;
    int x = reinterpret_cast<int>(d);
    return 0;
}
```
<!-- error: 08_reinterpret_cast.clg -->
```
error[semantic/forbidden-construct]: call to unknown function/method `reinterpret_cast`
  --> 08_reinterpret_cast.clg:3:13
  help: CeeLing has no borrow/ownership summary for anything it did not compile itself; unknown calls are rejected by default rather than silently trusted (the default-reject rule). Define `reinterpret_cast` in CeeLing source, or call an existing CeeLing function/method instead.
```
<!-- example: 08_const_cast.clg -->
```cpp
class Config {
public:
    int level;
    Config(int l) { level = l; }
};

int main() {
    Config c = new Config(1);
    Config m = const_cast<Config>(c);
    return 0;
}
```
<!-- error: 08_const_cast.clg -->
```
error[semantic/forbidden-construct]: call to unknown function/method `const_cast`
  --> 08_const_cast.clg:9:16
  help: CeeLing has no borrow/ownership summary for anything it did not compile itself; unknown calls are rejected by default rather than silently trusted (the default-reject rule). Define `const_cast` in CeeLing source, or call an existing CeeLing function/method instead.
```

`static_cast` and `dynamic_cast` are refused the same way. The C-style cast has no grammar rule:

<!-- example: 08_c_style_cast.clg -->
```cpp
int main() {
    double d = 1.5;
    int x = (int)d;
    return 0;
}
```
<!-- error: 08_c_style_cast.clg -->
```
Error: Syntax error at line 3, column 18, node type 'ERROR', near: d. Suggestion: check whether the statement is missing a trailing ';'.
```

**What remains.** Converting between primitive numbers is spelled like a call and accepted: `int(x)`, `double(x)`, `char(x)`, `byte(x)`, `int64(x)`. These are conversions of values, not of
representations; `byte(300)` wraps like an unsigned char, `int(-1.9)` truncates toward zero.

<!-- example: 08_primitive_casts.clg -->
```cpp
int main() {
    double d = 3.7;
    int i = int(d);
    char c = char(65);
    byte b = byte(300);
    int64 big = int64(65536) * int64(65536);
    int neg = int(-1.9);
    std::cout << i << " " << c << " " << int(b) << " " << big << " " << neg << std::endl;
    return 0;
}
```
<!-- output: 08_primitive_casts.clg -->
```
3 A 44 4294967296 -1
```

The argument must be a single primitive. Applied to an object it would read a smart pointer as a number; applied to a string it is not a conversion at all:

<!-- example: 08_class_cast.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

int main() {
    Node n = new Node(1);
    int x = int(n);
    return 0;
}
```
<!-- error: 08_class_cast.clg -->
```
error[semantic/forbidden-construct]: `int(...)` converts one primitive to another; the argument here is not a single primitive
  --> 08_class_cast.clg:9:13
  help: Primitive conversions take exactly one int/float/double/char/byte/int64 (or bool) value. A class, container or string cannot be cast to a primitive -- that would be a type-confusion hatch (guide chapter 13, #13).
```
<!-- example: 08_string_cast.clg -->
```cpp
int main() {
    string s = "12";
    int x = int(s);
    return 0;
}
```
<!-- error: 08_string_cast.clg -->
```
error[semantic/forbidden-construct]: `int(...)` converts one primitive to another; the argument here is not a single primitive
  --> 08_string_cast.clg:3:13
  help: Primitive conversions take exactly one int/float/double/char/byte/int64 (or bool) value. A class, container or string cannot be cast to a primitive -- that would be a type-confusion hatch (guide chapter 13, #13).
```

For text to number there is a library function on the allow-list, which reports bad input by throwing a string:

<!-- example: 08_stoi.clg -->
```cpp
int main() {
    string s = "12";
    int x = std::stoi(s);
    std::cout << x + 1 << std::endl;
    return 0;
}
```
<!-- output: 08_stoi.clg -->
```
13
```

## `union`

<!-- example: 08_union.cpp -->
```cpp
// Write one member, read another. In C++ this is undefined behaviour (C allows it; C++ does not),
// and every compiler accepts it without a word.
#include <iostream>

union Bits {
    int i;
    float f;
};

int main() {
    Bits b;
    b.i = 1;
    std::cout << b.f << std::endl;
    return 0;
}
```

**Why C++ does not stop it.** A union is a core feature, and reading a member other than the one last written is undefined behaviour that the compiler does not track (C allows it, C++ does not,
and compilers accept both silently).

**What CeeLing does.** No grammar rule; the parser's message says why:

<!-- example: 08_union.clg -->
```cpp
union Bits { int i; float f; };

int main() {
    return 0;
}
```
<!-- error: 08_union.clg -->
```
Error: Syntax error at line 1, column 1, node type 'ERROR', near: union Bits { int i; float f; }; Suggestion: raw `union` is forbidden (hatch #17) -- it allows reading a different member than was written, which is undefined behavior C++ cannot catch. Use a class with distinct fields instead.
```

A class with distinct fields says what a union tried to say. `std::variant` is not supported at all (there is nothing to guard), so there is no tagged-union replacement yet.

## `c_str()`

<!-- example: 08_c_str.cpp -->
```cpp
// c_str() of a temporary string: the string is destroyed at the end of the statement, the
// pointer lives on. Compiles with no warning; reading *p is undefined behaviour.
#include <iostream>
#include <string>

std::string name() { return "a string long enough to live on the heap, not in the object"; }

int main() {
    const char* p = name().c_str();
    std::cout << p << std::endl;
    return 0;
}
```

**Why C++ does not stop it.** `c_str()` returns a raw `const char*` into the string's buffer. It is the same raw pointer as `.get()`, from a string, and it is needed to call C interfaces.

**What CeeLing does.** The scan of `std::string` finds that `c_str` returns a raw pointer, so the admission rule refuses it, for the same reason as `.get()`: a raw pointer has nowhere legal to go.

<!-- example: 08_c_str.clg -->
```cpp
string name() {
    return "a string long enough to live on the heap";
}

int main() {
    int p = name().c_str();
    return 0;
}
```
<!-- error: 08_c_str.clg -->
```
error[semantic/forbidden-construct]: call to unknown function/method `c_str`
  --> 08_c_str.clg:6:13
  help: CeeLing has no borrow/ownership summary for anything it did not compile itself; unknown calls are rejected by default rather than silently trusted (the default-reject rule). Define `c_str` in CeeLing source, or call an existing CeeLing function/method instead.
```

## Uninitialized variables

<!-- example: 08_uninitialized.cpp -->
```cpp
// n is declared and never given a value; reading it is undefined behaviour. g++ -Wall -Wextra
// stays silent when the read is behind a call and a branch, as it usually is in real code.
#include <iostream>

int pick(bool first) {
    int n;
    if (first) { n = 1; }
    return n;
}

int main() {
    std::cout << pick(false) << std::endl;
    return 0;
}
```

**Why C++ does not stop it.** An uninitialized `int` is legal, and whether a path reads it before writing depends on control flow. `g++` can warn at higher optimization levels, but with the read
behind a call, as here, it is silent.

**What CeeLing does.** A local variable declaration must have an initializer. A bare `int n;` has no grammar rule:

<!-- example: 08_uninitialized.clg -->
```cpp
int pick(bool first) {
    int n;
    if (first) { n = 1; }
    return n;
}

int main() {
    std::cout << pick(false) << std::endl;
    return 0;
}
```
<!-- error: 08_uninitialized.clg -->
```
Error: Syntax error at line 2, column 5, node type 'ERROR', near: int n;
```
<!-- example: 08_initialized.clg -->
```cpp
int pick(bool first) {
    int n = 0;
    if (first) { n = 1; }
    return n;
}

int main() {
    std::cout << pick(false) << std::endl;
    return 0;
}
```
<!-- output: 08_initialized.clg -->
```
0
```

One exception, deliberate: a container (`std::vector<int> v;`) may be declared without an initializer, because an empty container is a well-defined state.

**A field is different, and also safe.** A class **field** may be declared without an initializer. For an object the field is an empty handle, and a dereference is checked (chapter 7). A
number, `bool` or `char` field starts at **zero**, as in Java: the compiler writes `{}` after it, which value-initialises it, so a constructor that forgets one leaves a defined value, not garbage:

<!-- example: 08_field_zero.clg -->
```cpp
class Box {
public:
    int count;
    Box() { }
};

int main() {
    Box b = new Box();
    std::cout << b.count << std::endl;
    return 0;
}
```
<!-- generated: 08_field_zero.clg "class Box {" -->
```cpp
class Box {
public:
    int count{};
    Box();
};
```

<!-- output: 08_field_zero.clg -->
```
0
```

`Box()` never writes `count`, and the program reads 0. (Before this was done, the field was the bare C++ declaration `int count;` and held whatever the memory held; the guide's earlier
draft showed that gap. It is closed by construction, not by an analysis: there is no uninitialised primitive left to find.) What stays unchecked is whether zero is the *right* value, and
`std::string` and containers, which are empty to begin with.

## File-scope variables

<!-- example: 08_global.cpp -->
```cpp
// A file-scope variable belongs to no object and no function, so every function and every thread
// can reach it. Two threads add to it with no lock: a data race (undefined behaviour). The
// compiler says nothing.
#include <iostream>
#include <thread>

int counter = 0;

void work() {
    for (int i = 0; i < 100000; i++) { counter = counter + 1; }
}

int main() {
    std::thread t(work);
    work();
    t.join();
    std::cout << counter << std::endl;
    return 0;
}
```

**Why C++ does not stop it.** A global belongs to no object and no function; any function, any thread, any static initializer can reach it. The program above has a data race on `counter` and
compiles in silence.

**What CeeLing does.** A global mutable variable has no owner and no lifetime the checker can reason about, so a variable declaration outside a function or class is rejected:

<!-- example: 08_global.clg -->
```cpp
int counter = 5;

int main() {
    std::cout << counter << std::endl;
    return 0;
}
```
<!-- error: 08_global.clg -->
```
error[semantic/forbidden-construct]: statement at file scope
  --> 08_global.clg:1:1
  help: File-scope variables are not supported (global mutable state has no owner and no lifetime the checker can reason about). Put the value in a function, or pass it as a parameter or a class field.
```

Put the value in a function, pass it as a parameter, or make it a field.

## Throwing

<!-- example: 08_throw.cpp -->
```cpp
// C++ can throw any type, so a handler for one type does not see another. The int is not a
// std::string and not a std::exception: it passes through catch (const std::string&) and ends the
// program in std::terminate.
#include <iostream>
#include <string>

int main() {
    try {
        throw 42;
    } catch (const std::string& e) {
        std::cout << "caught " << e << std::endl;
    }
    return 0;
}
```

**Why C++ does not stop it.** `throw` accepts any type, and a handler matches by exact type. An `int` slips past a handler written for strings, and an uncaught exception ends the program.

**What CeeLing does.** `throw` takes a string, and the handler is `catch (string e)`: the only value a CeeLing program throws is a string. This is a restriction of the exception model more than a memory-safety hatch: an exception
class hierarchy would need inheritance and answers about who owns a thrown object.

<!-- example: 08_throw_non_string.clg -->
```cpp
int main() {
    throw 42;
    return 0;
}
```
<!-- error: 08_throw_non_string.clg -->
```
error[semantic/use-after-move]: `throw` operand is not a string
  --> 08_throw_non_string.clg:2:11
  help: CeeLing's `throw` is restricted to `string` messages -- there is no exception-class hierarchy yet (that needs inheritance/polymorphism, not implemented), so a thrown value can only be a plain error message. Throw a string literal or a `string`-typed variable instead.
```
<!-- example: 08_throw_string.clg -->
```cpp
int main() {
    try {
        throw "bad thing";
    } catch (string e) {
        std::cout << e << std::endl;
    }
    return 0;
}
```
<!-- output: 08_throw_string.clg -->
```
bad thing
```

A thrown string literal is wrapped in `std::string` in the output, so that the handler (which matches `std::string`, not `const char*`) does catch it. Note that a run-time check of chapter 7
is not an exception and is not caught by this handler.

## Traps of construction

Two C++ behaviours that read one way and do another, both statically decidable, and so refused.

**A virtual call from a constructor.** While the base constructor runs the object is still the base: the call does not reach the derived override.

<!-- example: 08_virtual_in_ctor.cpp -->
```cpp
// While Animal's constructor runs, the object is an Animal, not yet a Dog. speak() therefore
// prints "generic" even though a Dog is being built. Legal, silent, and not what the code says.
#include <iostream>

struct Animal {
    Animal() { speak(); }
    virtual ~Animal() {}
    virtual void speak() { std::cout << "generic" << std::endl; }
};

struct Dog : Animal {
    void speak() override { std::cout << "woof" << std::endl; }
};

int main() {
    Dog d;
    return 0;
}
```

C++ allows it and prints "generic" for a `Dog`. CeeLing rejects a call, in a constructor, to a method that is virtual anywhere in the class's hierarchy. (CeeLing has no destructor syntax, so
the other half of the trap cannot be written.)

<!-- example: 08_virtual_in_ctor.clg -->
```cpp
class Animal {
public:
    int age;
    Animal(int a) {
        age = a;
        speak();
    }
    virtual void speak() {
        std::cout << "generic" << std::endl;
    }
};

class Dog : public Animal {
public:
    Dog(int a) { age = a; }
    void speak() override {
        std::cout << "woof" << std::endl;
    }
};

int main() {
    Animal a = new Dog(3);
    return 0;
}
```
<!-- error: 08_virtual_in_ctor.clg -->
```
error[semantic/forbidden-construct]: virtual method `speak` called from a constructor of `Animal`
  --> 08_virtual_in_ctor.clg:6:9
  help: A virtual call inside a constructor does NOT dispatch to a derived class's override -- the object is still being built, so the base version runs instead. That makes the code do something different from what it looks like it does. Move the call to after construction, or call a non-virtual method.
```

**`this` passed from a constructor.** Passing `this` lowers to `shared_from_this()`, which throws `std::bad_weak_ptr` while the object is still being constructed, every time.

<!-- example: 08_this_in_ctor.cpp -->
```cpp
// shared_from_this() needs the object to be owned by a shared_ptr already; inside the
// constructor it never is yet, so it throws std::bad_weak_ptr. The compiler cannot see that.
#include <memory>
#include <vector>

struct Node;

struct Registry {
    std::vector<std::shared_ptr<Node>> all;
};

struct Node : std::enable_shared_from_this<Node> {
    explicit Node(Registry& r) { r.all.push_back(shared_from_this()); }
};

int main() {
    Registry reg;
    auto n = std::make_shared<Node>(reg);      // throws std::bad_weak_ptr
    return 0;
}
```
<!-- example: 08_this_in_ctor.clg -->
```cpp
class Registry {
public:
    Registry() { }
    void addSelf(Registry r) { }
};

class Node {
public:
    Node() {
        Registry reg = new Registry();
        reg.addSelf(this);
    }
};

int main() {
    Node n = new Node();
    return 0;
}
```
<!-- error: 08_this_in_ctor.clg -->
```
error[semantic/forbidden-construct]: `this` passed as an argument from a constructor
  --> 08_this_in_ctor.clg:11:9
  help: passing `this` lowers to shared_from_this(), which throws std::bad_weak_ptr unconditionally when called before the object's shared_ptr control block exists -- and inside a constructor, it never does yet. Move this to after construction (e.g. a separate init() method called once the object is already held by a shared_ptr).
```

The same call from an ordinary method, once the object exists, is accepted:

<!-- example: 08_this_after_ctor.clg -->
```cpp
class Registry {
public:
    int count;
    Registry() { count = 0; }
    void add(Node n) { count = count + 1; }
};

class Node {
public:
    Node() { }
    int announce() {
        Registry reg = new Registry();
        reg.add(this);
        return reg.count;
    }
};

int main() {
    Node n = new Node();
    std::cout << n.announce() << std::endl;
    return 0;
}
```
<!-- output: 08_this_after_ctor.clg -->
```
1
```

## Calls the compiler does not know

<!-- example: 08_unknown_call.cpp -->
```cpp
// firstWord comes from "a library": all the compiler sees is its declaration. The result points
// into the string that was passed in, which here is a temporary, gone at the end of the
// statement. Nothing in the declaration says so, and the compiler does not ask. No warning;
// printing w.text is undefined behaviour.
#include <iostream>
#include <string>

struct View {
    const char* text;
};

View firstWord(const std::string& s) { return View{s.c_str()}; }

int main() {
    View w = firstWord(std::string("hello world, from a temporary string of some length"));
    std::cout << w.text << std::endl;
    return 0;
}
```

**Why C++ does not stop it.** The compiler sees the declaration of `firstWord`: it returns a `View`. Whether the `View` points into the argument is not in the type, so the temporary
string being destroyed at the end of the statement goes unnoticed. This is the central difficulty of using libraries from a safe language: what a library function does with what it is given is
not written anywhere the checker can read.

**What CeeLing does.** A call is accepted only if the callee is a function or method of the program, an entry of the library table (the standard-library calls and methods the compiler has
facts about), or a name you declared with `ext_fn`. Anything else is rejected, rather than treated as a harmless fresh value:

<!-- example: 08_unknown_call.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

int main() {
    Node a = new Node(1);
    Node b = new Node(2);
    Node r = mysteryFunction(a, b);
    std::cout << r.v << std::endl;
    return 0;
}
```
<!-- error: 08_unknown_call.clg -->
```
error[semantic/forbidden-construct]: call to unknown function/method `mysteryFunction`
  --> 08_unknown_call.clg:10:14
  help: CeeLing has no borrow/ownership summary for anything it did not compile itself; unknown calls are rejected by default rather than silently trusted (the default-reject rule). Define `mysteryFunction` in CeeLing source, or call an existing CeeLing function/method instead.
```

There is no hole through qualified names, through `try` bodies, or through a `throw` expression:

<!-- example: 08_unknown_qualified.clg -->
```cpp
int main() {
    string s = "42";
    int x = std::notARealFunction(s);
    return 0;
}
```
<!-- error: 08_unknown_qualified.clg -->
```
error[semantic/forbidden-construct]: call to unknown function/method `std::notARealFunction`
  --> 08_unknown_qualified.clg:3:13
  help: CeeLing has no borrow/ownership summary for anything it did not compile itself; unknown calls are rejected by default rather than silently trusted (the default-reject rule). Define `std::notARealFunction` in CeeLing source, or call an existing CeeLing function/method instead.
```
<!-- example: 08_unknown_in_try.clg -->
```cpp
int main() {
    try {
        int x = mysteryUnknownFunction(3);
        std::cout << x << std::endl;
    } catch (string e) {
        std::cout << e << std::endl;
    }
    return 0;
}
```
<!-- error: 08_unknown_in_try.clg -->
```
error[semantic/forbidden-construct]: call to unknown function/method `mysteryUnknownFunction`
  --> 08_unknown_in_try.clg:3:17
  help: CeeLing has no borrow/ownership summary for anything it did not compile itself; unknown calls are rejected by default rather than silently trusted (the default-reject rule). Define `mysteryUnknownFunction` in CeeLing source, or call an existing CeeLing function/method instead.
```
<!-- example: 08_unknown_in_throw.clg -->
```cpp
int main() {
    try {
        throw mysteryUnknownMessage(7);
    } catch (string e) {
        std::cout << e << std::endl;
    }
    return 0;
}
```
<!-- error: 08_unknown_in_throw.clg -->
```
error[semantic/forbidden-construct]: call to unknown function/method `mysteryUnknownMessage`
  --> 08_unknown_in_throw.clg:3:15
  help: CeeLing has no borrow/ownership summary for anything it did not compile itself; unknown calls are rejected by default rather than silently trusted (the default-reject rule). Define `mysteryUnknownMessage` in CeeLing source, or call an existing CeeLing function/method instead.
```

**Where it stops.** The table decides how much real library code compiles; growing it (and the declaration packs of chapter 11) is how more libraries become usable. The price of default-reject
is that code the checker has no facts about is refused even when it is fine.

## `ref` without an owner

A `ref` is an observer: it looks at an object that someone else owns. It is safe only while the owner is alive, so it may be bound only to something whose death the checker can see.

<!-- example: 08_ref_unowned.cpp -->
```cpp
// A reference to the object a field owns. Clearing the field destroys the object; the reference
// does not know. Compiles with no warning; reading c.v is undefined behaviour.
#include <iostream>
#include <memory>

struct Item {
    int v;
    std::unique_ptr<Item> child;
    explicit Item(int x) : v(x) {}
};

int main() {
    Item parent(1);
    parent.child = std::make_unique<Item>(42);
    Item& c = *parent.child;
    parent.child = nullptr;              // the object c refers to is destroyed
    std::cout << c.v << std::endl;
    return 0;
}
```

**Why C++ does not stop it.** A reference to the object a field owns is just `Node&`. Clearing the field destroys the object; the reference does not know.

**What CeeLing does.** `ref` may observe a local object, an element `v[i]` of a local container, another `ref`, or `nullptr`. Bound to a field, to a getter's result or to a temporary, it is
rejected, because the owner is not a local whose death can be tracked:

<!-- example: 08_ref_field.clg -->
```cpp
class Item {
public:
    int v;
    Item child;
    Item(int x) { v = x; child = nullptr; }
};

int main() {
    Item parent = new Item(1);
    parent.child = new Item(42);
    ref Item c = parent.child;
    parent.child = nullptr;
    std::cout << c.v << std::endl;
    return 0;
}
```
<!-- error: 08_ref_field.clg -->
```
error[semantic/use-after-move]: `ref c` is bound to `parent->child`, whose owner cannot be tracked
  --> 08_ref_field.clg:11:5
  help: a `ref` may only observe an object local, an element `v[i]` of a container local, another `ref`, or nullptr. A field (`p.child`) or a call result (`p.getChild()`, `make()`) has no local owner whose death the checker can see, so the observer could silently dangle. Use a plain (owning/sharing) local instead: `c = ...` declared without `ref`.
```
<!-- example: 08_ref_getter.clg -->
```cpp
class Item {
public:
    int v;
    Item child;
    Item(int x) { v = x; child = nullptr; }
    Item getChild() { return child; }
};

int main() {
    Item parent = new Item(1);
    parent.child = new Item(42);
    ref Item c = parent.getChild();
    parent.child = nullptr;
    std::cout << c.v << std::endl;
    return 0;
}
```
<!-- error: 08_ref_getter.clg -->
```
error[semantic/use-after-move]: `ref c` is bound to `getChild()`, whose owner cannot be tracked
  --> 08_ref_getter.clg:12:5
  help: a `ref` may only observe an object local, an element `v[i]` of a container local, another `ref`, or nullptr. A field (`p.child`) or a call result (`p.getChild()`, `make()`) has no local owner whose death the checker can see, so the observer could silently dangle. Use a plain (owning/sharing) local instead: `c = ...` declared without `ref`.
```
<!-- example: 08_ref_temporary.clg -->
```cpp
class Item {
public:
    int v;
    Item(int x) { v = x; }
};

Item make() {
    return new Item(9);
}

int main() {
    ref Item c = make();
    std::cout << c.v << std::endl;
    return 0;
}
```
<!-- error: 08_ref_temporary.clg -->
```
error[semantic/use-after-move]: `ref c` is bound to `make()`, whose owner cannot be tracked
  --> 08_ref_temporary.clg:12:5
  help: a `ref` may only observe an object local, an element `v[i]` of a container local, another `ref`, or nullptr. A field (`p.child`) or a call result (`p.getChild()`, `make()`) has no local owner whose death the checker can see, so the observer could silently dangle. Use a plain (owning/sharing) local instead: `c = ...` declared without `ref`.
```

The accepted case, for contrast:

<!-- example: 08_ref_local.clg -->
```cpp
class Item {
public:
    int v;
    Item(int x) { v = x; }
};

int main() {
    Item owner = new Item(9);
    ref Item c = owner;
    std::cout << c.v << std::endl;
    return 0;
}
```
<!-- output: 08_ref_local.clg -->
```
9
```

`ref` is also refused as a function parameter (a call hands the object over, so there is nothing for a callee to observe), and as a field or a return type; the full account is
[chapter 6](06-dangling-references.md).

<!-- example: 08_ref_param.clg -->
```cpp
class Item {
public:
    int v;
    Item(int x) { v = x; }
};

int read(ref Item i) {
    return i.v;
}

int main() {
    Item a = new Item(1);
    std::cout << read(a) << std::endl;
    return 0;
}
```
<!-- error: 08_ref_param.clg -->
```
Error: error[semantic/observer_param]: `ref` observers cannot be function parameters (line 7, column 10). Function arguments always transfer ownership (move); pass by value and let the caller keep observers local.
```

## `ext_fn`: the one hatch that is admitted

Everything above is sealed: no syntax reaches it. `ext_fn` is different in kind. It is the source-level equivalent of Rust's `unsafe`, without the name: a **deliberate trust boundary**, written
by you, for C++ that CeeLing cannot read.

```
ext_fn ReturnType name(params) [borrows N];
```

It is a signature only. The body is hand-written C++, linked in separately; CeeLing emits a forward declaration and treats the name as a known callable:

<!-- example: 08_ext_fn.clg -->
```cpp
ext_fn double mySqrt(double x);

int main() {
    double y = mySqrt(4.0);
    std::cout << y << std::endl;
    return 0;
}
```
<!-- generated: 08_ext_fn.clg "double mySqrt(double x);" -->
```cpp
double mySqrt(double x);

int main() {
    double y = mySqrt(4.0);
    std::cout << y << std::endl;
    return 0;
}
```
<!-- example: 08_ext_fn_shim.cpp -->
```cpp
// The C++ side of an ext_fn: the CeeLing file declares `ext_fn double mySqrt(double x);`, this is
// the body, written by hand and linked in. CeeLing never reads it; it believes the declaration.
#include <cmath>

double mySqrt(double x) { return std::sqrt(x); }
```

The first file is the CeeLing side. The second is the shim that supplies the body; it is not part of any check.

* **What it trusts.** The signature. If the arity or types differ from the real function, or the real function does something the signature does not disclose (for example keeps a raw pointer
  into an argument past the call), CeeLing believes the declaration and emits the call. That is a real, silent hole in the contract, exactly as real as a wrong `unsafe` block.
* **What it limits.** Parameters and results are primitives or `std::vector` of primitives; a class is rejected. A container passed to an `ext_fn` is moved into it by default; the optional
  `borrows N` clause declares that the result aliases parameter `N`.
* **What it does not do.** It does not make a name callable for others: declaring `mySqrt` does not make `myCube` known.

<!-- example: 08_ext_fn_class.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

ext_fn int inspect(Node n);

int main() {
    return 0;
}
```
<!-- error: 08_ext_fn_class.clg -->
```
Error: error[semantic/forbidden-construct]: ext_fn parameter `n` type `Node` is not supported (line 7, column 1). ext_fn signatures are restricted to primitive types (int, float, double, bool, string, char) and std::vector<primitive> for now -- a class-typed ext_fn parameter or return needs a hand-written borrow-summary annotation that isn't designed yet (see guide chapter 11).
```
<!-- example: 08_ext_fn_other_call.clg -->
```cpp
ext_fn double mySqrt(double x);

int main() {
    double y = myCube(4.0);
    std::cout << y << std::endl;
    return 0;
}
```
<!-- error: 08_ext_fn_other_call.clg -->
```
error[semantic/forbidden-construct]: call to unknown function/method `myCube`
  --> 08_ext_fn_other_call.clg:4:16
  help: CeeLing has no borrow/ownership summary for anything it did not compile itself; unknown calls are rejected by default rather than silently trusted (the default-reject rule). Define `myCube` in CeeLing source, or call an existing CeeLing function/method instead.
```

The syntax in full (`ref` parameters, `borrows`, the shim pattern), and what the contract says about the C++ behind it, is [chapter 11](11-talking-to-cpp.md). The contract's wording keeps the
boundary visible: *no dangling references in code CeeLing checked, excluding what trusted libraries do with what they are given*. The bounds and null checks of chapter 7 likewise do not look
inside a shim.

## Where this chapter stops

* Sealing is about **source you write**. The generated C++ is plain C++ and uses whatever it needs.
* The sealed list is a list of constructs. It does not say the *remaining* language is safe: dangling references, use after move and the run-time checks are what do that, in chapters 4 to 7.
* Several of these messages are generic syntax errors, not tailored explanations; the construct is refused all the same, and no C++ is generated.
* Everything admitted on trust is in one place, `ext_fn`, plus the libraries you link and the C++ compiler that builds the output ([chapter 13](13-contract-and-threats.md)).
