# 12. Programs of many files

A real program is many files. This chapter is what CeeLing does with them: what `#include "x.clh"` means, how the generated files are compiled one by one, why the pointer kinds of two files can disagree and how
a pair of flags makes them agree, that the safety checks do not stop at a file boundary, and the generated JSON decoders that are the one feature tied to a particular other project.

## The problem: a header carries types, not facts

In C++ a file sees another file through a header, and a header says what a function's parameters are, not what the function does with them:

<!-- example: 12_header_no_facts.cpp -->
```cpp
// Two "files" in one: the declaration is what a header would show, the definition lives in another .cpp.
// (Not run by the checker: it reads freed memory.)
#include <iostream>
#include <vector>

struct Node { int v; };

void growAll(std::vector<Node>& v);          // header: nothing says this may reallocate v

void growAll(std::vector<Node>& v) {         // library .cpp
    for (int i = 0; i < 1000; i++) v.push_back(Node{i});
}

int main() {
    std::vector<Node> v;
    v.push_back(Node{1});
    Node& first = v[0];
    growAll(v);
    std::cout << first.v << std::endl;       // first points into the old buffer
    return 0;
}
```

**Why C++ does not stop it.** `void growAll(std::vector<Node>& v);` is all the other file sees. That the function may reallocate `v`, and so kill `first`, is not part of the declaration, and a reference carries
no lifetime. Each file compiles; the bug is in how they fit. `g++ -Wall -Wextra` is silent.

CeeLing can do better because it reads the other file's *source*, not only a declaration of it. That is also the price: a program is analysed as a whole, one file at a time but with every included file in view.

## `#include "x.clh"`

`.clh` is a name, not a file: it means "the declarations of `x.clg`, which sits next to this file". Two files:

<!-- example: 12_counter_lib.clg -->
```cpp
class Counter {
public:
    int n;
    Counter(int start) { n = start; }
    void bump() { n = n + 1; }
};

int twice(int x) { return x * 2; }
```
<!-- example: 12_counter_main.clg -->
```cpp
#include "12_counter_lib.clh"

int main() {
    Counter c = new Counter(10);
    c.bump();
    std::cout << twice(c.n) << std::endl;
    return 0;
}
```
<!-- output: 12_counter_main.clg -->
```
22
```

What the compiler does with the include:

- It finds `12_counter_lib.clg` beside the including file, parses it, and **merges** its classes, functions and enums into the including program as *external declarations*. Every check then sees them: `twice`,
  `Counter` and `bump` are known, not "unknown calls", and their bodies are analysed too. Includes are followed transitively and a file reached twice is merged once.
- It does **not** emit them again. The including file's C++ gets `#include "12_counter_lib.h"` instead, and the definitions stay in the library's own `.cpp`:

<!-- generated: 12_counter_main.clg "int main()" -->
```cpp
int main() {
    std::unique_ptr<Counter> c = std::make_unique<Counter>(10);
    cling::nn(c, "line 5")->bump();
    std::cout << twice(cling::nn(c, "line 6")->n) << std::endl;
    return 0;
}
```

- The header is generated on request. `cling 12_counter_lib.clg --cpp 12_counter_lib.h` writes declarations only; `--cpp 12_counter_lib.cpp` writes the definitions:

<!-- header: 12_counter_lib.clg "class Counter {" -->
```cpp
class Counter {
public:
    int n{};
    Counter(int start);
    void bump();
};
```

- A missing library is an error, not an unknown call:

<!-- example: 12_include_missing.clg -->
```cpp
#include "nothere.clh"

int main() {
    return 0;
}
```
<!-- error: 12_include_missing.clg -->
```
Error: Could not resolve #include "nothere.clh": no sibling source nothere.clg (a .clh include must have a .clg of the same name next to it)
```

An `#include` of anything else (`<cmath>`, `"shim.h"`) is copied into the C++ unread ([chapter 11](11-talking-to-cpp.md)). The cost of the design is that the library's source is re-parsed by every file that includes it, and
the compiler tracks no dependencies between builds (a script decides what to recompile); it is cheap at the sizes in use and is what a build script parallelises.

## Compiling the files one by one

Each `.clg` becomes a `.cpp` and a `.h`, each `.cpp` is compiled on its own, and the objects are linked, the way any C++ project is built:

```
cling 12_counter_lib.clg  --cpp 12_counter_lib.cpp
cling 12_counter_lib.clg  --cpp 12_counter_lib.h
cling 12_counter_main.clg --cpp 12_counter_main.cpp
g++ -std=c++17 12_counter_main.cpp 12_counter_lib.cpp -o program
```

The checker does exactly this for every example of this chapter. A real project does it with a script: app-cling's `scripts/build.sh` transpiles each module to its own `.cpp` and `.h` in parallel, compiles them in
parallel, and links them with the native layer in `shim/`.

## The safety checks are the same across files

Because the included file is merged, the dangling and use-after-move checks see its bodies and derive their summaries from them. A function in another file that takes its argument moves it; one that pushes
into a vector invalidates its observers:

<!-- example: 12_cross_lib.clg -->
```cpp
// A library file: no main.
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

class Holder {
public:
    std::vector<Node> items;
    Holder() { }
    void grow() { items.push_back(new Node(3)); }
};

class Job { public: Job() { } virtual void run() { } };

void eat(Node n) { }
void growAll(std::vector<Node> v) { }
```
<!-- example: 12_cross_use_after_move.clg -->
```cpp
#include "12_cross_lib.clh"

int main() {
    Node a = new Node(1);
    eat(a);                          // eat() is in the other file and takes its argument: a is gone
    std::cout << a.v << std::endl;
    return 0;
}
```
<!-- error: 12_cross_use_after_move.clg -->
```
error[semantic/use-after-move]: use of moved-from value `a`
  --> 12_cross_use_after_move.clg:6:18
  help: `a` was moved earlier; assign a new value before using it
```

<!-- example: 12_cross_container_moved.clg -->
```cpp
#include "12_cross_lib.clh"

int main() {
    std::vector<Node> v;
    v.push_back(new Node(1));
    ref Node o = v[0];
    growAll(v);                      // the vector is handed to the other file's function: o's owner is gone
    std::cout << o.v << std::endl;
    return 0;
}
```
<!-- error: 12_cross_container_moved.clg -->
```
error[semantic/use-after-move]: use of dangling observer `o` (its owner `v` is gone)
  --> 12_cross_container_moved.clg:8:18
  help: `o` observes `v`, which no longer has a live owner
```

and the check is not a blanket refusal: a correct use of the same library is accepted and runs.

<!-- example: 12_cross_accepted.clg -->
```cpp
#include "12_cross_lib.clh"

int main() {
    Node a = new Node(1);
    ref Node o = a;                  // observes a local; grow() cannot touch it
    Holder h = new Holder();
    h.grow();
    Node b = new Node(7);
    eat(b);
    std::cout << o.v << " " << 7 << std::endl;
    return 0;
}
```
<!-- output: 12_cross_accepted.clg -->
```
1 7
```

**Evidence, not proof.** `tests/separate/run_cross_file_test.sh` takes the 281 `tests/safety` programs that can be split, cuts each into a library file and a `main` file, and requires the same verdict: 124 rejects stay
rejects, 153 accepts stay accepts, none is lost. Four are rejected *only* when split, each listed with its reason in `tests/separate/split_equivalence.py`: a `spawn` of a class defined in another file, a `decodeX` outside the declaring file, and a pointer
hint (`//@ptr`) separated from its function. The first is the one to know. The thread rule needs the class to be closed, and a class in another file might have subclasses the compiler cannot see from here:

<!-- example: 12_cross_spawn.clg -->
```cpp
#include "12_cross_lib.clh"

int main() {
    Node a = new Node(1);
    spawn(closure Job [a] () { a.v = 2; });      // Node is defined in another file: its subclasses cannot be ruled out
    return 0;
}
```
<!-- error: 12_cross_spawn.clg -->
```
Error: error[semantic/spawn-captures-object]: `spawn` captures `a` of type `Node`, which is not a plain value (line 5, column 11). only plain values may cross into a new thread: primitives, string, std::vector of those, and `struct`s made only of those. They are copied, so no memory is shared with the creating thread. A class object (whatever pointer kind was inferred for it) may have another live owner here and may be a subclass declared elsewhere, so it is refused -- make the data a `struct`, or serialize it to a string first (e.g. printUnformatted(item)) and rebuild it inside the thread
```

The same program in one file is accepted ([chapter 10](10-threads.md)):

<!-- example: 12_cross_spawn_one_file.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

class Job { public: Job() { } virtual void run() { } };

int main() {
    Node a = new Node(1);
    spawn(closure Job [a] () {
        a.v = 2;
        std::cout << a.v << std::endl;
    });
    return 0;
}
```
<!-- output: 12_cross_spawn_one_file.clg -->
```
2
```

**Where it stops.** A C++ header, an `ext_fn` or a library on the other side of an include is still a trusted boundary ([chapter 11](11-talking-to-cpp.md)), and the gaps of [chapter 13](13-contract-and-threats.md) apply to many files as to one.

## Pointer kinds are decided per file

The pointer kind of a field, a container element or a method result (`unique_ptr` or `shared_ptr`, [chapter 4](04-ownership.md)) is chosen from how the class is *used*, and that is decided for one file at a time. The
header a file produces fixes the choice. So a file that uses a class can need `shared_ptr` where the file that declares it, blind to that use, chose `unique_ptr`; the two generated files then disagree and g++
refuses them. A bag of items, and a file that copies an element out of it twice:

<!-- example: 12_demand_lib.clg -->
```cpp
class Item {
public:
    int v;
    Item(int x) { v = x; }
    int get() { return v; }
};

class Bag {
public:
    std::vector<Item> items;
    Bag() { }
    void add(Item i) { items.push_back(i); }
};
```
<!-- example: 12_demand_use.clg -->
```cpp
#include "12_demand_lib.clh"

int total(Bag b) {
    Item first = b.items[0];       // two copies of an element: the element needs more than one owner
    Item again = b.items[0];
    return first.get() + again.get();
}

int main() {
    Bag b = new Bag();
    b.add(new Item(5));
    std::cout << total(b) << std::endl;
    return 0;
}
```

Compiled separately and without help, the pair fails:

<!-- cxxerror: 12_demand_use.clg -->
```
error: conversion from 'std::unique_ptr<Item>' to non-scalar type 'std::shared_ptr<Item>' requested
```

The fix needs no new syntax. `--ptr-demands-out FILE` makes a file write what it needs from the classes it includes; `--ptr-demands FILE` (repeatable) makes every file read such demands before its analysis. A demand
can only make a member shared, never the other way.

<!-- demands: 12_demand_use.clg -->
```
shared elements items
```

With the demands read by both files, the declaring file follows and the pair compiles, links and runs:

<!-- header: 12_demand_use.clg "class Bag {" -->
```cpp
class Bag {
public:
    std::vector<std::shared_ptr<Item>> items;
    Bag();
    void add(std::shared_ptr<Item> i);
};
```
<!-- output: 12_demand_use.clg -->
```
10
```

Only members the file really reaches are written, so a field that merely shares a name with another class's is not demanded. Making one member shared can make another needed, so a driver repeats the rounds until
the demands stop changing: app-cling's `scripts/pointer_demands.sh` compiles every module to write, merges, then compiles to read, a few rounds at most. Without either flag the output is byte for byte what it
was before. `tests/separate/run_demands_test.sh` (a CTest test) checks all three: the pair fails without demands, fits with them, and a demand that names nothing changes nothing.

**Where it stops.** This fixes the *fit* of the generated files, not the safety analysis, which is still per file. Splitting a program into modules can change which copies the emitter inserts (app-cling's split
removed one defensive `std::string(...)` copy in `Watches::flush`, because name-matched call chains collide less in a smaller file).

## Generated JSON decoders: `decodeX(item)`

A page or a client sends JSON, and reading it field by field with hand-written helpers is where mistakes live. A call to `decode<Name>(item)`, where `<Name>` is a `struct` declared in this file and nobody
declared a function `decode<Name>`, makes the compiler **write the decoder**. There is no new syntax: the text is generated as ordinary CeeLing and parsed, so every checker sees it.

**What it needs.** The names of cJSON-cling (`Item`, `isObject`, `isString`, `isNumber`, `isBool`, `isTrue`, `isArray`, `isNull`, `getObjectItem`, `getArraySize`, `getArrayItem`), visible through
`#include "cjson_api.clh"`. Nothing else, and not the whole library: the examples here use `12_mini_item.clg`, a stand-in of 40 lines with just those names and a way to build a document by hand, so they run
without cJSON-cling. One example below uses the real thing.

<!-- example: 12_mini_item.clg -->
```cpp
// A stand-in for cjson-cling: just the names a generated decoder uses, and helpers to build a document by hand.
// ---- a stand-in for cjson-cling: just the names the generated decoders use ----
class Item {
public:
    Item next;
    Item child;
    string key;
    string valuestring;
    double valuedouble;
    int type;
    Item() { type = 0; valuedouble = 0.0; key = ""; valuestring = ""; }
};
bool isObject(Item i) { return i != nullptr && i.type == 6; }
bool isNull(Item i) { return i != nullptr && i.type == 0; }
bool isArray(Item i) { return i != nullptr && i.type == 5; }
bool isString(Item i) { return i != nullptr && i.type == 4; }
bool isNumber(Item i) { return i != nullptr && i.type == 3; }
bool isBool(Item i) { return i != nullptr && (i.type == 1 || i.type == 2); }
bool isTrue(Item i) { return i != nullptr && i.type == 2; }
int getArraySize(Item a) {
    int n = 0;
    Item c = a.child;
    while (c != nullptr) { n = n + 1; c = c.next; }
    return n;
}
Item getArrayItem(Item a, int index) {
    Item c = a.child;
    int k = index;
    while (c != nullptr && k > 0) { c = c.next; k = k - 1; }
    return c;
}
Item getObjectItem(Item o, string name) {
    Item c = o.child;
    while (c != nullptr) { if (c.key == name) { return c; } c = c.next; }
    return nullptr;
}
// ---- building a document by hand ----
Item node(int type, string key) { Item i = new Item(); i.type = type; i.key = key; return i; }
Item num(string key, double v) { Item i = new Item(); i.type = 3; i.key = key; i.valuedouble = v; return i; }
Item str(string key, string v) { Item i = new Item(); i.type = 4; i.key = key; i.valuestring = v; return i; }
Item flag(string key, bool v) { Item i = new Item(); i.key = key; i.type = 1; if (v) { i.type = 2; } return i; }
void put(Item parent, Item child) {
    if (parent.child == nullptr) { parent.child = child; return; }
    Item last = parent.child;
    while (last.next != nullptr) { last = last.next; }
    last.next = child;
}
```

**What it decodes.** A field may be `int`, `int64`, `double`, `float`, `bool`, `string`, `byte`, a `std::vector` of those, a `struct`, or a `std::vector` of a struct. The struct needs a constructor whose
parameters are its fields in declaration order (the compiler has no automatic constructor; the error shows the one to add), and the decoder ends in `new Name(...)`. Extra keys are ignored.

<!-- example: 12_decode_basic.clg -->
```cpp
#include "12_mini_item.clh"

struct Pt {
public:
    int x;
    int y;
    Pt(int a, int b) { x = a; y = b; }
};

struct Args {
public:
    string root;
    int count;
    Pt origin;
    std::vector<int> nums;
    Args(string r, int c, Pt o, std::vector<int> n) { root = r; count = c; origin = o; nums = n; }
};

int main() {
    Item doc = node(6, "");
    put(doc, str("root", "/tmp"));
    put(doc, str("ignored", "a key Args does not have"));
    put(doc, num("count", 3.0));
    Item origin = node(6, "origin");
    put(origin, num("x", 1.0));
    put(origin, num("y", 2.0));
    put(doc, origin);
    Item nums = node(5, "nums");
    put(nums, num("", 5.0));
    put(nums, num("", 7.0));
    put(doc, nums);
    Args a = decodeArgs(doc);                 // never defined in this file: the compiler writes it
    std::cout << a.root << " " << a.count << " " << a.origin.y << " " << a.nums[0] << " " << a.nums[1] << " y " << a.nums.size() << std::endl;
    return 0;
}
```
<!-- output: 12_decode_basic.clg -->
```
/tmp 3 2 5 7 y 2
```
<!-- generated: 12_decode_basic.clg "Args decodeArgs(std::shared_ptr<Item> j) {" -->
```cpp
Args decodeArgs(std::shared_ptr<Item> j) {
    return decodeArgsAt(j, "");
}
```

**What it says when the JSON is wrong.** The JSON must be an object. A missing field or one of the wrong type throws `"invalid_argument: `path.field` must be a string"`, with the path (`o.x`) and the
index of an array element (`list[1]`); a missing field is reported as the wrong type it is (nothing is not a string). An `int`, `int64` or `byte` must be a whole number inside its range, checked *before* the C++
conversion, which would be undefined behaviour for `1e20`.

<!-- example: 12_decode_errors.clg -->
```cpp
#include "12_mini_item.clh"

struct Inner {
public:
    int x;
    Inner(int a) { x = a; }
};

struct Wide {
public:
    string root;
    int n;
    std::vector<int> list;
    Inner o;
    Wide(string r, int m, std::vector<int> l, Inner i) { root = r; n = m; list = l; o = i; }
};

void tryIt(Item doc) {
    try {
        Wide w = decodeWide(doc);
        std::cout << "ok" << std::endl;
    } catch (string message) {
        std::cout << message.substr(message.find("`")) << std::endl;
    }
}

Item good() {
    Item doc = node(6, "");
    put(doc, str("root", "r"));
    put(doc, num("n", 1.0));
    Item list = node(5, "list");
    put(list, num("", 1.0));
    put(doc, list);
    Item o = node(6, "o");
    put(o, num("x", 1.0));
    put(doc, o);
    return doc;
}

int main() {
    Item missing = node(6, "");                // nothing at all: `root` is missing, reported as the wrong type
    tryIt(missing);
    Item fraction = good();
    getObjectItem(fraction, "n").valuedouble = 1.5;       // not a whole number
    tryIt(fraction);
    Item badElement = good();
    put(getObjectItem(badElement, "list"), str("", "x"));  // the second element is a string
    tryIt(badElement);
    Item huge = good();
    getObjectItem(getObjectItem(huge, "o"), "x").valuedouble = 1e20;   // out of range for an int
    tryIt(huge);
    return 0;
}
```
<!-- output: 12_decode_errors.clg -->
```
`root` must be a string
`n` must be a whole number in range
`list[1]` must be a number
`o.x` must be a whole number in range
```

The message is built with `std::to_string`, which is on the allow-list for this: it turns a number into a fresh `string` and borrows nothing (CeeLing had no number-to-text function before).

<!-- example: 12_to_string.clg -->
```cpp
int main() {
    int i = 3;
    string message = "list[" + std::to_string(i) + "] must be a number";   // number to text; returns a fresh string
    std::cout << message << std::endl;
    return 0;
}
```
<!-- output: 12_to_string.clg -->
```
list[3] must be a number
```

**Optional fields.** A struct with a constructor that takes **no parameters** has every field optional. The decoder starts from `new X()` and overwrites the fields the object has (a JSON `null` counts as absent), so a
missing field keeps the value the constructor gave it; a present field must still have the right type. It is all-or-nothing per struct and implicit: giving a required-fields struct a default constructor for another
reason makes its fields optional.

<!-- example: 12_decode_optional.clg -->
```cpp
#include "12_mini_item.clh"

struct Opts {
public:
    string title;
    int width;
    Opts() {                          // a constructor with no parameters: every field is optional
        title = "untitled";
        width = 800;
    }
};

int main() {
    Item empty = node(6, "");
    Opts a = decodeOpts(empty);       // nothing present: the constructor's values stay
    Item some = node(6, "");
    put(some, str("title", "Doc"));
    put(some, num("width", 640.0));
    Opts b = decodeOpts(some);
    std::cout << a.title << " " << a.width << " | " << b.title << " " << b.width << std::endl;
    return 0;
}
```
<!-- output: 12_decode_optional.clg -->
```
untitled 800 | Doc 640
```

**With the real library.** The same decoder against cJSON-cling's parser (this example is built with that project's own `tests/build.sh`, so it needs cJSON-cling beside this repository):

<!-- example: 12_decode_cjson.clg -->
```cpp
#include "cjson_item.clh"
#include "cjson_api.clh"
#include "cjson_parse.clh"
#include "cjson_print.clh"

struct Pt {
public:
    int x;
    int y;
    Pt(int a, int b) { x = a; y = b; }
};

struct Args {
public:
    string root;
    int count;
    Pt origin;
    std::vector<string> tags;
    Args(string r, int c, Pt o, std::vector<string> t) { root = r; count = c; origin = o; tags = t; }
};

int main() {
    Args a = decodeArgs(parse("{\"root\":\"/tmp\",\"count\":3,\"origin\":{\"x\":1,\"y\":2},\"tags\":[\"pen\",\"ink\"],\"more\":true}"));
    std::cout << a.root << " " << a.count << " " << a.origin.y << " " << a.tags[0] << "," << a.tags[1] << std::endl;
    try {
        Args b = decodeArgs(parse("{\"root\":\"/tmp\",\"count\":2.5,\"origin\":{\"x\":1,\"y\":2},\"tags\":[]}"));
    } catch (string message) {
        std::cout << message << std::endl;
    }
    return 0;
}
```
<!-- output: 12_decode_cjson.clg -->
```
/tmp 3 2 pen,ink
invalid_argument: `count` must be a whole number in range
```

**What is rejected.** All at compile time:

<!-- example: 12_decode_no_constructor_rejected.clg -->
```cpp
#include "12_mini_item.clh"

struct Thing {
public:
    string a;
    int b;
};                                    // no constructor taking (a, b)

int main() {
    Item doc = node(6, "");
    Thing t = decodeThing(doc);
    return 0;
}
```
<!-- error: 12_decode_no_constructor_rejected.clg -->
```
Error: error[semantic/struct-json-constructor]: struct `Thing` has no constructor taking its fields in declaration order. the generated decoder builds it with `new Thing(...)`; add a constructor Thing(string a, int b)
```

<!-- example: 12_decode_field_rejected.clg -->
```cpp
#include "12_mini_item.clh"

struct Board {
public:
    std::vector<std::vector<int>> grid;      // a vector of vectors
    Board(std::vector<std::vector<int>> g) { grid = g; }
};

int main() {
    Item doc = node(6, "");
    Board b = decodeBoard(doc);
    return 0;
}
```
<!-- error: 12_decode_field_rejected.clg -->
```
Error: error[semantic/struct-json-field]: field `grid` of struct `Board` has type `std::vector<std::vector<int>>`, which a generated decoder cannot read. supported: int, int64, double, float, bool, string, byte, a struct, and std::vector of those (not nested vectors); read other data by hand from the Item instead
```

<!-- example: 12_decode_class_rejected.clg -->
```cpp
#include "12_mini_item.clh"

class Thing {                                // a class, not a struct
public:
    string a;
    Thing(string x) { a = x; }
};

int main() {
    Item doc = node(6, "");
    Thing t = decodeThing(doc);
    return 0;
}
```
<!-- error: 12_decode_class_rejected.clg -->
```
error[semantic/forbidden-construct]: call to unknown function/method `decodeThing`
  --> 12_decode_class_rejected.clg:11:15
  help: CeeLing has no borrow/ownership summary for anything it did not compile itself; unknown calls are rejected by default rather than silently trusted (the default-reject rule). Define `decodeThing` in CeeLing source, or call an existing CeeLing function/method instead.
```

<!-- example: 12_decode_without_item_rejected.clg -->
```cpp
struct Thing {
public:
    string a;
    Thing(string x) { a = x; }
};

class Item {
public:
    Item() { }
};

int main() {
    Item doc = new Item();             // an Item, but not one that has isObject, getObjectItem ...
    Thing t = decodeThing(doc);
    return 0;
}
```
<!-- error: 12_decode_without_item_rejected.clg -->
```
Error: error[semantic/struct-json-needs-cjson]: generated struct decoders need cjson-cling, which is not visible here (missing: isObject, isString, isNumber, isBool, isTrue, isArray, getObjectItem, getArraySize, getArrayItem). add #include "cjson_api.clh" (and cjson_item.clh)
```

A class, a base struct, a namespaced struct, a `char` and a vector of vectors are refused as well; a function you write yourself named `decodeName` always wins.

**One file only.** The decoder is written in the file that declares the struct, when something there calls it. A file that merely includes the struct has the declaration but no body to link to, and says so:

<!-- example: 12_decode_bare_types.clg -->
```cpp
#include "12_mini_item.clh"

struct Args {
public:
    string root;
    Args(string r) { root = r; }
};                                     // declares the struct, but nothing here calls decodeArgs
```
<!-- example: 12_decode_other_file_rejected.clg -->
```cpp
#include "12_decode_bare_types.clh"

int main() {
    Item doc = node(6, "");
    put(doc, str("root", "/tmp"));
    Args a = decodeArgs(doc);
    return 0;
}
```
<!-- error: 12_decode_other_file_rejected.clg -->
```
Error: error[semantic/struct-json-external]: `decodeArgs` is called, but struct `Args` is declared in another file. the decoder is generated in the file that declares the struct (call decodeArgs there, or declare a function `Args decodeArgs(Item j)` that calls it)
```

The fix is the one the message names: a function next to the struct that calls the decoder, which the rest of the program calls instead. app-cling does this for every argument struct
("a struct is decoded in the file that declares it: other files call this"):

<!-- example: 12_decode_types.clg -->
```cpp
#include "12_mini_item.clh"

struct Args {
public:
    string root;
    Args(string r) { root = r; }
};

Args parseArgs(Item j) {
    return decodeArgs(j);              // the decoder is written here, in the file that declares Args
}
```
<!-- example: 12_decode_wrapper.clg -->
```cpp
#include "12_decode_types.clh"

int main() {
    Item doc = node(6, "");
    put(doc, str("root", "/tmp"));
    Args a = parseArgs(doc);           // call the wrapper that lives next to the struct
    std::cout << a.root << std::endl;
    return 0;
}
```
<!-- output: 12_decode_wrapper.clg -->
```
/tmp
```

<!-- source: app-cling/src/dialogs.clg "DialogArgs readDialogArgs" -->
```
DialogArgs readDialogArgs(Item args) {
    return decodeDialogArgs(args);
}
```

**What it does not do.** Validate meaning (a path, a length), produce JSON (`toJson` is open), or decode into classes. The result holds plain values only, so it can be captured by a `spawn`
([chapter 10](10-threads.md)).

## What needs which other project

| Feature | Needs |
|---|---|
| `#include "x.clh"`, `--cpp x.h`, the pointer-demand flags | only the compiler |
| `decodeX(item)` | the names of cJSON-cling (or a stand-in that defines them, as `12_mini_item.clg` does) |
| `12_decode_cjson.clg` and the cJSON examples of [chapter 15](15-case-study-cjson.md) | the cJSON-cling project, built by its `tests/build.sh` |
| `scripts/pointer_demands.sh` and `scripts/build.sh` | app-cling, as drivers for a many-module project |
