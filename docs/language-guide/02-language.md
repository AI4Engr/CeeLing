# 2. The language

CeeLing looks like Java written with C++ punctuation. This chapter is the syntax tour, and most of it is about one thing: **what a variable of an object type is**, because that is where
CeeLing differs from C++ and from Java at the same time. After that come classes, inheritance, operators, control flow, errors and the generic syntax, and at the end a list of what the
language does not have, each checked against the real compiler.

Every program in this chapter is compiled by the real compiler each time the guide is checked. The C++ twins compile with `g++ -Wall -Wextra`; the header of each says whether g++ speaks
up. Ownership is [chapter 4](04-ownership.md), handing objects over is [chapter 5](05-moves.md), dangling references are [chapter 6](06-dangling-references.md), the run-time checks are
[chapter 7](07-run-time-checks.md), numbers, strings and containers are [chapter 3](03-values-and-containers.md), closures are [chapter 9](09-closures.md).

## A program

A program is a list of classes, structs, enums, namespaces and free functions, and a `main`. There are no file-scope variables. `main` is `int main()` and nothing else: no arguments.
Its return value is the exit code. Output is `std::cout << ... << std::endl`. `string` is the string type (`std::string` also works). Comments start with `//`; block comments do not exist.

<!-- example: 02_program.clg -->
```cpp
#include <iostream>

int area(int width, int height) {
    return width * height;
}

string greet(string name) {
    return "hello, " + name;
}

int main() {
    int w = 3;
    int h = 4;
    std::cout << "area of " << w << " x " << h << " = " << area(w, h) << std::endl;
    std::cout << "greeting: " << greet("CeeLing") << std::endl;
    return 0;
}
```
<!-- output: 02_program.clg -->
```
area of 3 x 4 = 12
greeting: hello, CeeLing
```

The `#include` lines are optional for the standard library: the generated C++ includes what it needs. They matter for your own `.clh` headers ([chapter 12](12-many-files.md)).

**Two compilers check your program.** CeeLing checks what it exists for: dangling references, use after move, the sealed escape hatches, and which calls are known. It does *not* do ordinary
type checking. The generated C++ goes to g++, and g++ reports a wrong argument count (`add(1)` for `add(int, int)`), a wrong type (`string s = 5;`), an undeclared name or a redeclared
variable, in C++ terms and with C++ line numbers. Two consequences to know early. A **free function must be defined above its first use** (there is no forward declaration, so two free
functions cannot call each other; methods of one class can, see "Classes that refer to each other"). And a few mistakes that CeeLing lets through are listed under "Where it stops" in the
sections where they occur.

## An object variable is a handle

In C++ a variable of class type is a value, and getting something that behaves like Java's reference means choosing a pointer type at every declaration and keeping the choices
consistent. The three spellings below compile without a warning, and nothing in the types says which one a line means:

<!-- example: 02_handle.cpp -->
```cpp
// Three ways to hold "a Node" in C++, and nothing in the type says which one a given line means.
// The raw-pointer version below never calls delete: it compiles without a warning and leaks.
#include <iostream>

struct Node {
    int v;
    explicit Node(int x) : v(x) {}
};

int main() {
    Node a(1);
    Node b = a;                 // a copy: changing b does not change a
    b.v = 5;
    std::cout << a.v << b.v << std::endl;

    Node* p = new Node(1);      // a pointer: spelled differently, never freed here
    Node* q = p;                // an alias of the same object
    q->v = 5;
    std::cout << p->v << std::endl;
    return 0;
}
```

**Why C++ does not stop this.** Value, raw pointer, `unique_ptr` and `shared_ptr` are all legal ways to hold "a Node"; which one is right is a design fact the compiler cannot see. The raw
pointer above is never deleted. The compiler has no idea it should have been.

**What CeeLing does.** An object variable is a **handle**: it names an object that lives on the heap. `Node a = new Node(1);` creates the object and makes `a` its owner. `Node b = a;` does
not copy the object; `b` is a second handle to the same one. `.` reaches members through the handle, whatever kind of smart pointer the compiler picked.

<!-- example: 02_handle.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

void bump(Node n) {
    n.v = n.v + 1;
}

int main() {
    Node a = new Node(1);      // a names an object on the heap; it is not a copy of one
    Node b = a;                // b names the SAME object
    b.v = 5;
    std::cout << a.v << std::endl;
    bump(a);
    std::cout << b.v << std::endl;
    return 0;
}
```
<!-- output: 02_handle.clg -->
```
5
6
```

Both lines show the same object: `b.v = 5` changed what `a` sees, and `bump(a)` changed what `b` sees. This is Java's reading of a class-typed variable, and it is the only one:
there is no way to say "copy this object" other than building a new one (or using a `struct`, below). The compiler writes the pointer type, the `make_unique`/`make_shared`, the moves and the
`->`:

<!-- generated: 02_handle.clg "int main()" -->
```cpp
int main() {
    std::shared_ptr<Node> a = std::make_shared<Node>(1);
    std::shared_ptr<Node> b = a;
    cling::nn(b, "line 14")->v = 5;
    std::cout << cling::nn(a, "line 15")->v << std::endl;
    bump(a);
    std::cout << cling::nn(b, "line 17")->v << std::endl;
    return 0;
}
```

(`cling::nn` is the null check, below. Because `a` and `b` are two owners, the compiler chose `shared_ptr`. With one owner it chooses `unique_ptr`.)

**Where it stops.** The handle is not a C++ pointer you can do arithmetic on, take the address of or delete; section "Pointers that do not exist" lists what is gone. The inference that picks
`unique_ptr`/`shared_ptr`/`weak_ptr` is [chapter 4](04-ownership.md); it can say no (ambiguous ownership is an error, not a guess).

## What the compiler turns a field into

A field of object type is declared the same way a local is, with no pointer type. The compiler picks one per field from how the program uses it: a field only one thing ever owns becomes
`unique_ptr`, a field whose object is also held elsewhere becomes `shared_ptr`, and a back link (a field that would close an ownership cycle) becomes `weak_ptr`.

<!-- example: 02_field_kinds.clg -->
```cpp
class Tree {
public:
    int v;
    Tree left;
    Tree right;
    Tree parent;
    Tree(int x) { v = x; }
    void attachLeft(Tree child) {
        left = child;
        child.parent = this;
    }
};

class Book {
public:
    int pages;
    Book(int p) { pages = p; }
};

class Shelf {
public:
    Book favourite;
    Book latest;
    Shelf() { }
    void add(Book b) {
        favourite = b;
        latest = b;
    }
};

int main() {
    Tree root = new Tree(3);
    root.attachLeft(new Tree(2));
    Shelf s = new Shelf();
    s.add(new Book(7));
    std::cout << root.v << " " << s.favourite.pages << " " << root.left.v << std::endl;
    return 0;
}
```
<!-- output: 02_field_kinds.clg -->
```
3 7 2
```

<!-- generated: 02_field_kinds.clg "class Tree :" -->
```cpp
class Tree : public std::enable_shared_from_this<Tree> {
public:
    int v{};
    std::shared_ptr<Tree> left;
    std::unique_ptr<Tree> right;
    std::weak_ptr<Tree> parent;
    Tree(int x);
    void attachLeft(std::shared_ptr<Tree> child);
    ~Tree();
};
```

`left` and `right` are the same declaration in the source and different types in the output, because `attachLeft` stores its parameter and so shares it, while nothing shares `right`.
`parent` is `weak_ptr` because `child.parent = this` points back at an owner. A class that stores `this` or returns `this` (`Account self() { return this; }`) is given
`std::enable_shared_from_this`, and `return this` becomes `shared_from_this()`. Handing `this` out *inside a constructor* is rejected, because no `shared_ptr` owns the object yet:

<!-- example: 02_this_in_constructor.cpp -->
```cpp
// Handing out a shared_ptr to "this" from inside the constructor compiles without a warning and throws
// std::bad_weak_ptr when it runs: no shared_ptr owns the object yet. (Not run by the guide.)
#include <memory>

struct Registry {
    void add(std::shared_ptr<struct Widget>) {}
};

struct Widget : std::enable_shared_from_this<Widget> {
    Widget() {
        Registry reg;
        reg.add(shared_from_this());
    }
};

int main() {
    auto w = std::make_shared<Widget>();
    return 0;
}
```
<!-- example: 02_this_in_constructor_rejected.clg -->
```cpp
class Registry {
public:
    Registry() { }
    void add(Registry r) { }
};

class Widget {
public:
    Widget() {
        Registry reg = new Registry();
        reg.add(this);                 // the object is not yet held by anyone
    }
};

int main() {
    Widget w = new Widget();
    return 0;
}
```
<!-- error: 02_this_in_constructor_rejected.clg -->
```
error[semantic/forbidden-construct]: `this` passed as an argument from a constructor
  --> 02_this_in_constructor_rejected.clg:11:9
  help: passing `this` lowers to shared_from_this(), which throws std::bad_weak_ptr unconditionally when called before the object's shared_ptr control block exists -- and inside a constructor, it never does yet. Move this to after construction (e.g. a separate init() method called once the object is already held by a shared_ptr).
```

**Where it stops.** What the rules are, and when the compiler gives up, is [chapter 4](04-ownership.md). A weak field is read through `lock()` and null-checked like any other handle. A class
with a long chain of its own type also gets a destructor, below.

## Empty handles: `nullptr`, `!`, and the null check

An object field starts **empty**; `nullptr` is the empty handle; a handle can be tested with `== nullptr`, `!= nullptr` or `!`, and emptied again by assigning `nullptr`. A local of object
type must be initialised (`Node n;` is a syntax error), but a local `std::vector` or `std::map` may be declared bare, as an empty container.

<!-- example: 02_empty_field.clg -->
```cpp
class Item {
public:
    int v;
    Item next;                          // a field of object type starts empty
    Item(int x) { v = x; }
};

int main() {
    Item first = new Item(1);
    std::cout << "first has no next: " << (first.next == nullptr) << std::endl;

    Item second = new Item(2);
    Item third = new Item(3);
    third.next = second;
    second.next = first;
    Item cursor = third;
    while (cursor != nullptr) {
        std::cout << cursor.v;
        cursor = cursor.next;
        if (cursor != nullptr) { std::cout << " "; }
    }
    std::cout << std::endl;

    second.next = nullptr;              // empty it again
    std::cout << "cut: " << (!second.next) << std::endl;
    return 0;
}
```
<!-- output: 02_empty_field.clg -->
```
first has no next: 1
3 2 1
cut: 1
```

The loop ends at the empty `next`, which is how a list is walked without any null being dereferenced.

**The C++ problem.** A raw pointer field has no default. Unless every constructor sets it, it holds whatever the memory held, and testing it against `nullptr` is already undefined behaviour.
The following compiles with no warning (the guide does not run it):

<!-- example: 02_empty_field.cpp -->
```cpp
// A pointer field nobody initialised holds whatever was in that memory. Testing it against nullptr
// then reads an indeterminate value (undefined behaviour); the program below is not run by the guide.
#include <iostream>

struct Item {
    int v;
    Item* next;                    // the constructor forgets it
    explicit Item(int x) : v(x) {}
};

int main() {
    Item* a = new Item(1);
    if (a->next == nullptr) {      // may be true, false, or a crash
        std::cout << "last" << std::endl;
    }
    delete a;
    return 0;
}
```

An empty `unique_ptr` has the opposite trap: it is well defined to test, and `->` on it is undefined. **Why C++ does not stop either:** the pointer's type is the same whether it is empty or
full, and the language defines no check on `->`.

**What CeeLing does.** An object-typed field is always created empty. Every member access through a handle (`.` in your source, `->` in the output) goes through `cling::nn`, which stops the
program and names the `.clg` line if the handle is empty:

<!-- example: 02_null_deref.clg -->
```cpp
class Item {
public:
    int v;
    Item next;
    Item(int x) { v = x; }
    int nextValue() { return next.v; }
};

int main() {
    Item a = new Item(1);
    Item b = new Item(2);
    a.next = b;
    std::cout << a.nextValue() << std::endl;
    std::cout << b.nextValue() << std::endl;       // b has no next: the program stops here
    return 0;
}
```
<!-- output: 02_null_deref.clg -->
```
2
```

The program printed `2` and then stopped at line 8, the access `next.v` inside `nextValue`, because `b` has no `next`. It aborts; it does not throw, so `catch` cannot swallow it
([Errors](#errors-throw-and-catch), below).

**Where it stops.** Every dereference is checked at run time; nothing is proved non-null statically, so the check costs a branch each. The check finds an *empty* handle, not a `ref`
observer whose object is gone (that is the static analysis, chapter 6). It does not cover `this` or a handle handed to an `ext_fn`. It can be switched off per build with
`--no-null-checks`, after which a claim about the program is a claim about how it was built ([chapter 7](07-run-time-checks.md)).

A primitive field is never left uninitialised either: `int`, `double`, `bool` and `char` fields start at zero (the generated C++ writes `int count{};`), so a constructor that forgets one
leaves 0, not garbage. Locals are protected the other way: `int n;` is rejected ([chapter 8](08-sealed-escape-hatches.md)).

## Object identity: `==` and `!=`

Between two object handles, `==` and `!=` ask "is this the same object?". Equal fields are not enough; a comparison reads both handles and moves nothing.

<!-- example: 02_identity.clg -->
```cpp
class Point {
public:
    int x;
    Point(int v) { x = v; }
};

int main() {
    Point a = new Point(1);
    Point alias = a;
    Point other = new Point(1);
    Point otherAlias = other;
    std::cout << "same object: " << (a == alias) << std::endl;
    std::cout << "equal fields, different objects: " << (a == other) << std::endl;
    std::cout << "not the same: " << (a != other) << std::endl;
    std::cout << "still usable: " << a.x << " " << other.x << std::endl;
    return 0;
}
```
<!-- output: 02_identity.clg -->
```
same object: 1
equal fields, different objects: 0
not the same: 1
still usable: 1 1
```

**The C++ problem.** With smart pointers, `a == b` compares addresses and `*a == *b` compares values (if someone wrote an `operator==`). Both compile, they differ by one character, and
neither warns:

<!-- example: 02_identity.cpp -->
```cpp
// With smart pointers, == on two handles compares ADDRESSES; comparing the objects needs a dereference
// and an operator== somebody wrote. Both spellings compile, and they look almost the same.
#include <iostream>
#include <memory>

struct Point {
    int x;
    explicit Point(int v) : x(v) {}
    bool operator==(const Point& o) const { return x == o.x; }
};

int main() {
    auto a = std::make_shared<Point>(1);
    auto b = std::make_shared<Point>(1);
    std::cout << (a == b) << std::endl;      // 0: different objects
    std::cout << (*a == *b) << std::endl;    // 1: equal values
    return 0;
}
```

**What CeeLing does.** A handle comparison is always identity, because there is no `*` to put in front of a handle. If you want value equality, write a method and call it. If your class
defines `bool operator==(Money other)`, then `==` on its handles calls that instead (section "Operators").

**Where it stops.** Identity comparison of two handles of different inferred pointer kinds can fail in g++: when one of the two had to become `shared_ptr` (because it is aliased) and the
other stayed `unique_ptr`, the generated `a == b` is `shared_ptr == unique_ptr`, which does not exist in C++. CeeLing accepts the program, g++ rejects the output. Make both objects
aliased, or compare a field of each. Comparing two `struct` values with `==` fails the same way (a struct has no `operator==` unless you write one).

## `ref`: looking at an object without owning it

`ref Counter view = c;` makes `view` an **observer**: it sees the object `c` owns, and does not keep it alive. It is the only reference-like thing in the language, it is local to a
function, and it is the subject of [chapter 6](06-dangling-references.md). Here only the syntax:

<!-- example: 02_ref_observer.clg -->
```cpp
class Counter {
public:
    int n;
    Counter() { n = 0; }
};

int main() {
    Counter c = new Counter();
    ref Counter view = c;        // view looks at c's object; it does not own it
    view.n = 7;
    std::cout << c.n << " " << view.n << std::endl;
    return 0;
}
```
<!-- output: 02_ref_observer.clg -->
```
7 7
```

<!-- generated: 02_ref_observer.clg "int main()" -->
```cpp
int main() {
    std::unique_ptr<Counter> c = std::make_unique<Counter>();
    Counter* view = (c).get();
    cling::nn(view, "line 10")->n = 7;
    std::cout << cling::nn(c, "line 11")->n << " " << cling::nn(view, "line 11")->n << std::endl;
    return 0;
}
```

An observer is a raw pointer in the generated C++. That is exactly the C++ hazard: a reference to an object that its owner can destroy.

<!-- example: 02_ref_dangles.cpp -->
```cpp
// A C++ reference to an object owned by a unique_ptr: replacing the owner destroys the object,
// and the reference still compiles, with no warning. Reading it is undefined behaviour (not run by the guide).
#include <iostream>
#include <memory>

struct Counter {
    int n = 0;
};

int main() {
    auto c = std::make_unique<Counter>();
    Counter& view = *c;
    c = std::make_unique<Counter>();       // the old object is destroyed here
    std::cout << view.n << std::endl;      // reads a destroyed object
    return 0;
}
```

**Why C++ does not stop it.** `Counter&` has no lifetime. Replacing the `unique_ptr` is an ordinary assignment, and nothing connects it to the reference.

**What CeeLing does.** It records that `view` observes `c`, and that assigning a new object to `c` ends the old one:

<!-- example: 02_ref_dangles.clg -->
```cpp
class Counter {
public:
    int n;
    Counter() { n = 0; }
};

int main() {
    Counter c = new Counter();
    ref Counter view = c;
    c = new Counter();           // the object view looks at is destroyed here
    std::cout << view.n << std::endl;
    return 0;
}
```
<!-- error: 02_ref_dangles.clg -->
```
error[semantic/use-after-move]: use of dangling observer `view` (its owner `c` is gone)
  --> 02_ref_dangles.clg:11:18
  help: `view` observes `c`, which no longer has a live owner
```

**Where it stops.** `ref` is allowed only on locals. It cannot be a parameter, a field or a return type (listed at the end of the chapter), which is how an observer can never outlive its
function. Many cases besides this one are in chapter 6, with the ones not yet seen.

## Giving an object away: assignment and arguments

Assignment `b = a` between two object variables makes `b` another owner. **Passing an object to a function hands it over**: afterwards the compiler treats the caller's variable as empty
until you assign it a new value, and rejects any use in between. When a second owner exists the object stays reachable through it; the rule of exactly when an argument is moved and when it is
shared is [chapter 5](05-moves.md).

<!-- example: 02_pass_hands_over.clg -->
```cpp
class Ticket {
public:
    int number;
    Ticket(int n) { number = n; }
};

void archive(Ticket t) { }

int main() {
    Ticket ticket = new Ticket(7);
    archive(ticket);                       // the object now belongs to archive's parameter
    std::cout << ticket.number << std::endl;
    return 0;
}
```
<!-- error: 02_pass_hands_over.clg -->
```
error[semantic/use-after-move]: use of moved-from value `ticket`
  --> 02_pass_hands_over.clg:12:18
  help: `ticket` was moved earlier; assign a new value before using it
```

Giving the variable a new object makes it usable again:

<!-- example: 02_pass_reassign.clg -->
```cpp
class Ticket {
public:
    int number;
    Ticket(int n) { number = n; }
};

void archive(Ticket t) {
    std::cout << "archived " << t.number << std::endl;
}

int main() {
    Ticket ticket = new Ticket(7);
    archive(ticket);                       // handed over: ticket is empty for the compiler
    ticket = new Ticket(8);                // a new object makes the name usable again
    std::cout << "next ticket " << ticket.number << std::endl;
    return 0;
}
```
<!-- output: 02_pass_reassign.clg -->
```
archived 7
next ticket 8
```

You never write `std::move`; the compiler writes it where the last use is, and the tracking above is what makes that safe ([chapter 1, section 3](01-why-ceeling.md#3-using-an-object-after-it-was-moved)).

**Where it stops.** The compiler's rule is checked per function body; what a callee does with the object is summarised, not re-analysed (chapter 5). There are no reference parameters (see the
end of the chapter), so a callee cannot be asked to "just look".

## `struct`: a value, not a handle

`struct` is a class that is a plain value: stored directly, copied on assignment, never behind a smart pointer. The choice is explicit; the compiler does not guess from the fields.

<!-- example: 02_struct_value.clg -->
```cpp
struct Point {
public:
    int x;
    Point(int v) { x = v; }
};

class Cell {
public:
    int x;
    Cell(int v) { x = v; }
};

void moveRight(Point p) {
    p.x = p.x + 100;
}

int main() {
    Point a = new Point(1);
    Point b = a;                 // a copy
    b.x = 9;
    std::cout << "struct: " << a.x << " " << b.x << std::endl;

    Cell c = new Cell(1);
    Cell d = c;                  // the same object
    d.x = 9;
    std::cout << "class:  " << c.x << " " << d.x << std::endl;

    Point p = new Point(1);
    moveRight(p);
    std::cout << "passed a struct, caller still has " << p.x << std::endl;
    return 0;
}
```
<!-- output: 02_struct_value.clg -->
```
struct: 1 9
class:  9 9
passed a struct, caller still has 1
```

<!-- generated: 02_struct_value.clg "int main()" -->
```cpp
int main() {
    Point a = Point(1);
    Point b = a;
    b.x = 9;
    std::cout << "struct: " << a.x << " " << b.x << std::endl;
    std::shared_ptr<Cell> c = std::make_shared<Cell>(1);
    std::shared_ptr<Cell> d = c;
    cling::nn(d, "line 25")->x = 9;
    std::cout << "class:  " << cling::nn(c, "line 26")->x << " " << cling::nn(d, "line 26")->x << std::endl;
    Point p = Point(1);
    moveRight(p);
    std::cout << "passed a struct, caller still has " << p.x << std::endl;
    return 0;
}
```

`Point b = a;` is a copy, so changing `b` leaves `a` alone; `Cell d = c;` is another handle, so changing `d` changes `c`. A struct passed to a function is copied, and the callee changes its own
copy. Structs may have a base struct, methods and constructors, and a `std::vector` of structs holds copies:

<!-- example: 02_struct_base.clg -->
```cpp
struct Point {
public:
    int x;
    int y;
    Point(int px, int py) { x = px; y = py; }
    int sum() { return x + y; }
};

struct Point3 : public Point {
public:
    int z;
    Point3(int px, int py, int pz) { x = px; y = py; z = pz; }
};

int main() {
    Point3 q = new Point3(1, 2, 3);
    std::cout << q.sum() + q.z << std::endl;
    std::vector<Point> pts;
    pts.push_back(new Point(1, 2));
    Point first = pts[0];                // a copy of the element
    first.x = 55;
    std::cout << pts[0].x << " " << first.x << " " << pts.size() << std::endl;
    return 0;
}
```
<!-- output: 02_struct_base.clg -->
```
6
1 55 1
```

A struct cannot be observed with `ref`, because there is no single object to look at:

<!-- example: 02_struct_ref_rejected.clg -->
```cpp
struct Point {
public:
    int x;
    Point(int v) { x = v; }
};

int main() {
    Point a = new Point(1);
    ref Point r = a;           // there is no single object to look at: a is the value itself
    return 0;
}
```
<!-- error: 02_struct_ref_rejected.clg -->
```
error[semantic/use-after-move]: `ref` on value type `Point` (`r`) has no owner to observe
  --> 02_struct_ref_rejected.clg:9:5
  help: `Point` is a value type (copied, not smart-pointer-backed); CeeLing cannot form an observer of it. Use a plain copy instead: `Point r = ...;`
```

The rest of the C++ choice remains yours in one place: a struct field of struct type is held by value, so it must be built in the member initialiser (the next section).

**Where it stops.** A struct is not immune to the move rules: handing one to a function can still end its use in the caller (chapter 5). It has no empty state (`Point p = nullptr;` is accepted
by CeeLing and rejected by g++), no `==`, and it must contain only what it can hold by value. A struct can be captured by a `spawn` job because it is plain data ([chapter 10](10-threads.md)).

## Pointers that do not exist

Everything C++ does with a raw pointer has no spelling. This is deliberate: one raw pointer anywhere would void the guarantee ([chapter 8](08-sealed-escape-hatches.md)).

| C++ | CeeLing |
|---|---|
| `Node* p = &a;` | no address-of, no pointer types |
| `Node* next;` (field, parameter, return) | no pointer types anywhere |
| `delete p;` | `delete` is rejected; the compiler decides when an object dies |
| `p->v`, `*p` | `->` is accepted as another spelling of `.`; there is no `*` on a handle |
| `p + 1` | no pointer arithmetic |
| `new int[10]` | `std::vector<int>` ([chapter 3](03-values-and-containers.md)) |
| `unique_ptr<T>`, `shared_ptr<T>`, `weak_ptr<T>`, `.get()` | written for you; `.get()` is rejected |

Each rejection, with the compiler's own words. The first two are plain syntax errors: the grammar has no production for them.

<!-- example: 02_no_address_of.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

int main() {
    Node a = new Node(1);
    Node b = &a;               // there is no address-of operator
    return 0;
}
```
<!-- error: 02_no_address_of.clg -->
```
Error: Syntax error at line 9, column 13, node type 'this_expression'
```
<!-- example: 02_no_pointer_field.clg -->
```cpp
class Node {
public:
    int v;
    Node * next;               // a pointer field: no such declaration
    Node(int x) { v = x; }
};

int main() {
    return 0;
}
```
<!-- error: 02_no_pointer_field.clg -->
```
Error: Syntax error at line 1, column 1, node type 'ERROR', near: class Node {
public:
    int v;
    Node
```
<!-- example: 02_no_delete.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

int main() {
    Node a = new Node(1);
    delete a;                  // the compiler decides when an object dies
    return 0;
}
```
<!-- error: 02_no_delete.clg -->
```
Error: Syntax error at line 9, column 5, node type 'ERROR', near: delete a; Suggestion: raw `delete` is forbidden (hatch #15) -- object lifetime is managed automatically by the compiler-chosen smart pointer; there is no manual delete.
```

The arrow is accepted, and means the same as the dot:

<!-- example: 02_arrow_is_dot.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

int main() {
    Node n = new Node(2);
    std::cout << n.v << " "
              << n->v << std::endl;        // -> is accepted as another spelling of .
    return 0;
}
```
<!-- output: 02_arrow_is_dot.clg -->
```
2 2
```

**Where it stops.** Two pointer-looking spellings get past CeeLing and are stopped by g++ instead: `*a` on a class that has no `operator*`, and `a + 1` on a handle. The C++ output does not
compile, so no program with them runs, but the message is g++'s. `auto` is accepted for numbers (`auto n = 5;`) and breaks for objects (`auto a = new Node(3);` generates C++ that g++
rejects); write the type.

## Classes

A class has fields, constructors and methods, in any order, after a single `public:`. There is no `private:` or `protected:` (a syntax error), so every member is public; there is no field
initialiser (`int x = 5;`); there are no static members. A class with no constructor gets C++'s implicit one.

<!-- example: 02_class_basics.clg -->
```cpp
class Account {
public:
    int balance;
    int operations;
    Account(int start) {
        balance = start;
        operations = 0;
    }
    void deposit(int amount) {
        balance = balance + amount;
        count();
    }
    void withdraw(int amount) {
        this.balance = this.balance - amount;      // this.x and plain x mean the same field
        count();
    }
    void count() { operations = operations + 1; }
    Account self() { return this; }
};

int main() {
    Account acct = new Account(100);
    acct.deposit(50);
    std::cout << "balance " << acct.balance << std::endl;
    acct.withdraw(20);
    std::cout << "balance " << acct.balance << " after " << acct.operations << " operations" << std::endl;
    Account again = acct.self();
    std::cout << "same object back: " << (again == acct) << std::endl;
    return 0;
}
```
<!-- output: 02_class_basics.clg -->
```
balance 150
balance 130 after 2 operations
same object back: 1
```

`this` is a handle to the current object; `this.balance` and `balance` are the same field. Methods may call each other in any order, and a method may return `this`
(`Account self()`). A constructor can be overloaded by parameter count and types, as can methods and free functions; C++ picks the right one:

<!-- example: 02_ctor_overloads.clg -->
```cpp
class Label {
public:
    string text;
    int n;
    Label() { text = "none"; n = 0; }
    Label(int x) { text = "one"; n = x; }
    Label(int x, int y) { text = "two"; n = x + y; }
    string show() { return text + "/" + std::to_string(n); }
};

int main() {
    Label a = new Label();
    Label b = new Label(1);
    Label c = new Label(1, 2);
    std::cout << a.show() << " " << b.show() << " " << c.show() << std::endl;
    return 0;
}
```
<!-- output: 02_ctor_overloads.clg -->
```
none/0 one/1 two/3
```

<!-- example: 02_free_overload.clg -->
```cpp
int pick(int a) { return 1; }
int pick(int a, int b) { return 2; }
string pick(string s) { return s + "!"; }

int main() {
    std::cout << pick(5) << " " << pick(1, 2) << " " << pick("s") << std::endl;
    return 0;
}
```
<!-- output: 02_free_overload.clg -->
```
1 2 s!
```

(`new Label(1, 2, 3)` with no such constructor is not rejected by CeeLing; g++ rejects the generated code.)

A call may be made on the result of another call to your own code, of any depth:

<!-- example: 02_chain_on_own_result.clg -->
```cpp
class Inner {
public:
    int v;
    Inner(int val) { v = val; }
};

class Holder {
public:
    Inner inner;
    Holder(int val) { inner = new Inner(val); }
    Inner getInner() { return inner; }
};

Holder makeHolder() {
    return new Holder(7);
}

int main() {
    int v = makeHolder().getInner().v;       // a call on a call's result works for your own functions and methods
    std::cout << v << std::endl;
    return 0;
}
```
<!-- output: 02_chain_on_own_result.clg -->
```
7
```

## Constructors: what order fields are set in

In C++ the members of a class are initialised in the order they are *declared*, whatever order the constructor's initialiser list is written in. Write it in a different order and, if one
initialiser reads another member, you read memory that does not exist yet. g++ warns here (`-Wreorder`, `-Wmaybe-uninitialized`); on the guide's toolchain the program below dies with `std::bad_alloc`:

<!-- example: 02_ctor_order.cpp -->
```cpp
// A member initializer list runs in DECLARATION order, whatever order it is written in. Here b is
// initialized first and reads a before a exists. g++ -Wall warns (-Wreorder, -Wmaybe-uninitialized);
// the program is not run by the guide (on the guide's toolchain it died with std::bad_alloc).
#include <iostream>
#include <string>

struct Pair {
    std::string b;
    std::string a;
    Pair() : a("x"), b(a + "y") {}
};

int main() {
    Pair p;
    std::cout << p.b << std::endl;
    return 0;
}
```

In CeeLing a constructor is a body of ordinary statements, run in the order written. The compiler may move leading plain assignments into C++'s initialiser list (it must, for a field
whose type has no default constructor, see below), but only under rules that keep the written order: it stops at the first assignment that is out of declaration order, at the first
statement that is not a plain assignment to the class's own field (a call, say), and an initialiser may read only parameters and fields hoisted before it.

<!-- example: 02_ctor_order.clg -->
```cpp
class Pair {
public:
    string b;                  // declared first ...
    string a;
    Pair() {
        a = "x";               // ... but assigned second
        b = a + "y";
    }
};

int main() {
    Pair p = new Pair();
    std::cout << p.b << std::endl;
    return 0;
}
```
<!-- output: 02_ctor_order.clg -->
```
xy
```

<!-- generated: 02_ctor_order.clg "Pair::Pair()" -->
```cpp
Pair::Pair() : a("x") {
    this->b = (this->a + "y");
}
```

`b` is declared first but assigned second; the compiler hoisted `a` and left `b = a + "y"` in the body, so `b` sees a finished `a`. The ordinary case does hoist, and the first call ends
the run:

<!-- example: 02_ctor_hoist.clg -->
```cpp
class Point {
public:
    int x;
    int y;
    int sum;
    Point(int px, int py) {
        x = px;
        y = py;
        sum = x + y;
    }
};

class Box {
public:
    int size;
    int calls;
    Box(int s) {
        size = s;
        calls = 0;
        announce();            // a call ends the run of plain assignments
    }
    void announce() { calls = calls + 1; }
};

int main() {
    Point p = new Point(3, 4);
    std::cout << p.x << " " << p.y << " " << p.sum << std::endl;
    Box b = new Box(2);
    std::cout << b.size << " " << (b.size + 3) << std::endl;
    return 0;
}
```
<!-- output: 02_ctor_hoist.clg -->
```
3 4 7
2 5
```

<!-- generated: 02_ctor_hoist.clg "Point::Point" -->
```cpp
Point::Point(int px, int py) : x(px), y(py), sum((this->x + this->y)) {
}
```

<!-- generated: 02_ctor_hoist.clg "Box::Box" -->
```cpp
Box::Box(int s) : size(s), calls(0) {
    announce();
}
```

Hoisting is also what lets a struct hold a struct whose only constructor takes arguments. A plain assignment in the body would first need to build a default `Point`, which does not exist:

<!-- example: 02_ctor_nested_struct.clg -->
```cpp
struct Point {
public:
    int x;
    int y;
    Point(int px, int py) { x = px; y = py; }      // no constructor without arguments
};

struct Wrapper {
public:
    Point point;
    Wrapper(Point p) { point = p; }                // C++ must build point here, not assign it later
};

int main() {
    Point p = new Point(1, 2);
    Wrapper w = new Wrapper(p);
    std::cout << w.point.x << " " << w.point.y << std::endl;
    return 0;
}
```
<!-- output: 02_ctor_nested_struct.clg -->
```
1 2
```

<!-- generated: 02_ctor_nested_struct.clg "Wrapper::Wrapper" -->
```cpp
Wrapper::Wrapper(Point p) : point(p) {
}
```

There is no initialiser-list syntax (`A(int v) : x(v)`) and no explicit base-constructor call (`B(int v) : A(v)`); a derived constructor assigns the inherited fields itself. The compiler
supplies the base's no-argument constructor.

**Where it stops.** The reordering rules protect fields from being read too early. A primitive field the constructor never assigns starts at zero (see "Empty handles").

## Classes that refer to each other

C++ needs a forward declaration (`class Child;`) and pointers to cross-refer. CeeLing classes may use classes declared later in the file, in fields, parameters and method bodies, and the
output carries the forward declarations:

<!-- example: 02_mutual_classes.clg -->
```cpp
class Parent {
public:
    string name;
    Child kid;                         // Child is declared further down
    Parent(string n) { name = n; }
    void adopt(Child c) {
        kid = c;
        c.mother = this;
    }
};

class Child {
public:
    string name;
    int age;
    Parent mother;
    Child(string n, int a) { name = n; age = a; }
    string describe() { return mother.name + " has " + name + ", aged " + std::to_string(age); }
};

int main() {
    Parent p = new Parent("Ann");
    p.adopt(new Child("Bo", 5));
    std::cout << p.kid.describe() << std::endl;
    std::cout << p.kid.name << "'s mother: " << p.kid.mother.name << std::endl;
    return 0;
}
```
<!-- output: 02_mutual_classes.clg -->
```
Ann has Bo, aged 5
Bo's mother: Ann
```

The generated C++ starts with `class Parent;` and `class Child;`, then the two class bodies.

`Child mother` points back at its parent and nothing in the source says "weak"; the compiler sees that `kid` owns downward and `mother` points back (previous sections).

**Free functions do not get this.** They are emitted in source order, with no forward declaration, so a free function can use only functions above it, and two free functions cannot be
mutually recursive. Put mutually recursive code in methods of one class, where the order does not matter:

<!-- example: 02_mutual_functions.clg -->
```cpp
class Parity {
public:
    Parity() { }
    int isEven(int n) {
        if (n == 0) { return 1; }
        return isOdd(n - 1);               // a method may call one declared after it
    }
    int isOdd(int n) {
        if (n == 0) { return 0; }
        return isEven(n - 1);
    }
};

int main() {
    Parity p = new Parity();
    std::cout << p.isEven(10) << " " << p.isOdd(10) << std::endl;
    return 0;
}
```
<!-- output: 02_mutual_functions.clg -->
```
1 0
```

## The destructor you do not write

There is no destructor syntax; writing `~Resource() { }` is a syntax error.

<!-- example: 02_no_destructor_syntax.clg -->
```cpp
class Resource {
public:
    int id;
    Resource(int i) { id = i; }
    ~Resource() { }            // there is no destructor syntax
};

int main() {
    return 0;
}
```
<!-- error: 02_no_destructor_syntax.clg -->
```
Error: Syntax error at line 4, column 32, node type '}'
```

What the compiler writes for you is two things. A class used as a base gets `virtual ~Base() = default;`, so deleting a derived object through a base handle can never skip the derived
destructor (below). And a class with a direct owning field of its own type (a list node) gets a generated destructor that frees the chain in a loop. The reason is a plain C++ crash:

<!-- example: 02_destructor.cpp -->
```cpp
// Each node's destructor destroys the next node, which destroys the next: one stack frame per node.
// With a million nodes the program prints its line and then crashes while freeing the list
// (not run by the guide; on the guide's toolchain it exits with a stack overflow).
#include <iostream>
#include <memory>

struct Item {
    std::unique_ptr<Item> next;
    int v;
    explicit Item(int x) : v(x) {}
};

int main() {
    std::unique_ptr<Item> head = std::make_unique<Item>(0);
    for (int i = 1; i < 1000000; i = i + 1) {
        auto n = std::make_unique<Item>(i);
        n->next = std::move(head);
        head = std::move(n);
    }
    std::cout << "built " << head->v << std::endl;
    return 0;
}
```

**Why C++ does not stop it.** Each `unique_ptr` destructor destroys the next node from inside its own, one stack frame per node, and the language has no limit check. A million-node list
compiles clean, builds, prints, and crashes at the closing brace (on the guide's toolchain it exits with 127; the guide does not run it).

<!-- example: 02_destructor.clg -->
```cpp
class Item {
public:
    Item next;
    int v;
    Item(int x) { v = x; }
};

int main() {
    Item head = new Item(0);
    int i = 1;
    while (i < 1000000) {
        Item n = new Item(i);
        n.next = head;
        head = n;
        i = i + 1;
    }
    std::cout << "built " << head.v << std::endl;
    return 0;                          // a million nodes are freed here, one at a time
}
```
<!-- output: 02_destructor.clg -->
```
built 999999
```

<!-- generated: 02_destructor.clg "Item::~Item" -->
```cpp
Item::~Item() {
    if (!next) return;
    std::vector<std::shared_ptr<Item>> pending;
    if (next) pending.push_back(std::move(next));
    while (!pending.empty()) {
        std::shared_ptr<Item> node = std::move(pending.back());
        pending.pop_back();
        if (node.use_count() != 1) continue;
        if (node->next) pending.push_back(std::move(node->next));
    }
}
```

The generated destructor detaches the links onto a heap worklist and frees them one at a time, so the depth stays one. A node with another owner is only released, never taken apart.

**Where it stops.** The loop covers a direct self-typed field. A chain that goes through a `std::vector<Item>` or through another class (A to B to A), and a class that has a base or
subclasses, keep the implicit destructor, and a deep enough chain there can still overflow the stack.

## Inheritance

A class has at most one base: `class Dog : public Animal`. The base must be a class in this program (the base list is always `public`). A derived class gets the base's fields and methods;
its constructor assigns inherited fields directly. A base-typed variable or parameter accepts any derived object.

C++ gives `virtual` to the programmer per method, and the cost of forgetting it is silent. Both halves of that appear below: only `virtual` methods dispatch on the real object.

<!-- example: 02_inherit_dispatch.clg -->
```cpp
class Animal {
public:
    string name;
    Animal(string n) { name = n; }
    string kind() { return "animal"; }                 // not virtual: the declared type decides
    virtual string sound() { return "..."; }           // virtual: the real object decides
};

class Dog : public Animal {
public:
    Dog(string n) { name = n; }                         // inherited fields are assigned directly
    string kind() { return "dog"; }
    string sound() override { return "woof"; }
};

class Puppy : public Dog {
public:
    Puppy(string n) { name = n; }
    string sound() override { return "yip"; }
};

void describe(Animal a) {                               // accepts any Animal, derived ones included
    std::cout << a.name << " " << a.kind() << " " << a.sound() << std::endl;
}

int main() {
    Animal pet = new Dog("Rex");                        // declared Animal, built as Dog
    describe(pet);
    Puppy p = new Puppy("Bit");
    describe(p);
    std::vector<Animal> zoo;
    zoo.push_back(new Animal("Generic"));
    zoo.push_back(new Dog("Rex"));
    zoo.push_back(new Puppy("Bit"));
    string line = "";
    for (Animal z : zoo) {
        line = line + z.sound() + " ";
    }
    std::cout << line.substr(0, line.length() - 1) << std::endl;
    return 0;
}
```
<!-- output: 02_inherit_dispatch.clg -->
```
Rex animal woof
Bit animal yip
... woof yip
```

`kind()` is not virtual, so through an `Animal` handle `Dog("Rex")` answers `animal`, as in C++. `sound()` is virtual, so the object decides. `Animal pet = new Dog("Rex")` declares an
`Animal` and builds a `Dog`; the pointer kind comes from the declared type. Every base class gets a virtual destructor, which C++ makes you remember:

<!-- generated: 02_inherit_dispatch.clg "class Animal {" -->
```cpp
class Animal {
public:
    virtual ~Animal() = default;
protected:
    Animal() = default;
public:
    std::string name;
    Animal(std::string n);
    std::string kind();
    virtual std::string sound();
};
```

The C++ twin of the mistake that matters here: no virtual destructor, delete through a base pointer (g++ happens to warn; with smart pointers built from raw pointers the same mistake
compiles silently):

<!-- example: 02_inherit_no_virtual_dtor.cpp -->
```cpp
// Deleting a Dog through an Animal* when Animal's destructor is not virtual is undefined behaviour:
// Dog's destructor (and its members) may never run. g++ -Wall happens to warn about the delete here;
// with a smart pointer built from a raw base pointer the same mistake compiles in silence.
#include <iostream>
#include <string>

struct Animal {
    virtual void speak() { std::cout << "..." << std::endl; }
};

struct Dog : Animal {
    std::string toy = "ball";
    void speak() override { std::cout << "woof" << std::endl; }
};

int main() {
    Animal* a = new Dog();
    a->speak();
    delete a;
    return 0;
}
```

**Passing a derived object to a base-typed parameter** keeps the derived object. In C++ a base-typed *value* parameter slices it, and nothing warns:

<!-- example: 02_inherit_slicing.cpp -->
```cpp
// A base-class PARAMETER taken by value copies only the base part of the argument: the Dog is sliced
// into an Animal, and the call prints the base's answer. The program compiles without a warning and
// prints "..." for both lines.
#include <iostream>
#include <string>
#include <vector>

struct Animal {
    virtual ~Animal() = default;
    virtual std::string sound() const { return "..."; }
};

struct Dog : Animal {
    std::string sound() const override { return "woof"; }
};

void describe(Animal a) { std::cout << a.sound() << std::endl; }

int main() {
    Dog d;
    describe(d);                       // prints ...
    std::vector<Animal> zoo;
    zoo.push_back(d);                  // sliced again
    std::cout << zoo[0].sound() << std::endl;
    return 0;
}
```

Both lines print `...`. In CeeLing `describe(Animal a)` takes a handle, so `describe(p)` in the example above printed `yip`.

**A virtual call inside a constructor** runs the base's version in C++, even for a derived object. The program below is accepted by g++ with no warning and prints `generic`:

<!-- example: 02_inherit_ctor_virtual.cpp -->
```cpp
// While Animal's constructor runs, the object is only an Animal: the virtual call does not reach Dog.
// It prints "generic", although the object being built is a Dog. No warning.
#include <iostream>

struct Animal {
    Animal() { speak(); }
    virtual ~Animal() = default;
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

CeeLing rejects it. (There is no destructor syntax, so the matching trap in destructors cannot be written.)

<!-- example: 02_inherit_ctor_virtual.clg -->
```cpp
class Animal {
public:
    int age;
    Animal(int a) {
        age = a;
        speak();                 // in C++ this runs Animal::speak even for a Dog
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
<!-- error: 02_inherit_ctor_virtual.clg -->
```
error[semantic/forbidden-construct]: virtual method `speak` called from a constructor of `Animal`
  --> 02_inherit_ctor_virtual.clg:6:9
  help: A virtual call inside a constructor does NOT dispatch to a derived class's override -- the object is still being built, so the base version runs instead. That makes the code do something different from what it looks like it does. Move the call to after construction, or call a non-virtual method.
```

Other rejections: a base the program does not define (an external class has no known members to check), and an inheritance cycle.

<!-- example: 02_inherit_unknown_base.clg -->
```cpp
class Dog : public Missing {
public:
    int age;
    Dog(int a) { age = a; }
};

int main() {
    return 0;
}
```
<!-- error: 02_inherit_unknown_base.clg -->
```
Error: error[semantic/unknown-base-class]: class `Dog` inherits from `Missing`, which is not a class declared in this program (line 1). CeeLing cannot inherit from anything it did not compile itself -- an external base class has no known field/method set, so its members could not be resolved or safety-checked.
```
<!-- example: 02_inherit_cycle.clg -->
```cpp
class A : public B {
public:
    A() { }
};

class B : public A {
public:
    B() { }
};

int main() {
    return 0;
}
```
<!-- error: 02_inherit_cycle.clg -->
```
Error: error[semantic/inheritance-cycle]: class `A` is part of an inheritance cycle (line 1). A class cannot directly or indirectly inherit from itself.
```

A base handle offers the base's methods only. There is no downcast (`dynamic_cast` is rejected), so a derived-only method cannot be reached through it:

<!-- example: 02_derived_only_method_rejected.clg -->
```cpp
class Animal {
public:
    Animal() { }
    virtual string sound() { return "..."; }
};

class Dog : public Animal {
public:
    Dog() { }
    string sound() override { return "woof"; }
    string fetch() { return "ball"; }
};

int main() {
    Animal a = new Dog();
    std::cout << a.fetch() << std::endl;       // an Animal handle offers Animal's methods only; there is no downcast
    return 0;
}
```
<!-- error: 02_derived_only_method_rejected.clg -->
```
error[semantic/forbidden-construct]: call to unknown function/method `fetch`
  --> 02_derived_only_method_rejected.clg:16:18
  help: CeeLing has no borrow/ownership summary for anything it did not compile itself; unknown calls are rejected by default rather than silently trusted (CLAUDE.md "default-reject, never default-allow"). Define `fetch` in CeeLing source, or call an existing CeeLing function/method instead.
```

(The message is the general "unknown call" message; the method does exist, on the derived class.) There is no multiple inheritance, no pure virtual (`= 0`), no `protected`, and no way to
call a base-class method explicitly; see the end of the chapter.

**Where it stops.** `override` is passed through to C++: a typo such as `sund() override` is not caught by CeeLing, g++ rejects the output. A class that inherits in a program, or is
inherited from, is excluded from the thread-sendable objects ([chapter 10](10-threads.md)).

## `enum` and `namespace`

`enum Color { Red, Green, Blue };` is an unscoped enum, as in C++: the names are visible without a prefix, and an enumerator converts to `int`. You cannot give values (`= 5` is a syntax
error); they count from zero. The type can be used for fields, parameters and locals.

<!-- example: 02_enum.clg -->
```cpp
enum Color { Red, Green, Blue };

class Pixel {
public:
    Color color;
    Pixel(Color c) { color = c; }
    string name() {
        if (color == Red) { return "red"; }
        if (color == Green) { return "green"; }
        return "blue";
    }
};

int main() {
    Pixel p = new Pixel(Green);
    Color c = Blue;
    int n = Green;                         // an enumerator converts to int, as in C++ (unscoped enum)
    std::cout << p.name() << " " << n << " " << c << std::endl;
    return 0;
}
```
<!-- output: 02_enum.clg -->
```
green 1 2
```

(`Color d = 7;` is not caught by CeeLing; g++ rejects the int-to-enum conversion.)

`namespace Name { ... }` holds classes, enums, free functions and structs; names are used qualified from outside (`Shapes::Shape`, `Shapes::Square`) and unqualified from inside. In C++
you write the same, plus the forward declarations; CeeLing adds no new rule.

<!-- example: 02_namespace.clg -->
```cpp
namespace Shapes {
    enum Kind { Circle, Square };
    class Shape {
    public:
        int kind;
        int size;
        Shape(int k, int s) { kind = k; size = s; }
        int area() {
            if (kind == Square) { return size * size; }      // inside the namespace, no prefix
            return 3 * size * size;
        }
    };
    int perimeterOfSquare(int s) { return 4 * s; }
}

namespace Util {
    int twice(int x) { return x + x; }
}

int main() {
    Shapes::Shape s = new Shapes::Shape(Shapes::Square, 3);
    std::cout << s.area() << " " << Shapes::perimeterOfSquare(3) << " " << Util::twice(4) << std::endl;
    Shapes::Kind k = Shapes::Circle;
    std::cout << k << std::endl;
    return 0;
}
```
<!-- output: 02_namespace.clg -->
```
9 12 8
0
```

<!-- generated: 02_namespace.clg "namespace Shapes" -->
```cpp
namespace Shapes {

enum Kind { Circle, Square };

#ifndef CEELING_DECL_SHAPE
#define CEELING_DECL_SHAPE
class Shape {
public:
    int kind{};
    int size{};
    Shape(int k, int s);
    int area();
};
```

Namespaces are **one level only**, and the same name can be opened more than once. There is no `using namespace`.

<!-- example: 02_namespace_nested_rejected.clg -->
```cpp
namespace Outer {
    namespace Inner {
        class Node {
        public:
            int v;
            Node(int x) { v = x; }
        };
    }
}

int main() {
    return 0;
}
```
<!-- error: 02_namespace_nested_rejected.clg -->
```
Error: error[semantic/nested-namespace]: namespace `Outer` contains a nested namespace declaration (line 2, column 5). CeeLing supports only a single level of namespace nesting -- flatten the inner namespace's members into `Outer` directly, or give it its own top-level namespace.
```

## Operators

Arithmetic `+ - * / %`, comparison `== != < > <= >=`, logical `&& || !`, bitwise `& | ~` and shifts `<< >>` work as in C++ on numbers; unary minus and prefix `++` exist. There is no `^`, no
postfix `x++`, no compound assignment (`+=`), and no ternary. Integer division truncates, `%` is C++'s `%`, and nothing is checked beyond what C++ checks: in a test, `2147483647 + 1` printed a wrapped value and `10 / 0` crashed the program, because the operators are emitted unchanged
([chapter 3](03-values-and-containers.md)).

<!-- example: 02_arithmetic.clg -->
```cpp
int main() {
    int a = 7;
    int b = 2;
    std::cout << a / b << " " << a % b << " " << -a << " " << a - 5 << " " << a / 2.0 << std::endl;
    std::cout << (a > b) << (a == b) << (a != b && !(a < b)) << (a <= b || a < 0) << std::endl;
    int bits = 12;
    std::cout << (bits & 4) + 8 << " " << (bits | 3) - 12 << " " << (bits >> 1) - 1 << " " << (1 << 3) << " " << ~8 << std::endl;
    std::cout << (5 << 3) << " " << (40 >> 3) << std::endl;
    return 0;
}
```
<!-- output: 02_arithmetic.clg -->
```
3 1 -7 2 3.5
1010
12 3 5 8 -9
40 5
```

**Precedence is C++'s.** From tightest to loosest: `* / %`, `+ -`, `<< >>`, `< <= > >=`, `== !=`, `&`, `|`, `&&`, `||`. A shift binds tighter than a comparison, so `4 >> 1 == 2` is
`(4 >> 1) == 2`; `==` binds tighter than `&`, which is C++'s famous trap, kept as it is (`6 & 3 == 3` is `6 & (3 == 3)`). Until the grammar was corrected the shift and comparison tiers were the
wrong way round, so `4 >> 1 == 2` meant `4 >> (1 == 2)`; programs that relied on that need the parentheses they would have needed in C++.

<!-- example: 02_precedence_shift.clg -->
```cpp
int main() {
    int bare = 4 >> 1 == 2;          // as in C++: (4 >> 1) == 2, which is 1
    int grouped = (4 >> 1) == 2;
    int sum = 1 << 2 + 1;            // + binds tighter than <<: 1 << 3
    int masked = 6 & 3 == 3;         // == binds tighter than &, as in C++: 6 & (3 == 3) is 6 & 1
    std::cout << bare << " " << grouped << " " << sum << " " << masked << std::endl;
    return 0;
}
```
<!-- output: 02_precedence_shift.clg -->
```
1 1 8 0
```

<!-- generated: 02_precedence_shift.clg "int main()" -->
```cpp
int main() {
    int bare = ((4 >> 1) == 2);
    int grouped = ((4 >> 1) == 2);
    int sum = (1 << (2 + 1));
    int masked = (6 & (3 == 3));
    std::cout << bare << " " << grouped << " " << sum << " " << masked << std::endl;
    return 0;
}
```

(The `<<` that writes to `std::cout` is chained flat and is not affected; `std::cout << a + b` prints the sum. A loose boolean needs parentheses in a stream chain: `std::cout << (a || b)`.
`&` and `|` bind looser than `==`, which is C++'s own rule.)

**Overloading** is a fixed set, as methods of a class: `operator[]` (read only), `operator=`, `operator*`, prefix `operator++`, `operator!=` and `operator==`, plus the free function
`ostream operator<<(ostream out, T x)`. Anything else (`operator+`, `operator<`, and so on) is a syntax error; write a named method.

<!-- example: 02_operators.clg -->
```cpp
class Money {
public:
    int cents;
    Money(int c) { cents = c; }
    int operator[](int extra) { return cents + extra; }       // reading m[i]
    void operator=(int c) { cents = c; }                        // m = 7;
    bool operator==(Money other) { return cents == other.cents; }
};

ostream operator<<(ostream out, Money m) {
    out << "money(" << m.cents << ")";
    return out;
}

int main() {
    Money m = new Money(42);
    std::cout << m[0] << " " << m[5] << std::endl;
    m = 7;
    std::cout << m.cents << std::endl;
    std::cout << m << std::endl;
    Money n = new Money(7);
    std::cout << "equal money: " << (m == n) << std::endl;
    return 0;
}
```
<!-- output: 02_operators.clg -->
```
42 47
7
money(7)
equal money: 1
```

<!-- example: 02_operator_plus_rejected.clg -->
```cpp
class Money {
public:
    int cents;
    Money(int c) { cents = c; }
    Money operator+(Money other) { return new Money(cents + other.cents); }
};

int main() {
    return 0;
}
```
<!-- error: 02_operator_plus_rejected.clg -->
```
Error: Syntax error at line 5, column 19, node type 'ERROR', near: +. Suggestion: check whether the statement is missing a trailing ';'.
```

`operator[]` returns a value, so `m[i] = v` is rejected with an advice to write a setter. On a `std::vector`, `std::string` or `std::map` the subscript keeps its meaning (checked indexing,
chapter 7; insert-or-update for a map).

<!-- example: 02_operator_subscript_write_rejected.clg -->
```cpp
class Box {
public:
    int val;
    Box(int v) { val = v; }
    int operator[](string key) { return val; }
};

int main() {
    Box b = new Box(5);
    b["key"] = 10;                 // operator[] can be read, not assigned through
    return 0;
}
```
<!-- error: 02_operator_subscript_write_rejected.clg -->
```
error[semantic/forbidden-construct]: assignment through `operator[]` is not supported
  --> 02_operator_subscript_write_rejected.clg:10:5
  help: `b` is a Box; `b[key] = value` would need operator[] to return a reference, which CeeLing does not yet support (no `ref` return type). Add a named setter method instead (e.g. `void set(key, value)`).
```

**The iterator protocol** is how your own class works in a range-for. Give the class (or a free function) `begin()` and `end()` returning an iterator-like class with `operator*`,
prefix `operator++` and `operator!=`:

<!-- example: 02_iterator_protocol.clg -->
```cpp
class Counter {
public:
    int value;
    Counter(int v) { value = v; }
    int operator*() { return value; }                // *it: the current element
    void operator++() { value = value + 1; }         // ++it: step forward
    bool operator!=(Counter other) { return value != other.value; }
};

class Range {
public:
    int lo;
    int hi;
    Range(int a, int b) { lo = a; hi = b; }
    Counter begin() { return new Counter(lo); }
    Counter end() { return new Counter(hi); }
};

class Item {
public:
    int value;
    Item next;
    Item(int v) { value = v; }
};

class Cursor {
public:
    Item current;
    Cursor(Item start) { current = start; }
    Item operator*() { return current; }
    void operator++() { current = current.next; }
    bool operator!=(Cursor other) { return current != other.current; }
};

class Bag {
public:
    Item head;
    Bag() { }
    void push(int v) {
        Item n = new Item(v);
        n.next = head;
        head = n;
    }
};

Cursor begin(Bag b) { return new Cursor(b.head); }       // free begin/end work too
Cursor end(Bag b) { return new Cursor(nullptr); }

int main() {
    Range r = new Range(1, 5);
    string line = "";
    for (int x : r) { line = line + "x=" + std::to_string(x) + " "; }
    std::cout << line.substr(0, line.length() - 1) << std::endl;

    Bag bag = new Bag();
    bag.push(1);
    bag.push(2);
    bag.push(3);
    string seen = "";
    for (Item it : bag) { seen = seen + "v=" + std::to_string(it.value) + " "; }
    std::cout << seen.substr(0, seen.length() - 1) << std::endl;
    return 0;
}
```
<!-- output: 02_iterator_protocol.clg -->
```
x=1 x=2 x=3 x=4
v=3 v=2 v=1
```

## Control flow

`if` / `else if` / `else`, `while`, a range-based `for`, `break` and `continue`. Braces are required. There is no C-style `for(;;)`, no `do/while`, no `switch`, no `goto`, no labels:
`break` and `continue` always mean the innermost loop, and outside a loop they are an error.

<!-- example: 02_control_flow.clg -->
```cpp
int classify(int n) {
    if (n < 3) {
        return 0;
    } else if (n < 10) {
        return 1;
    } else {
        return 2;
    }
}

int main() {
    int i = 0;
    while (i < 8) {
        i = i + 1;
        if (i % 2 == 0) { continue; }        // skip even numbers
        if (i > 5) { break; }                // leave the loop
        std::cout << i;
    }
    std::cout << std::endl;
    if (classify(3) == 1) { std::cout << "3 is small" << std::endl; }
    std::vector<int> v;
    v.push_back(1);
    v.push_back(2);
    v.push_back(3);
    int sum = 0;
    for (int x : v) {
        if (x == 3) { break; }
        sum = sum + x;
    }
    std::cout << "sum 1+2 then stop: " << sum << std::endl;
    return 0;
}
```
<!-- output: 02_control_flow.clg -->
```
135
3 is small
sum 1+2 then stop: 3
```

<!-- example: 02_break_outside_loop.clg -->
```cpp
int main() {
    int i = 0;
    if (i == 0) {
        break;
    }
    return 0;
}
```
<!-- error: 02_break_outside_loop.clg -->
```
error[semantic/use-after-move]: `break` outside a loop
  --> 02_break_outside_loop.clg:4:9
  help: `break` and `continue` apply to the innermost enclosing `while` or `for`; there is no `switch` and no labelled form.
```

**What the loop variable is** depends on the element type, and CeeLing writes the `&` that C++ makes you remember. In C++ `for (auto x : v)` copies each element, so an edit to `x` is
lost; the fix is `auto&`, and forgetting it compiles clean:

<!-- example: 02_range_for.cpp -->
```cpp
// Forgetting the & in a range-for copies every element, and the loop body edits the copy.
// The program compiles without a warning and prints 1 2.
#include <iostream>
#include <vector>

int main() {
    std::vector<int> v{1, 2};
    for (auto x : v) { x = x + 10; }       // should have been auto&
    std::cout << v[0] << " " << v[1] << std::endl;
    return 0;
}
```

In CeeLing a primitive loop variable is a copy, a class-typed one is the element's own handle (an edit changes the object), and a struct is copied:

<!-- example: 02_range_for_elements.clg -->
```cpp
class Cell {
public:
    int n;
    Cell(int x) { n = x; }
};

struct Val {
public:
    int n;
    Val(int x) { n = x; }
};

int main() {
    std::vector<int> v;
    v.push_back(1);
    v.push_back(2);
    for (int x : v) { x = x + 10; }              // x is a copy of the element
    std::cout << "ints: " << v[0] << " " << v[1] << std::endl;

    std::vector<Cell> cs;
    cs.push_back(new Cell(1));
    for (Cell c : cs) { c.n = c.n + 10; }        // c is the element's handle: the same object
    std::cout << "objects: " << cs[0].n << std::endl;

    std::vector<Val> vs;
    vs.push_back(new Val(1));
    for (Val s : vs) { s.n = s.n + 10; }         // a struct element is copied
    std::cout << "structs: " << vs[0].n << std::endl;
    return 0;
}
```
<!-- output: 02_range_for_elements.clg -->
```
ints: 1 2
objects: 11
structs: 1
```

<!-- generated: 02_range_for_elements.clg "int main()" -->
```cpp
int main() {
    std::vector<int> v;
    v.push_back(1);
    v.push_back(2);
    for (int x : v) {
        x = (x + 10);
    }
    std::cout << "ints: " << cling::at(v, 0, 18) << " " << cling::at(v, 1, 18) << std::endl;
    std::vector<std::unique_ptr<Cell>> cs;
    cs.push_back(std::make_unique<Cell>(1));
    for (std::unique_ptr<Cell>& c : cs) {
        cling::nn(c, "line 22")->n = (cling::nn(c, "line 22")->n + 10);
    }
    std::cout << "objects: " << cling::nn(cling::at(cs, 0, 23), "line 23")->n << std::endl;
    std::vector<Val> vs;
    vs.push_back(Val(1));
    for (Val s : vs) {
        s.n = (s.n + 10);
    }
    std::cout << "structs: " << cling::at(vs, 0, 28).n << std::endl;
    return 0;
}
```

**Where it stops.** Changing a container while looping over it (`push_back` in the body) is the iterator-invalidation case of chapter 6. Range-for over a `std::map` gives C++ pairs, and a
`for (int k : m)` does not compile in g++. A `for` runs over a container or a class with the iterator protocol above; there is no counting loop, so count with `while`.

## Errors: `throw` and `catch`

`throw` takes a string, `catch (string e)` takes a string. That is the whole mechanism: no exception classes, one `catch` per `try`, no `catch (...)`, no rethrow keyword (throw the caught
string again). The thrown string can be built with `+`. An exception travels up through calls until a `catch` matches.

<!-- example: 02_errors.clg -->
```cpp
int parse(string s, int limit) {
    if (s.length() == 0) { throw "empty input"; }
    int n = std::stoi(s);
    if (n > limit) { throw "too big: " + s; }
    return n;
}

void attempt(string s) {
    try {
        std::cout << parse(s, 100) << std::endl;
    } catch (string e) {
        std::cout << "caught: " << e << std::endl;
    }
}

int main() {
    attempt("42");
    attempt("");
    attempt("500");
    try {
        try { throw "inner"; } catch (string e) { throw e + "+outer"; }
    } catch (string e) {
        std::cout << e << std::endl;
    }
    return 0;
}
```
<!-- output: 02_errors.clg -->
```
42
caught: empty input
caught: too big: 500
inner+outer
```

<!-- generated: 02_errors.clg "int parse" -->
```cpp
int parse(const std::string& s, int limit) {
    if ((s.length() == 0)) {
        throw std::string("empty input");
    }
    int n = std::stoi(s);
    if ((n > limit)) {
        throw ("too big: " + s);
    }
    return n;
}
```

CeeLing writes `throw std::string(...)` for you. In C++, `throw "text"` throws a `const char*`, which `catch (std::string)` does not match, and the program ends in `std::terminate` (g++ is silent):

<!-- example: 02_throw_const_char.cpp -->
```cpp
// throw "text" throws a const char*, not a std::string, so catch (std::string) never matches and the
// program ends in std::terminate. No warning. (Not run by the guide.)
#include <iostream>
#include <string>

int main() {
    try {
        throw "bad thing";
    } catch (std::string e) {
        std::cout << "caught " << e << std::endl;
    }
    return 0;
}
```

Other payloads and catch types are rejected:

<!-- example: 02_throw_non_string_rejected.clg -->
```cpp
int main() {
    throw 42;
    return 0;
}
```
<!-- error: 02_throw_non_string_rejected.clg -->
```
error[semantic/use-after-move]: `throw` operand is not a string
  --> 02_throw_non_string_rejected.clg:2:11
  help: CeeLing's `throw` is restricted to `string` messages -- there is no exception-class hierarchy yet (that needs inheritance/polymorphism, not implemented), so a thrown value can only be a plain error message. Throw a string literal or a `string`-typed variable instead.
```
<!-- example: 02_catch_int_rejected.clg -->
```cpp
int main() {
    try {
        throw "x";
    } catch (int e) {
    }
    return 0;
}
```
<!-- error: 02_catch_int_rejected.clg -->
```
error[semantic/use-after-move]: `catch` type `int` is not supported
  --> 02_catch_int_rejected.clg:2:5
  help: CeeLing's `catch` is restricted to `string` this slice, matching `throw`'s restriction to string messages -- there is no exception-class hierarchy yet. Declare the catch parameter as `string`.
```

(The error category in these two messages is printed as `use-after-move`, which is the checker that hosts them; the text is what counts.)

**What `catch` does not catch.** A library function throws its own type. `std::stoi("abc")` throws `std::invalid_argument`, and `catch (string e)` does not match it, so the program ends in
`terminate`:

<!-- example: 02_catch_not_everything.clg -->
```cpp
int main() {
    try {
        int n = std::stoi("abc");          // the library throws its own exception type
        std::cout << n << std::endl;
    } catch (string e) {
        std::cout << "caught " << e << std::endl;
    }
    return 0;
}
```
<!-- output: 02_catch_not_everything.clg -->
```
terminate called after throwing an instance of 'std::invalid_argument'
  what():  stoi
```

And the run-time checks of [chapter 7](07-run-time-checks.md) are not exceptions at all. A failed bounds check calls `abort()`, so no `catch` can swallow it:

<!-- example: 02_bounds_not_catchable.clg -->
```cpp
int main() {
    std::vector<int> v;
    v.push_back(1);
    try { std::cout << v[3] << std::endl; } catch (string e) { std::cout << "caught" << std::endl; }
    return 0;
}
```
<!-- output: 02_bounds_not_catchable.clg -->
```
cling: index out of bounds at line 4 (index 3, size 1)
```

The same holds for a null dereference. If the failure should be catchable, test first and `throw` yourself. `std::string::substr` with a bad position also throws a library exception
(`std::out_of_range`) that `catch (string e)` does not match.

## Generic syntax

CeeLing has exactly one generic feature, and it is a narrow one: a **generic method whose return type is the type parameter**. The shape comes from the libraries it was built to port
(`node.as<int>()`). The primary `<T> T as()` declares the name; each type you support gets a specialization `int as<int>()`. The call is `n.as<int>()`. There is a second form with an
argument of the same type (`as<int>(7)`), and a specialization may be added **outside** the class, with `ReturnType Class::method<Type>(params) { ... }`, so another file can teach
`Node` to produce its own types.

<!-- example: 02_generic_as.clg -->
```cpp
class Node {
public:
    string text;
    Node(string t) { text = t; }
    <T> T as() {                                  // the primary: the result type is the type parameter
        throw "no conversion for this type";
    }
    int as<int>() { return std::stoi(text); }     // one specialization per type you support
    double as<double>() { return std::stod(text); }
    <T> T as(T fallback) { return fallback; }
    int as<int>(int fallback) {
        if (text.length() == 0) { return fallback; }
        return std::stoi(text);
    }
};

struct Point {
public:
    int x;
    int y;
    Point(int a, int b) { x = a; y = b; }
};

Point Node::as<Point>() {                         // added from outside the class
    return new Point(std::stoi(text), 0);
}

int main() {
    Node n = new Node("42");
    Node half = new Node("2.5");
    Node empty = new Node("");
    int i = n.as<int>();
    Point p = n.as<Point>();
    std::cout << i << " " << half.as<double>() << " " << p.x << " " << empty.as<int>(7) << std::endl;
    int a = 3;
    int b = 5;
    std::cout << (a < b) << (n.as<int>() < 5) << std::endl;      // < is still just less-than
    return 0;
}
```
<!-- output: 02_generic_as.clg -->
```
42 2.5 42 7
10
```

<!-- generated: 02_generic_as.clg "template<>" -->
```cpp
template<>
int Node::as<int>() {
    return std::stoi(this->text);
}
```

The compiler turns the primary into a C++ member template and each specialization into an explicit specialization, so C++ overload resolution chooses the body. A call with a type that has no
specialization is rejected, so the primary's `throw` is never what runs:

<!-- example: 02_generic_unspecialized_rejected.clg -->
```cpp
class Node {
public:
    string text;
    Node(string t) { text = t; }
    <T> T as() { throw "no conversion"; }
    int as<int>() { return std::stoi(text); }
};

int main() {
    Node n = new Node("42");
    string s = n.as<string>();           // no as<string> was written
    return 0;
}
```
<!-- error: 02_generic_unspecialized_rejected.clg -->
```
error[semantic/forbidden-construct]: call to unknown function/method `as`
  --> 02_generic_unspecialized_rejected.clg:11:16
  help: CeeLing has no borrow/ownership summary for anything it did not compile itself; unknown calls are rejected by default rather than silently trusted (CLAUDE.md "default-reject, never default-allow"). Define `as` in CeeLing source, or call an existing CeeLing function/method instead.
```

`a < b` and `x.length() < 5` are still comparisons: the parser takes the `<` as a type argument only when the whole `name<Type>(...)` shape follows.

There are no generic free functions, no generic parameter types, no several type parameters and no generic classes:

<!-- example: 02_generic_free_rejected.clg -->
```cpp
<T> T identity(T x) {
    return x;
}

int main() {
    return 0;
}
```
<!-- error: 02_generic_free_rejected.clg -->
```
Error: Syntax error at line 1, column 1, node type 'ERROR', near: <T> T identity(T x) {
```

The standard containers are generic as written (`std::vector<Node>`, `std::map<string, int>`, nested too); [chapter 3](03-values-and-containers.md).

## What is not in the language, and what to write instead

Each item below was tried with the compiler. Most are a syntax error, because the grammar has no production for them. That is the cheap way to seal a feature, and it is why the message is
usually "Syntax error" and not an explanation; the ones with explanations say so.

**A ternary.** Use `if`.

<!-- example: 02_no_ternary.clg -->
```cpp
int main() {
    int a = 3;
    int b = a > 2 ? 1 : 0;
    std::cout << b << std::endl;
    return 0;
}
```
<!-- error: 02_no_ternary.clg -->
```
Error: Syntax error at line 3, column 19, node type 'ERROR', near: ? 1 : 0. Suggestion: check whether the statement is missing a trailing ';'.
```

**File-scope variables.** Global mutable state has no owner and no lifetime the checker can follow. Put the value in a class field or pass it as a parameter.

<!-- example: 02_no_file_scope.clg -->
```cpp
int counter = 0;

int main() {
    counter = counter + 1;
    std::cout << counter << std::endl;
    return 0;
}
```
<!-- error: 02_no_file_scope.clg -->
```
error[semantic/forbidden-construct]: statement at file scope
  --> 02_no_file_scope.clg:1:1
  help: File-scope variables are not supported (global mutable state has no owner and no lifetime the checker can reason about). Put the value in a function, or pass it as a parameter or a class field.
```

**Reference parameters, fields and returns.** `ref` is for locals. A parameter always takes the argument by value (an object is handed over; a number is copied), so to get a number *out*
of a function, return it, or put the result in an object field.

<!-- example: 02_no_ref_param.clg -->
```cpp
void bump(ref int n) {
    n = n + 1;
}

int main() {
    int a = 1;
    bump(a);
    return 0;
}
```
<!-- error: 02_no_ref_param.clg -->
```
Error: error[semantic/observer_param]: `ref` observers cannot be function parameters (line 1, column 11). Function arguments always transfer ownership (move); pass by value and let the caller keep observers local.
```
<!-- example: 02_no_ref_field.clg -->
```cpp
class Box {
public:
    int v;
    Box(int x) { v = x; }
};

class Holder {
public:
    ref Box target;
    Holder() { }
};

int main() {
    return 0;
}
```
<!-- error: 02_no_ref_field.clg -->
```
Error: Syntax error at line 9, column 9, node type 'ERROR', near: Box. Suggestion: check whether the statement is missing a trailing ';'.
```
<!-- example: 02_no_ref_return.clg -->
```cpp
class Box {
public:
    int v;
    Box(int x) { v = x; }
};

ref Box pick(Box a) {
    return a;
}

int main() {
    return 0;
}
```
<!-- error: 02_no_ref_return.clg -->
```
Error: Syntax error at line 7, column 1, node type 'ERROR', near: ref Box pick(Box a) {
```

(`ref` parameters exist only in `ext_fn` declarations of C++ you call, [chapter 11](11-talking-to-cpp.md).)

**`union`.**

<!-- example: 02_no_union.clg -->
```cpp
union Number {
    int whole;
    float part;
};

int main() {
    return 0;
}
```
<!-- error: 02_no_union.clg -->
```
Error: Syntax error at line 1, column 1, node type 'ERROR', near: union Number {
    int whole;
    float part;
}; Suggestion: raw `union` is forbidden (hatch #17) -- it allows reading a different member than was written, which is undefined behavior C++ cannot catch. Use a class with distinct fields instead.
```

**Library calls the compiler cannot follow**, such as `vector::front`. Every call must be to something CeeLing compiled, or to a library method whose facts say what it hands back in a shape the
checker can track. `front()` returns a reference to an element, and only `v[i]` is tracked as an observer so far, so it is refused (write `v[0]`); chapter 3 says how the admitted methods are decided.

<!-- example: 02_no_vector_front.clg -->
```cpp
int main() {
    std::vector<int> v;
    v.push_back(1);
    int x = v.front();
    std::cout << x << std::endl;
    return 0;
}
```
<!-- error: 02_no_vector_front.clg -->
```
error[semantic/forbidden-construct]: `front` is not available on `std::vector<int>`: it returns a reference to an element, and only `v[i]` is tracked as an observer so far
  --> 02_no_vector_front.clg:4:13
  help: CeeLing admits a method of the standard library only when the checker can follow what it hands back; the rule and the scanned facts are in library_summary.cpp and facts/std-*.clfacts.
```

Also: a call on a library call's result is rejected when the second call is not itself known. Your own functions chain freely (see "Classes"):

<!-- example: 02_no_chain_on_library_result.clg -->
```cpp
int main() {
    string s = "hello";
    int n = s.substr(1, 3).size();
    std::cout << n << std::endl;
    return 0;
}
```
<!-- error: 02_no_chain_on_library_result.clg -->
```
error[semantic/forbidden-construct]: call to unknown function/method `size`
  --> 02_no_chain_on_library_result.clg:3:13
  help: CeeLing has no borrow/ownership summary for anything it did not compile itself; unknown calls are rejected by default rather than silently trusted (CLAUDE.md "default-reject, never default-allow"). Define `size` in CeeLing source, or call an existing CeeLing function/method instead.
```

**Default arguments.** Write the overloads (they are supported):

<!-- example: 02_no_default_argument.clg -->
```cpp
int add(int x, int y = 2) {
    return x + y;
}

int main() {
    std::cout << add(1) << std::endl;
    return 0;
}
```
<!-- error: 02_no_default_argument.clg -->
```
Error: Syntax error at line 1, column 22, node type 'ERROR', near: = 2. Suggestion: check whether the statement is missing a trailing ';'.
```

**Static members.** Move shared state into an object that is passed or held.

<!-- example: 02_no_static_member.clg -->
```cpp
class Counter {
public:
    static int total;
    Counter() { }
};

int main() {
    return 0;
}
```
<!-- error: 02_no_static_member.clg -->
```
Error: Syntax error at line 3, column 12, node type 'ERROR', near: int. Suggestion: check whether the statement is missing a trailing ';'.
```

**Multiple inheritance.** One base only; share behaviour by composition.

<!-- example: 02_no_multiple_inheritance.clg -->
```cpp
class Walker {
public:
    Walker() { }
};

class Swimmer {
public:
    Swimmer() { }
};

class Duck : public Walker, public Swimmer {
public:
    Duck() { }
};

int main() {
    return 0;
}
```
<!-- error: 02_no_multiple_inheritance.clg -->
```
Error: Syntax error at line 11, column 14, node type 'ERROR', near: public Walker,. Suggestion: check whether the statement is missing a trailing ';'.
```

**Compound assignment, postfix `++`, the C-style `for`, `switch`, and the C-style cast.** Write `x = x + 2;`, `x = x + 1;`, a `while` or a range-for, an `if` chain, and a conversion
function such as `int(d)` for numbers (chapter 3).

<!-- example: 02_no_compound_assign.clg -->
```cpp
int main() {
    int x = 1;
    x += 2;
    return 0;
}
```
<!-- error: 02_no_compound_assign.clg -->
```
Error: Syntax error at line 3, column 8, node type 'ERROR', near: =. Suggestion: check whether the statement is missing a trailing ';'.
```
<!-- example: 02_no_postfix_increment.clg -->
```cpp
int main() {
    int x = 1;
    x++;
    return 0;
}
```
<!-- error: 02_no_postfix_increment.clg -->
```
Error: Syntax error at line 3, column 6, node type 'ERROR', near: ++. Suggestion: check whether the statement is missing a trailing ';'.
```
<!-- example: 02_no_c_for.clg -->
```cpp
int main() {
    for (int i = 0; i < 3; i = i + 1) {
        std::cout << i << std::endl;
    }
    return 0;
}
```
<!-- error: 02_no_c_for.clg -->
```
Error: Syntax error at line 1, column 12, node type 'ERROR', near: {
    for (int i = 0; i < 3; i = i + 1)
```
<!-- example: 02_no_switch.clg -->
```cpp
int main() {
    int x = 1;
    switch (x) {
        case 1: break;
    }
    return 0;
}
```
<!-- error: 02_no_switch.clg -->
```
Error: Syntax error at line 1, column 12, node type 'ERROR', near: {
    int x = 1;
    switch (x)
```
<!-- example: 02_no_c_cast.clg -->
```cpp
int main() {
    double d = 2.7;
    int i = (int)d;
    return 0;
}
```
<!-- error: 02_no_c_cast.clg -->
```
Error: Syntax error at line 3, column 18, node type 'ERROR', near: d. Suggestion: check whether the statement is missing a trailing ';'.
```

**Also tried, all rejected** (as syntax errors unless noted): block comments `/* */`; `and` / `or`; hexadecimal literals (`0xFF`) and the float suffix (`1.5f`); `const` on a variable; arrays
(`int a[5]`); `do ... while`; `goto`; `^`; a forward declaration of a free function (`int f(int);`); `using` and `typedef` (reported as a file-scope statement); `#define`; `private:` and
`protected:`; pure virtual (`= 0`); `friend`; a lambda `[](int x) { ... }` (closures are [chapter 9](09-closures.md)); initialiser lists; `main` with parameters or `void main()` (g++ rejects
the latter); `static_cast`, `dynamic_cast` and `sizeof` (rejected as unknown calls).

**Accepted, though you might expect otherwise.** Overloading (constructors, methods, free functions) works. A call on the result of one of your own calls works. `->` works. `auto` works for
numbers. `true` and `false` are `bool` values.

## Summary

| In C++ you decide, at each use | In CeeLing |
|---|---|
| value, raw pointer, `unique_ptr`, `shared_ptr`, `weak_ptr` | one handle syntax; the compiler picks the type from the ownership shape |
| `p->x` versus `x.y`, `*p`, `&x` | `.` everywhere (`->` is a spelling of `.`); no `*`, no `&` |
| `delete`, a virtual destructor, a long-list destructor | none to write; the compiler writes the right ones |
| `std::move` | none; moves are inserted and tracked |
| is `p` null? | an empty handle aborts at the line on dereference |
| `==` on pointers or on values | handles compare by identity |
| `&` in a range-for | the element type decides: object handle, or copy for numbers and structs |
| forward declarations, initialiser lists in the right order | not needed / order kept |

The edge of all this is the same as everywhere in the guide: the checks cover CeeLing code, not the libraries it calls or the C++ you link ([chapter 13](13-contract-and-threats.md)).
