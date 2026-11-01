# 6. Dangling references

A reference is dangling when the object it names has been destroyed. This is the bug C++ has most of and the one CeeLing was built to remove: **if a program compiles under CeeLing, no
reference in it names an object that is gone when the reference is used.** This chapter says what "reference" means in CeeLing, when an object is gone, what the compiler does about each
case, and, in the last section, which cases it does not yet see. How the checker is built is [chapter 14](14-how-it-works.md); who owns what is [chapter 4](04-ownership.md); handing an object
to a function is [chapter 5](05-moves.md).

Every program here is compiled by the real compiler each time the guide is checked. The C++ twins compile with `g++ -Wall -Wextra`; the file header of each says whether g++ speaks up.

## What C++ cannot tell you

A C++ reference or raw pointer is a number with a type. The type says what it points at, never whether that is still there. Smart pointers decide who calls `delete`; they say nothing
about a reference taken from them. So C++ code that is perfectly well typed, and that compilers accept without a word, can read freed memory. The rest of the chapter is that one idea,
met in a dozen shapes.

<!-- example: 06_same_type_two_lifetimes.cpp -->
```cpp
// Run-time effect: the second line reads an object the assignment destroyed. The two references have the same type.
#include <iostream>
#include <memory>

struct Node {
    int v;
    explicit Node(int x) : v(x) {}
};

int main() {
    auto a = std::make_shared<Node>(1);
    Node& fine = *a;
    std::cout << fine.v << std::endl;       // fine: a still owns the object
    Node& stale = *a;
    a = std::make_shared<Node>(2);          // the object stale names is destroyed
    std::cout << stale.v << std::endl;      // undefined behaviour; same type as fine
    return 0;
}
```

Both references have the type `Node&`. One is fine and one names a destroyed object, and no compiler can tell them apart by looking at the types.

CeeLing can do something about it because it is a smaller language that controls the whole pipeline. It forbids what C++ must allow (a reference that can be stored, returned or passed
on), and for the one kind of reference it keeps, it knows at every statement which object that reference depends on and whether that object is still alive.

## The `ref` observer

Start with what an ordinary variable is. In CeeLing a variable of a class type is an *owner*: it holds the object, or shares it with other owners. The compiler decides which, and writes the
smart pointer ([chapter 4](04-ownership.md)). Copying a variable makes another owner, so the object cannot disappear while an owner is left:

<!-- example: 06_plain_copy_owns.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

int main() {
    Node a = new Node(1);
    Node b = a;               // no ref: b is a second owner of the same object
    a = new Node(2);          // a now owns a new object; the first one lives on, through b
    std::cout << b.v << std::endl;
    std::cout << a.v << std::endl;
    return 0;
}
```
<!-- output: 06_plain_copy_owns.clg -->
```
1
2
```

`b` is a second owner, so the first object lives on after `a` takes a new one. Nothing can dangle in this program, and there is no keyword in it. That is the default, and most code never needs
anything else.

What owners cannot express is *looking* at an object without keeping it alive, as a C++ `T&` or `T*` does. Java has no such thing, C++ code uses it everywhere, and it is where the
danger is. CeeLing has one spelling for it, `ref`:

* `ref Node n = a;` makes `n` an **observer** of `a`'s object. `n` owns nothing. It is valid exactly as long as the owner it was taken from is.
* It needs a keyword of its own because the compiler must be told. `Node n = a;` already means "second owner", and nothing in a C++ `T&` says whether the programmer meant a view or a
  copy of the handle. With `ref`, every observer in a program is marked, so the analysis has a complete list of the things it must keep track of.

### What an observer may look at

An observer is bound to one of four things, and the compiler tracks all four:

<!-- example: 06_observer_kinds.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

int main() {
    Node a = new Node(1);
    std::vector<Node> v;
    v.push_back(new Node(2));
    ref Node later = nullptr;         // an observer that observes nothing yet
    ref Node ofLocal = a;             // a local object
    ref Node ofElement = v[0];        // an element of a container local
    ref Node ofObserver = ofLocal;    // another observer: it inherits the owner
    later = ofElement;                // an observer may be re-pointed
    std::cout << ofLocal.v << " " << ofElement.v << " " << ofObserver.v << " " << later.v << std::endl;
    return 0;
}
```
<!-- output: 06_observer_kinds.clg -->
```
1 2 1 2
```

| Binding | Example | Observes |
|---|---|---|
| a local object | `ref Node n = a;` | `a` |
| an element of a container local | `ref Node n = v[0];` | the container `v` (the whole container, not that one slot) |
| another observer | `ref Node m = n;` | whatever `n` observes; if `n` is already dead, so is `m` |
| `nullptr` | `ref Node n = nullptr;` | nothing yet; it may be pointed at something later with `n = a;` |

Here is what the compiler writes for that program:

<!-- generated: 06_observer_kinds.clg "int main()" -->
```cpp
int main() {
    std::unique_ptr<Node> a = std::make_unique<Node>(1);
    std::vector<std::unique_ptr<Node>> v;
    v.push_back(std::make_unique<Node>(2));
    Node* later = nullptr;
    Node* ofLocal = (a).get();
    Node* ofElement = (cling::at(v, 0, 13)).get();
    Node* ofObserver = ofLocal;
    later = ofElement;
    std::cout << cling::nn(ofLocal, "line 16")->v << " " << cling::nn(ofElement, "line 16")->v << " " << cling::nn(ofObserver, "line 16")->v << " " << cling::nn(later, "line 16")->v << std::endl;
    return 0;
}
```

An observer becomes a raw pointer in the generated C++. That is the one place a raw pointer exists, and it is why the checker below is not optional: CeeLing bans raw pointers in source
([chapter 8](08-sealed-escape-hatches.md)) because they carry no lifetime, and it can emit one here only because it has proved, statement by statement, that it is never used after its object
is gone. Every access through it also goes through the null check of [chapter 7](07-run-time-checks.md). An observer that observes nothing is therefore a clean stop, not a crash:

<!-- example: 06_null_observer.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

int main() {
    ref Node b = nullptr;
    std::cout << b.v << std::endl;     // b observes nothing: the null check stops the program
    return 0;
}
```
<!-- output: 06_null_observer.clg -->
```
cling: null dereference at line 9
```

### What an observer may not be

The reason `ref` is safe is as much about what it cannot do as what it can. An observer can only live in a local variable of the function that made it. It cannot be passed, returned or
stored, because each of those lets it outlive the object it depends on, and the analysis is per function: it cannot follow a `ref` out of one.

| Written | Result | Why |
|---|---|---|
| `void show(ref Node n)` | rejected | an argument would carry the observer into a function whose lifetime the caller's analysis cannot see |
| `ref Node r;` as a field | rejected (a syntax error: there is no such field) | a field can outlive the local object it was taken from |
| `ref Node f()` as a return type | rejected (a syntax error) | the caller would hold a reference into a frame that is gone |
| `std::vector<ref Node>` | rejected (a syntax error) | a container can outlive its elements' owners, and an element has no name to track |
| `ref Point p = a;` where `Point` is a `struct` | rejected | a `struct` is a value, copied rather than owned; there is no owner to observe |
| `ref Item c = parent.child;`, `= parent.getChild();`, `= make();` | rejected | the owner is a field, or a temporary that dies at the end of the statement; no local owner exists whose death the checker could watch |

<!-- example: 06_ref_param.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

void show(ref Node n) {
    std::cout << n.v << std::endl;
}

int main() {
    Node a = new Node(1);
    show(a);
    return 0;
}
```
<!-- error: 06_ref_param.clg -->
```
Error: error[semantic/observer_param]: `ref` observers cannot be function parameters (line 7, column 11). Function arguments always transfer ownership (move); pass by value and let the caller keep observers local.
```

The two that are syntax errors, a `ref` return type and a `ref` inside a container's type argument:

<!-- example: 06_ref_return.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

ref Node pick(Node a) {
    return a;
}

int main() {
    return 0;
}
```
<!-- error: 06_ref_return.clg -->
```
Error: Syntax error at line 7, column 1, node type 'ERROR', near: ref Node pick(Node a) {
```

<!-- example: 06_ref_in_container.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

int main() {
    std::vector<ref Node> v;
    return 0;
}
```
<!-- error: 06_ref_in_container.clg -->
```
Error: Syntax error at line 8, column 17, node type 'ERROR', near: ref. Suggestion: check whether the statement is missing a trailing ';'.
```

The three rejections that need most explanation are the last three, because each is something C++ lets through. The first is a reference to something owned by a field:

<!-- example: 06_bound_to_field.cpp -->
```cpp
// Run-time effect: c names the object the field held; clearing the field destroyed it.
#include <iostream>
#include <memory>
#include <vector>
#include <string>

struct Node {
    int v;
    explicit Node(int x) : v(x) {}
};

struct Item {
    int v;
    std::unique_ptr<Item> child;
    explicit Item(int x) : v(x) {}
};

int main() {
    Item parent(1);
    parent.child = std::make_unique<Item>(42);
    Item& c = *parent.child;
    parent.child = nullptr;                 // destroys the child
    std::cout << c.v << std::endl;          // undefined behaviour
    return 0;
}
```

**Why C++ does not stop it.** `c` is an `Item&`. That the object belongs to `parent.child` and that `parent.child = nullptr` destroys it is a fact about the program, not about the type.
g++ is silent.

**What CeeLing does.** A `ref` may only be taken from a *local* owner or a container local, because only those have a death event the checker can see (next section). A field is changed
by any method that has the object, and a call result is gone at the semicolon. So the binding itself is the error, not the later use:

<!-- example: 06_ref_bound_wrongly.clg -->
```cpp
class Item {
public:
    int v;
    Item child;
    Item(int x) { v = x; child = nullptr; }
    Item getChild() { return child; }
};

Item make() {
    return new Item(9);
}

int main() {
    Item parent = new Item(1);
    parent.child = new Item(42);
    ref Item viaField = parent.child;
    ref Item viaMethod = parent.getChild();
    ref Item viaCall = make();
    return 0;
}
```
<!-- error: 06_ref_bound_wrongly.clg -->
```
error[semantic/use-after-move]: `ref viaField` is bound to `parent->child`, whose owner cannot be tracked
  --> 06_ref_bound_wrongly.clg:16:5
  help: a `ref` may only observe an object local, an element `v[i]` of a container local, another `ref`, or nullptr. A field (`p.child`) or a call result (`p.getChild()`, `make()`) has no local owner whose death the checker can see, so the observer could silently dangle. Use a plain (owning/sharing) local instead: `viaField = ...` declared without `ref`.
error[semantic/use-after-move]: `ref viaMethod` is bound to `getChild()`, whose owner cannot be tracked
  --> 06_ref_bound_wrongly.clg:17:5
  help: a `ref` may only observe an object local, an element `v[i]` of a container local, another `ref`, or nullptr. A field (`p.child`) or a call result (`p.getChild()`, `make()`) has no local owner whose death the checker can see, so the observer could silently dangle. Use a plain (owning/sharing) local instead: `viaMethod = ...` declared without `ref`.
error[semantic/use-after-move]: `ref viaCall` is bound to `make()`, whose owner cannot be tracked
  --> 06_ref_bound_wrongly.clg:18:5
  help: a `ref` may only observe an object local, an element `v[i]` of a container local, another `ref`, or nullptr. A field (`p.child`) or a call result (`p.getChild()`, `make()`) has no local owner whose death the checker can see, so the observer could silently dangle. Use a plain (owning/sharing) local instead: `viaCall = ...` declared without `ref`.
```

The fix the message suggests is an ordinary local (`Item c = parent.child;`), which is a second owner and cannot dangle. A `struct` is rejected for a different reason: it is copied, so
`Point b = a;` is already independent and there is nothing to observe:

<!-- example: 06_ref_value_type.clg -->
```cpp
struct Point {
public:
    int v;
    Point(int x) { v = x; }
};

int main() {
    Point a = new Point(1);
    ref Point b = a;
    std::cout << b.v << std::endl;
    return 0;
}
```
<!-- error: 06_ref_value_type.clg -->
```
error[semantic/use-after-move]: `ref` on value type `Point` (`b`) has no owner to observe
  --> 06_ref_value_type.clg:9:5
  help: `Point` is a value type (copied, not smart-pointer-backed); CeeLing cannot form an observer of it. Use a plain copy instead: `Point b = ...;`
```

**Where it stops.** The restrictions cost something: "borrow this into a helper function" cannot be written. You pass the object (which hands it over, chapter 5), or you write the helper
as a method of the class. Two further misuses are not diagnosed by CeeLing at all, but they cannot produce a running program either, because the generated C++ does not compile: passing a `ref` as
an argument (`sink(b)`), and copying an observer into an owner (`Node c = b;`). `ref int b = a;` on a primitive behaves the same way. These were checked by hand against the compiler
and are not in the guide's checked set.

## When an object is gone: the death events

The checker keeps, for each observer, the owner it depends on. An observer is dead when that owner has let go of the object. Five things make an owner let go. After the event, any use of
the observer (reading a field, calling a method, passing it, copying it) is an error. Each event comes with the C++ that gets it wrong and then the CeeLing program that is refused.

The compiler's message is the same for all of them: `use of dangling observer `n` (its owner `a` is gone)`. (The label printed before it is `use-after-move`, the checker's one category for
"this name no longer refers to a live object". It is the same check as [chapter 5](05-moves.md)'s.)

### The owner is moved away

<!-- example: 06_moved.cpp -->
```cpp
// Run-time effect: sink destroys the object; the last line reads it.
#include <iostream>
#include <memory>
#include <vector>
#include <string>

struct Node {
    int v;
    explicit Node(int x) : v(x) {}
};

void sink(std::unique_ptr<Node>) {}

int main() {
    auto root = std::make_unique<Node>(1);
    Node& n = *root;
    sink(std::move(root));                  // the object is destroyed inside sink
    std::cout << n.v << std::endl;          // undefined behaviour
    return 0;
}
```

**Why C++ does not stop it.** `std::move` is a cast and `sink` is an ordinary function. Nothing connects `n` to `root`.

**What CeeLing does.** Handing an object to a function ends the caller's ownership ([chapter 5](05-moves.md)), and an owner's observers die with it:

<!-- example: 06_moved.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

void sink(Node n) { }       // takes the object; nothing else owns it afterwards

int main() {
    Node root = new Node(1);
    ref Node n = root;
    sink(root);             // death event: root hands its object over
    std::cout << n.v << std::endl;
    return 0;
}
```
<!-- error: 06_moved.clg -->
```
error[semantic/use-after-move]: use of dangling observer `n` (its owner `root` is gone)
  --> 06_moved.clg:13:18
  help: `n` observes `root`, which no longer has a live owner
```

The dependence is on one particular owner, not on "any object of that type". Another owner dying does nothing to this observer, so the check does not refuse sensible code:

<!-- example: 06_other_owner_dies.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

void sink(Node n) { }

int main() {
    Node a = new Node(1);
    Node b = new Node(2);
    ref Node n = a;         // n observes a, and only a
    sink(b);                // b dies; that is not n's owner
    std::cout << n.v << std::endl;
    return 0;
}
```
<!-- output: 06_other_owner_dies.clg -->
```
1
```

### The owner is given a new object

<!-- example: 06_reassign.cpp -->
```cpp
// Run-time effect: the last line reads an object that the assignment above it destroyed.
#include <iostream>
#include <memory>
#include <vector>
#include <string>

struct Node {
    int v;
    explicit Node(int x) : v(x) {}
};

int main() {
    auto a = std::make_shared<Node>(1);
    Node& n = *a;                           // n looks at the first object
    a = std::make_shared<Node>(2);          // the first object is destroyed here
    std::cout << n.v << std::endl;          // undefined behaviour
    return 0;
}
```

**Why C++ does not stop it.** Assignment to a smart pointer is another ordinary call. It happens to destroy the old object.

**What CeeLing does.** Assigning to an owner is a death event for everything that observed what it held:

<!-- example: 06_reassign.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

int main() {
    Node a = new Node(1);
    ref Node n = a;
    a = new Node(2);        // death event: the first object is released here
    std::cout << n.v << std::endl;
    return 0;
}
```
<!-- error: 06_reassign.clg -->
```
error[semantic/use-after-move]: use of dangling observer `n` (its owner `a` is gone)
  --> 06_reassign.clg:11:18
  help: `n` observes `a`, which no longer has a live owner
```

### The owner goes out of scope

<!-- example: 06_scope_exit.cpp -->
```cpp
// Run-time effect: the last line reads an object that died at the closing brace.
#include <iostream>
#include <memory>
#include <vector>
#include <string>

struct Node {
    int v;
    explicit Node(int x) : v(x) {}
};

int main() {
    Node* n = nullptr;
    {
        auto inner = std::make_unique<Node>(1);
        n = inner.get();
    }                                       // inner is destroyed here
    std::cout << n->v << std::endl;         // undefined behaviour
    return 0;
}
```

**Why C++ does not stop it.** `.get()` hands out a pointer that is free to be stored anywhere. The scope that ends the object is a different place from the one that uses the pointer.

**What CeeLing does.** An owner declared inside a block dies at the closing brace, and an observer that was pointed at it from outside the block dies with it:

<!-- example: 06_scope_exit.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

int main() {
    ref Node n = nullptr;
    if (1) {
        Node inner = new Node(1);
        n = inner;
    }                       // death event: inner goes out of scope
    std::cout << n.v << std::endl;
    return 0;
}
```
<!-- error: 06_scope_exit.clg -->
```
error[semantic/use-after-move]: use of dangling observer `n` (its owner `inner` is gone)
  --> 06_scope_exit.clg:13:18
  help: `n` observes `inner`, which no longer has a live owner
```

### The container changes under the observer

<!-- example: 06_container_mutation.cpp -->
```cpp
// Run-time effect: after the second push_back the vector may have moved its elements; first points at the old storage.
#include <iostream>
#include <vector>

int main() {
    std::vector<int> v;
    v.push_back(1);
    int& first = v[0];
    v.push_back(2);                         // may reallocate
    std::cout << first << std::endl;        // undefined behaviour if it did
    return 0;
}
```

This one has its own section below, [Containers](#containers-and-the-calls-that-invalidate-references), because the question "which calls?" has a longer answer. The CeeLing version:

<!-- example: 06_container_mutation.clg -->
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
    v.push_back(new Node(2));       // death event: push_back is on the list of invalidating calls
    std::cout << n.v << std::endl;
    return 0;
}
```
<!-- error: 06_container_mutation.clg -->
```
error[semantic/use-after-move]: use of dangling observer `n` (its owner `v` is gone)
  --> 06_container_mutation.clg:12:18
  help: `n` observes `v`, which no longer has a live owner
```

### An exception may have cut the block short

<!-- example: 06_try_catch.cpp -->
```cpp
// Run-time effect: the object n looks at was destroyed by the assignment inside the try block.
#include <iostream>
#include <memory>
#include <vector>
#include <string>

struct Node {
    int v;
    explicit Node(int x) : v(x) {}
};

int main() {
    auto a = std::make_shared<Node>(1);
    Node& n = *a;
    try {
        a = std::make_shared<Node>(2);      // destroys the first object
        throw std::string("failed");
    } catch (const std::string& e) {
        std::cout << e << std::endl;
    }
    std::cout << n.v << std::endl;          // undefined behaviour
    return 0;
}
```

**Why C++ does not stop it.** Control flow through `throw` is invisible to the type system, and the reference is still in scope.

**What CeeLing does.** The checker cannot know how far into a `try` body execution got before the exception. It therefore treats the body as "ran, or did not run" and takes the worse of the
two. Anything that could have killed an owner inside the body has maybe killed it:

<!-- example: 06_try_catch.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

int main() {
    Node a = new Node(1);
    ref Node n = a;
    try {
        a = new Node(2);            // may or may not have run when the exception arrives
        throw "failed";
    } catch (string e) {
        std::cout << e << std::endl;
    }
    std::cout << n.v << std::endl;
    return 0;
}
```
<!-- error: 06_try_catch.clg -->
```
error[semantic/use-after-move]: use of dangling observer `n` (its owner `a` is gone)
  --> 06_try_catch.clg:16:18
  help: `n` observes `a`, which no longer has a live owner
```

A `try` body that does nothing to the owner is accepted, so the rule does not tax every function that uses exceptions:

<!-- example: 06_try_untouched.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

int main() {
    Node a = new Node(1);
    ref Node n = a;
    try {
        std::cout << "no throw here" << std::endl;
    } catch (string e) {
        std::cout << e << std::endl;
    }
    std::cout << n.v << std::endl;
    return 0;
}
```
<!-- output: 06_try_untouched.clg -->
```
no throw here
1
```

**Where the death events stop.** They are the five above. An owner can also lose its object in ways CeeLing has no syntax for (`delete`, `.reset()`, `.get()` and a raw pointer are all
sealed, [chapter 8](08-sealed-escape-hatches.md)), and in ways it does not see, which are the last section of this chapter.

## Returning a reference to a local

<!-- example: 06_return_local.cpp -->
```cpp
// Run-time effect: the caller receives a reference into a stack frame that has been popped.
#include <iostream>

struct Node {
    int v;
    explicit Node(int x) : v(x) {}
};

Node& makeDangling() {
    Node n(1);
    return n;                               // g++ -Wall warns about this direct form; it stays a warning
}

int main() {
    Node& r = makeDangling();
    std::cout << r.v << std::endl;          // undefined behaviour
    return 0;
}
```

**Why C++ does not stop it.** A function may return a reference. Whether it is to a local, a parameter or something longer lived is a property of the body, and the type says `Node&` in all
three. g++ catches this direct form and says so, but only as a warning, and only while the return and the local are visible together; return `n` through a helper and the warning goes away.

**What CeeLing does.** It has no `ref` return type. A function's result is always an owner (the object moves out, or is shared), so there is no way to hand a borrow back. Returning the
*name of an observer* is then refused, while returning the owner is the ordinary factory pattern:

<!-- example: 06_return_local.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

Node makeDangling() {
    Node n = new Node(1);
    ref Node b = n;
    return b;
}

int main() {
    return 0;
}
```
<!-- error: 06_return_local.clg -->
```
error[semantic/use-after-move]: returning observer `b` (a `ref`) escapes its function
  --> 06_return_local.clg:10:12
  help: `b` is a non-owning reference into this function's own stack frame; CeeLing has no `ref` return type, so the caller would receive a reference to an object that no longer exists. Return the owner itself, or restructure so the caller creates its own observer.
```

<!-- example: 06_return_owner.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

Node makeNode() {
    Node n = new Node(1);
    ref Node b = n;
    std::cout << b.v << std::endl;
    return n;               // the owner moves out; the observer ends with the function
}

int main() {
    Node result = makeNode();
    std::cout << result.v << std::endl;
    return 0;
}
```
<!-- output: 06_return_owner.clg -->
```
1
1
```

**Where it stops.** Nothing: with no `ref` return and no `ref` parameter, no path leads an observer out of its function, so the rule needs no analysis beyond the one name check.

## A reference stored in a field

<!-- example: 06_ref_field.cpp -->
```cpp
// Run-time effect: the Holder outlives the object its member refers to.
#include <iostream>
#include <memory>
#include <vector>
#include <string>

struct Node {
    int v;
    explicit Node(int x) : v(x) {}
};

struct Holder {
    Node& r;                                // a reference stored in a longer-lived object
    explicit Holder(Node& n) : r(n) {}
};

int main() {
    auto a = std::make_unique<Node>(1);
    Holder h(*a);
    a.reset();                              // the object is gone, the holder still names it
    std::cout << h.r.v << std::endl;        // undefined behaviour
    return 0;
}
```

**Why C++ does not stop it.** A reference member is legal, and the holder and the object it names have independent lifetimes. This is the shape of most long-lived dangling bugs: the
reference is stored in one place and used in another, far from the code that destroys the object.

**What CeeLing does.** It does not analyse this; it removes it. There is no `ref` field in the grammar, and together with "no `ref` parameter" and "no `ref` in a container" there is no remaining
syntactic path to put an observer into an object:

<!-- example: 06_ref_field.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

class Holder {
public:
    ref Node r;
};

int main() {
    return 0;
}
```
<!-- error: 06_ref_field.clg -->
```
Error: Syntax error at line 9, column 14, node type 'ERROR', near: r. Suggestion: check whether the statement is missing a trailing ';'.
```

A field of a class type is an owner, which keeps the object alive (a plain field store is a share, not a hand-over, so storing `b` into a field does not stop you using `b`). The message
is a plain syntax error, because from the parser's side that is what a `ref` field is.

## Function summaries: when the result is the argument

An observer need not be written with `ref`. If a function returns one of its own parameters, the result *is* that object, and an owner of the result is a reference to what the argument held.
C++ has this exact shape in every function that returns `const T&` to a parameter:

<!-- example: 06_summary.cpp -->
```cpp
// Run-time effect: r is a reference to the first parameter's object, which x gave up.
#include <iostream>
#include <memory>
#include <vector>
#include <string>

struct Node {
    int v;
    explicit Node(int x) : v(x) {}
};

Node& pick(Node& a, Node&) { return a; }

int main() {
    auto x = std::make_unique<Node>(1);
    auto y = std::make_unique<Node>(2);
    Node& r = pick(*x, *y);                 // r is x's object
    x = std::make_unique<Node>(3);          // x's first object is destroyed
    std::cout << r.v << std::endl;          // undefined behaviour
    return 0;
}
```

**Why C++ does not stop it.** The signature `Node& pick(Node&, Node&)` does not say which parameter the result aliases. Rust makes you write that in lifetime annotations; C++ leaves the
fact in the body.

**What CeeLing does.** It reads the body once and records the fact (a *summary*): "the result of `pick` is its first parameter". CeeLing has no `ref` parameters, so every object argument is handed over, and
the only fact that varies from function to function is what, if anything, the result aliases. Each call site then treats the variable that receives the result as an observer of the owner
whose object came back:

<!-- example: 06_summary_ok.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

Node pick(Node a, Node b) {
    return a;               // summary of pick: the result is its first parameter
}

int main() {
    Node x = new Node(1);
    Node y = new Node(2);
    Node r = pick(x, y);    // so r refers to the object x referred to
    std::cout << r.v << std::endl;
    return 0;
}
```
<!-- output: 06_summary_ok.clg -->
```
1
```

<!-- example: 06_summary_dangles.clg -->
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
    Node r = pick(x, y);    // r follows x
    x = new Node(3);        // x lets go of the object r follows
    std::cout << r.v << std::endl;
    return 0;
}
```
<!-- error: 06_summary_dangles.clg -->
```
error[semantic/use-after-move]: use of dangling observer `r` (its owner `x` is gone)
  --> 06_summary_dangles.clg:16:18
  help: `r` observes `x`, which no longer has a live owner
```

Nobody wrote `ref` and no lifetime was annotated; the summary was computed. The summary also follows a *chain* of calls. In `makeHolder().getInner()` the receiver is a temporary, and the
analysis resolves what class it is and what that method returns. A chained call on a CeeLing class is safe because the result of every CeeLing call is an owner, never a pointer into the
temporary that is gone by the time the next call runs:

<!-- example: 06_chained_ok.clg -->
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
    Inner i = makeHolder().getInner();      // the first call's result is a temporary; the second call still works
    std::cout << i.v << std::endl;
    return 0;
}
```
<!-- output: 06_chained_ok.clg -->
```
7
```

The same lookup finds the summary of a method reached through a chain, so the dangling use is still caught:

<!-- example: 06_chained_dangles.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

class Picker {
public:
    Picker() { }
    Node pick(Node a, Node b) {
        return a;
    }
};

class PickerFactory {
public:
    PickerFactory() { }
    Picker makePicker() {
        return new Picker();
    }
};

int main() {
    PickerFactory factory = new PickerFactory();
    Node x = new Node(1);
    Node y = new Node(2);
    Node r = factory.makePicker().pick(x, y);    // the summary is found through the chain
    x = new Node(3);
    std::cout << r.v << std::endl;
    return 0;
}
```
<!-- error: 06_chained_dangles.clg -->
```
error[semantic/use-after-move]: use of dangling observer `r` (its owner `x` is gone)
  --> 06_chained_dangles.clg:29:18
  help: `r` observes `x`, which no longer has a live owner
```

Summaries are CeeLing's answer to "no lifetime annotations" (the reason is in [chapter 14](14-how-it-works.md)). They are exact for code CeeLing compiled. For code it did not (a C++ library, an
`ext_fn`), the only facts are the ones someone wrote down, and what is not written down is refused ([chapter 11](11-talking-to-cpp.md)).

**Where it stops.** The summary has one fact: which parameter the result aliases, or none. A function whose result aliases a *field* of an argument, or either of two arguments depending on a
condition, is not described by it; the first is rejected by the ordinary rules and the second gets only the first parameter that the return statements agree on. Cases the analysis cannot
settle are errors, not guesses.

## Branches

An `if` makes the later code depend on a path the compiler cannot know. C++ lets a pointer be assigned in one arm to an object that dies at the end of that arm:

<!-- example: 06_branch.cpp -->
```cpp
// Run-time effect: when cond is true, obs names b, which was destroyed at the end of the if block.
#include <iostream>
#include <memory>
#include <vector>
#include <string>

struct Node {
    int v;
    explicit Node(int x) : v(x) {}
};

int main() {
    Node a(1);
    Node* obs = &a;
    bool cond = true;
    if (cond) {
        Node b(2);
        obs = &b;
    }                                       // b is destroyed here
    std::cout << obs->v << std::endl;       // right when cond is false, undefined behaviour when it is true
    return 0;
}
```

**Why C++ does not stop it.** The program is correct when `cond` is false. A compiler that rejected every such pointer would reject most of the language.

**What CeeLing does.** Both arms are checked from the same starting state, and then merged. An effect in *either* arm still holds after the `if`: moved stays moved, dead stays dead. Two arms
that leave an observer pointing at *different* owners are ambiguous, so the observer is refused afterward, even though both owners are alive:

<!-- example: 06_branch_ambiguous.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

int main() {
    Node a = new Node(1);
    Node b = new Node(2);
    ref Node obs = a;
    bool cond = true;
    if (cond) {
        obs = b;
    } else {
        obs = a;
    }
    std::cout << obs.v << std::endl;    // obs follows b or a, depending on cond
    return 0;
}
```
<!-- error: 06_branch_ambiguous.clg -->
```
error[semantic/use-after-move]: use of dangling observer `obs` (its owner `b` is gone)
  --> 06_branch_ambiguous.clg:17:18
  help: `obs` observes `b`, which no longer has a live owner
```

The merge must not be too eager either. An effect inside the `if` arm must not be visible inside the `else` arm, which is a different path. The following is accepted, because the use is in
the arm where `obs` was never re-pointed and `b` never given up:

<!-- example: 06_branch_ok.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

int main() {
    Node a = new Node(1);
    Node b = new Node(2);
    ref Node obs = a;
    bool cond = false;
    if (cond) {
        obs = b;                // this branch re-points obs ...
        b = new Node(3);        // ... and then kills b
    } else {
        std::cout << obs.v << std::endl;    // this branch never saw either: obs still follows a
    }
    return 0;
}
```
<!-- output: 06_branch_ok.clg -->
```
1
```

**Where it stops.** The name in the message is the checker's best account, not always the owner that died. In the first example `obs` is refused because the arms disagree, and the message
says "owner `b` is gone", though `b` is alive. The verdict is right; the wording is not always.

## Loops and `break`

A loop body runs zero or more times, and a loop can be left from the middle. CeeLing analyses a `while` or a range-`for` body again from the state the previous pass left, until nothing
changes. Otherwise `sink(a)` in a loop would be accepted, because a single pass sees `a` alive:

<!-- example: 06_loop_moved_again.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

void sink(Node n) { }

int main() {
    Node a = new Node(1);
    int i = 0;
    while (i < 2) {
        i = i + 1;
        sink(a);            // fine the first time; the second pass starts with a moved-from a
    }
    return 0;
}
```
<!-- error: 06_loop_moved_again.clg -->
```
error[semantic/use-after-move]: use of moved-from value `a`
  --> 06_loop_moved_again.clg:14:14
  help: `a` was moved earlier; assign a new value before using it
```

A `break` leaves the straight-line flow. What happened before it does not reach the statements after it in the body, but it *does* reach the code after the loop. This program moves `a` only
on the path that stops looping, and the checker follows that:

<!-- example: 06_loop_break_ok.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

void sink(Node n) { }

int main() {
    Node a = new Node(1);
    int i = 0;
    while (i < 3) {
        i = i + 1;
        if (i == 2) {
            sink(a);        // a is moved, but only on the path that leaves the loop
            break;
        }
        if (i == 1) {
            std::cout << a.v << std::endl;
        }
    }
    return 0;
}
```
<!-- output: 06_loop_break_ok.clg -->
```
1
```

And the reverse, where the only path that goes wrong is the one that breaks:

<!-- example: 06_loop_break_dangles.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

int main() {
    Node a = new Node(1);
    ref Node obs = a;
    int i = 0;
    while (i < 3) {
        i = i + 1;
        Node b = new Node(2);
        obs = b;
        if (i == 2) { break; }      // leaves the loop while obs still follows b
        obs = a;
    }
    std::cout << obs.v << std::endl;
    return 0;
}
```
<!-- error: 06_loop_break_dangles.clg -->
```
error[semantic/use-after-move]: use of dangling observer `obs` (its owner `a` is gone)
  --> 06_loop_break_dangles.clg:18:18
  help: `obs` observes `a`, which no longer has a live owner
```

(The message names `a`, the last owner `obs` was seen to follow, while it is `b`, the loop-local, that died. The verdict is the point.)

## Containers, and the calls that invalidate references

The most common dangling reference in real C++ is a reference into a `std::vector` that grew:

<!-- example: 06_container_mutation.cpp -->
```cpp
// Run-time effect: after the second push_back the vector may have moved its elements; first points at the old storage.
#include <iostream>
#include <vector>

int main() {
    std::vector<int> v;
    v.push_back(1);
    int& first = v[0];
    v.push_back(2);                         // may reallocate
    std::cout << first << std::endl;        // undefined behaviour if it did
    return 0;
}
```

**Why C++ does not stop it.** The standard says in prose which calls invalidate references. No type can say it: whether `push_back` reallocates depends on a capacity known at run time, and
the reference has the type `int&` before and after. Debug libraries (`-D_GLIBCXX_DEBUG`) and sanitizers find some of these when the code runs; nothing in C++ rejects the program.

**What CeeLing does.** An observer of an element, `ref Node n = v[0];`, is an observer of the container `v`. The checker has a short table of what the vector methods CeeLing knows do to
that:

| Call | Effect on observers of the vector |
|---|---|
| `push_back`, `pop_back`, `resize` | they die |
| `reserve`, `copyWithin` | nothing |
| `size`, `begin`, `end` | nothing (and `begin`/`end` produce no tracked iterator, see below) |
| `clear`, `erase`, `insert`, anything else | not accepted: the call is an error |

<!-- example: 06_element_ok.clg -->
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
    std::cout << n.v << std::endl;      // v is not touched between the binding and the use
    return 0;
}
```
<!-- output: 06_element_ok.clg -->
```
1
```

The observer dies at a `push_back`, and, as for every other death event, a change inside one arm of an `if` counts after the `if`:

<!-- example: 06_branch_invalidate.clg -->
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
    bool cond = true;
    if (cond) {
        v.push_back(new Node(2));       // on this path only
    }
    std::cout << n.v << std::endl;
    return 0;
}
```
<!-- error: 06_branch_invalidate.clg -->
```
error[semantic/use-after-move]: use of dangling observer `n` (its owner `v` is gone)
  --> 06_branch_invalidate.clg:15:18
  help: `n` observes `v`, which no longer has a live owner
```

`pop_back` is the call that is exact: it destroys the element the observer looks at.

<!-- example: 06_pop_back.clg -->
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
    v.pop_back();                       // destroys the element n looks at
    std::cout << n.v << std::endl;
    return 0;
}
```
<!-- error: 06_pop_back.clg -->
```
error[semantic/use-after-move]: use of dangling observer `n` (its owner `v` is gone)
  --> 06_pop_back.clg:12:18
  help: `n` observes `v`, which no longer has a live owner
```

For `push_back` the rule is stricter than the generated code needs. CeeLing keeps a vector of objects as a vector of smart pointers to heap objects, and the observer is a pointer to the
object, not to the slot, so a reallocation moves pointers and leaves the objects where they are. The checker tracks invalidation for the container as a whole, not per element, and takes the
simple rule. The cost is a refused program that would have been fine; the gain is that the rule does not depend on how the element happens to be stored.

Methods that are not on the table are not "unknown, therefore allowed". Every call to a library function or method the compiler has no fact about is refused (this is the default-reject rule of
[chapter 14](14-how-it-works.md)), so a mutator cannot slip past the table by being missing from it:

<!-- example: 06_vector_clear.clg -->
```cpp
int main() {
    std::vector<int> v;
    v.push_back(1);
    v.clear();              // not on the list of vector methods CeeLing knows
    return 0;
}
```
<!-- error: 06_vector_clear.clg -->
```
error[semantic/forbidden-construct]: call to unknown function/method `clear`
  --> 06_vector_clear.clg:4:5
  help: CeeLing has no borrow/ownership summary for anything it did not compile itself; unknown calls are rejected by default rather than silently trusted (CLAUDE.md "default-reject, never default-allow"). Define `clear` in CeeLing source, or call an existing CeeLing function/method instead.
```

### Why `reserve` and `copyWithin` do not invalidate

Both exist only for vectors of primitives (`int`, `byte`, ...), and a vector of objects refuses them:

<!-- example: 06_reserve_class.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

int main() {
    std::vector<Node> v;
    v.reserve(10);
    return 0;
}
```
<!-- error: 06_reserve_class.clg -->
```
error[semantic/forbidden-construct]: `reserve` on `std::vector<Node>`: element type is not primitive
  --> 06_reserve_class.clg:9:5
  help: `reserve` is only allowed on vectors of primitive elements (int, byte, char, ...), matching `resize`'s own restriction -- v1 has no motivating case for reserving capacity on an object-element vector.
```

<!-- example: 06_reserve_copywithin.clg -->
```cpp
int main() {
    std::vector<int> v;
    v.push_back(7);
    v.push_back(8);
    v.push_back(3);
    v.reserve(100);                 // capacity only: size and values stay
    v.copyWithin(1, 0, 1);          // v[1] = v[0], inside the same vector
    std::cout << v[0] << " " << v[1] << " " << v[2] << std::endl;
    return 0;
}
```
<!-- output: 06_reserve_copywithin.clg -->
```
7 7 3
```

The reason they are not on the invalidating list is not that `reserve` cannot reallocate; it can. It is that nothing in CeeLing can observe it. A primitive element has no owner and no
`ref` can point at it (`v[i]` is a copy of the number), CeeLing has no iterators that survive a statement, and a container passed to a function is either copied or read through a `const&`
that cannot outlive the call. `reserve` only grows capacity and never changes `size()` or a value; `copyWithin` moves values inside the vector and never creates or destroys an element.
`push_back`, `pop_back` and `resize` change `size()`, so they stay on the list.

### Iterators, and the range-`for`

CeeLing does not track iterators. `begin()` and `end()` are accepted as calls, and a `for (int x : v)` loop over a vector or a map works, but the checker has no notion of an iterator
object that depends on a container, so a loop that grows the vector it walks is not caught. The C++ shape, and the CeeLing program that is accepted although it should not be:

<!-- example: 06_iterator.cpp -->
```cpp
// Run-time effect: the iterator is used after push_back may have reallocated.
#include <iostream>
#include <vector>

int main() {
    std::vector<int> v;
    v.push_back(1);
    auto it = v.begin();
    v.push_back(2);                         // invalidates it if the vector grew
    std::cout << *it << std::endl;          // undefined behaviour
    return 0;
}
```

<!-- example: 06_range_for.cpp -->
```cpp
// Run-time effect: push_back inside a range-for over the same vector invalidates the loop's own iterators.
#include <iostream>
#include <vector>

int main() {
    std::vector<int> v;
    v.push_back(1);
    v.push_back(2);
    for (int x : v) {
        v.push_back(x);                     // undefined behaviour once the vector reallocates
    }
    std::cout << v.size() << std::endl;
    return 0;
}
```

<!-- example: 06_gap_range_for.clg -->
```cpp
int main() {
    std::vector<int> v;
    v.push_back(1);
    v.push_back(2);
    for (int x : v) {
        v.push_back(x);             // the loop is iterating over v while v grows
    }
    std::cout << v.size() << std::endl;
    return 0;
}
```

This is a gap, not a design. The second program is undefined behaviour in C++ and CeeLing accepts it. (The same program with `ref Node` elements is also accepted. A loop variable is treated as
a fresh owner of each element.) The only protection today is that the standard vector mutators outside the table are refused, and that the common mistake with a *stored* iterator is hard
to write:

* A variable of type `std::vector<int>::iterator` does not parse.
* `auto it = v.begin();` does parse, because CeeLing passes a type name it does not recognise through to C++ unchanged. It is accepted, and an iterator held that way is **not** tracked. A
  program that does `auto it = v.begin(); v.push_back(2); *it` is accepted:

<!-- example: 06_gap_auto_iterator.clg -->
```cpp
std::vector<int> make() {
    std::vector<int> v;
    v.push_back(1);
    return v;
}

int main() {
    std::vector<int> v = make();
    auto it = v.begin();            // CeeLing does not know the type "auto": it is passed through
    v.push_back(2);                 // may reallocate; it now dangles
    std::cout << *it << std::endl;  // accepted -- and undefined behaviour
    return 0;
}
```
<!-- generated: 06_gap_auto_iterator.clg "int main()" -->
```cpp
int main() {
    std::vector<int> v = make();
    auto it = v.begin();
    v.push_back(2);
    std::cout << *it << std::endl;
    return 0;
}
```

An iterator class written in CeeLing itself is a different case and is safe in this respect: a class with `operator*`, `operator++` and `operator!=` is treated as a value type, has no `ref`
field in which to hold the container, and works with range-`for`:

<!-- example: 06_iterator_protocol.clg -->
```cpp
class Counter {
public:
    int value;
    Counter(int v) { value = v; }
    int operator*() { return value; }
    void operator++() { value = value + 1; }
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

int main() {
    Range r = new Range(1, 4);
    bool first = true;
    for (int x : r) {
        if (!first) { std::cout << " "; }
        std::cout << "x=" << x;
        first = false;
    }
    std::cout << std::endl;
    return 0;
}
```
<!-- output: 06_iterator_protocol.clg -->
```
x=1 x=2 x=3
```

## Views: `string_view`, `span`, `const&` to a temporary, Eigen

C++'s newest dangling sources are objects that look like values and are references. This part says what CeeLing does for each, and says plainly that most of it is not built.

### `const&` bound to a temporary

<!-- example: 06_const_ref_temporary.cpp -->
```cpp
// Run-time effect: binding a const reference to a temporary extends its life, but only when it is bound directly.
// Here the reference comes back out of a function, so the temporaries it points into die at the semicolon.
#include <iostream>
#include <string>

const std::string& longer(const std::string& a, const std::string& b) {
    return a.size() >= b.size() ? a : b;
}

int main() {
    const std::string& s = longer(std::string("short"), std::string("longer one"));
    std::cout << s << std::endl;            // undefined behaviour
    return 0;
}
```

**Why C++ does not stop it.** The rule that a `const&` extends a temporary's life applies when the temporary is bound *directly*. Through a function that returns a reference it does not, and
nothing in the call says which case this is. (Recent g++ warns about this particular shape; the warning is a heuristic, not a guarantee.)

**What CeeLing does.** In source you cannot write it. There is no `const T&` local, and the only reference variable, `ref`, refuses a call result or a temporary (the table above):

<!-- example: 06_no_const_ref.clg -->
```cpp
string make() {
    return "temporary";
}

int main() {
    const std::string& s = make();      // C++ would extend the temporary's life here; CeeLing has no such local
    std::cout << s << std::endl;
    return 0;
}
```
<!-- error: 06_no_const_ref.clg -->
```
Error: Syntax error at line 6, column 14, node type 'ERROR', near: ::string& s. Suggestion: check whether the statement is missing a trailing ';'.
```

The `const&` parameters and arguments that appear in the generated C++ are written by the compiler, and only after it has shown the callee reads without storing ([chapter 5](05-moves.md)). So
this shape is closed by construction, as `ref` fields are, rather than by analysis.

### `string_view` and `span`

<!-- example: 06_string_view.cpp -->
```cpp
// Run-time effect: the view points into the temporary string, which died at the semicolon; the output is garbage.
#include <iostream>
#include <string>
#include <string_view>

std::string make() {
    return "a string long enough to live on the heap, not in the string object";
}

int main() {
    std::string_view sv = make();           // the temporary is destroyed at the end of this statement
    std::cout << sv << std::endl;           // undefined behaviour
    return 0;
}
```

**What CeeLing does.** Nothing yet. There is no tracking of a view that depends on a string or a vector, and the program below is accepted:

<!-- example: 06_gap_string_view.clg -->
```cpp
string make() {
    return "a string long enough to live on the heap, not in the string object";
}

int main() {
    std::string_view sv = make();   // the string returned by make() is a temporary
    std::cout << sv << std::endl;   // accepted -- and a read of freed memory
    return 0;
}
```
<!-- generated: 06_gap_string_view.clg "int main()" -->
```cpp
int main() {
    std::string_view sv = make();
    std::cout << sv << std::endl;
    return 0;
}
```

The cause is not a missing rule for `string_view` as such. A type name the compiler does not know is passed to C++ as it stands, and this holds for `std::string_view`, `std::span`, `auto`
and any made-up name. The checks follow the types CeeLing knows (its classes, `string`, vectors and maps, a short list of external classes); a value of any other type is invisible to them. If you
write a program for a build where this matters, do not use those types, and prefer a `string` or a `std::vector` (an owner) wherever you would use a view.

### Eigen

The Eigen trap is an expression that holds references to its operands. A miniature of it, without Eigen, so that it can be compiled here:

<!-- example: 06_expression_template.cpp -->
```cpp
// A miniature of what Eigen's expression templates do: operator+ does not add, it returns an object that refers to both operands.
// Run-time effect: sum holds references to the temporaries, which died at the end of the statement that created them.
#include <iostream>
#include <vector>

struct Vec {
    std::vector<int> data;
};

struct Sum {                                // the lazy expression "a + b"
    const Vec& a;
    const Vec& b;
    int at(size_t i) const { return a.data[i] + b.data[i]; }
};

Sum operator+(const Vec& a, const Vec& b) { return Sum{a, b}; }

Vec make(int x) { return Vec{{x, x}}; }

int main() {
    auto sum = make(1) + make(2);           // auto keeps the lazy Sum; the two Vec temporaries are destroyed at the semicolon
    std::cout << sum.at(0) << std::endl;    // undefined behaviour
    return 0;
}
```

Eigen is the main reason these rules are written against *facts about calls* and not against the type system: its expression templates have no signature a checker could read. What CeeLing
has is a hand-written list of Eigen calls that return a view of their receiver. There are eight, all on `Eigen::MatrixXd`: `block`, `row`, `col`, `transpose`, `segment`, `head`, `tail`,
`diagonal`.

A variable assigned from one of them is an observer of the matrix it came from, and then all the death events above apply:

<!-- example: 06_eigen_block_dangles.clg -->
```cpp
#include <Eigen/Dense>
#include <iostream>

int main() {
    Eigen::MatrixXd a = Eigen::MatrixXd::Identity(3, 3);
    Eigen::MatrixXd b = a.block(0, 0, 2, 2);    // block() is on the list of calls that return a view of their receiver
    a = Eigen::MatrixXd::Identity(5, 5);        // a's storage is replaced
    std::cout << b << std::endl;
    return 0;
}
```
<!-- error: 06_eigen_block_dangles.clg -->
```
error[semantic/use-after-move]: use of dangling observer `b` (its owner `a` is gone)
  --> 06_eigen_block_dangles.clg:8:18
  help: `b` observes `a`, which no longer has a live owner
```

Anything else that could return a view is refused as an unknown call, which is the safe direction:

<!-- example: 06_eigen_unlisted.clg -->
```cpp
#include <Eigen/Dense>
#include <iostream>

int main() {
    Eigen::MatrixXd a = Eigen::MatrixXd::Identity(3, 3);
    Eigen::MatrixXd b = a.topRows(2);           // a view method nobody has written a fact for
    std::cout << b << std::endl;
    return 0;
}
```
<!-- error: 06_eigen_unlisted.clg -->
```
error[semantic/forbidden-construct]: call to unknown function/method `topRows`
  --> 06_eigen_unlisted.clg:6:25
  help: CeeLing has no borrow/ownership summary for anything it did not compile itself; unknown calls are rejected by default rather than silently trusted (CLAUDE.md "default-reject, never default-allow"). Define `topRows` in CeeLing source, or call an existing CeeLing function/method instead.
```

Now the limits, which are larger than the feature:

* **The program above is correct C++.** `Eigen::MatrixXd b = a.block(...)` copies the block into `b`, because the destination is a matrix and not a view. CeeLing refuses it anyway,
  because it tracks the call and not the type of the destination. It is conservative, not precise. The accepted case, using `b` while `a` is untouched, works and prints the right matrix
  (checked by compiling against Eigen by hand).
* **The tracked form is the safe one; the dangerous form is not tracked.** The danger in Eigen is a *lazy expression* kept in a variable (`auto e = A * B;`), which holds references to its
  operands. Written with `auto`, that is accepted by the compiler, because of the pass-through of unknown type names above. Checked by hand against Eigen: `auto e = a * b;`, then assigning a new
  matrix to `a`, then `Eigen::MatrixXd r = e;` is accepted, and aborts at run time on Eigen's own assertion `invalid matrix product`. Older notes in the repository say that CeeLing has no
  `auto` and so cannot express this; the compiler disagrees.
* **Only eight methods, only `MatrixXd`.** Eigen's other views (`topRows`, `array`, ...) are refused until someone adds them. A chained view (`a.block(...).transpose()`) is refused too.
* **No Eigen program is in this guide's accepted examples**, because the checker that compiles the examples has no Eigen header path. The two Eigen examples above are rejected ones, which
  need only the CeeLing compiler. The accepted counterparts live in a separate spike folder and were run by hand.

What works in a sentence: if you write `Eigen::MatrixXd` as the type of a variable and call one of the eight methods, you get a dependence on the receiver, with all the death events. What
does not: lazy expressions, any other view type, `string_view` and `span`.

## What this does not cover

The static half of the safety contract rests on the checks above, and each of these is a place where a dangling reference can still be built. They are listed in plain words, so that "it
compiled" is not read as more than it means.

* **Iterators.** An iterator held in a variable, and a range-`for` over a container that its own body changes, are not tracked.
* **Views.** `string_view`, `span` and similar types, and Eigen's lazy expressions held through `auto` (or any other type name the compiler does not know), are not tracked.
* **Types the compiler does not know pass through.** A declaration whose type name is not one of CeeLing's is emitted unchanged. A hazard named by such a type is invisible to every check
  in this chapter.
* **Other Eigen calls.** Eight view methods on one type; everything else is refused or untracked.
* **Library code.** What a C++ library or an `ext_fn` does with what it is handed is trusted. A function declared `ext_fn` with a wrong `borrows N` makes the checker believe a wrong fact
  ([chapter 11](11-talking-to-cpp.md)).
* **Granularity.** An observer of a container element depends on the whole container, not the element: more programs are refused than needed, never fewer. Pointing an observer at
  an element of a nested container is not modelled.
* **Message wording.** The owner named in a message is not always the one that died (see branches and loops above).
* **Misuse of `ref` that C++ refuses.** `ref` passed as an argument, copied into an owner, or applied to a primitive is not diagnosed by CeeLing; the generated C++ does not compile.
* **Data races.** A dangling reference can also come from another thread destroying an object. CeeLing's answer is [chapter 10](10-threads.md), and it is narrow.

Not on the list, because they are closed by construction: references that outlive a closure's frame ([chapter 9](09-closures.md)), references stored in fields, returned or passed, and
`const&` bound to a temporary in source code.

Everything in this chapter is a claim about the CeeLing source the compiler saw. The C++ it writes is then compiled by g++, and the C++ libraries it calls are not checked.
