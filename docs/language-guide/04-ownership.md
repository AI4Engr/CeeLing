# 4. Ownership: the compiler chooses the smart pointer

In CeeLing you write `Node next;` and `Node prev;`. You never write `unique_ptr`, `shared_ptr` or `weak_ptr`. The compiler reads how the objects are used and picks one of the three
for every field, local, parameter, return type and container element, then writes `make_unique` or `make_shared` for every `new`. This chapter is about what it picks, why, and where
its choice can be wrong.

The short version:

| What the compiler sees | It writes | Why |
|---|---|---|
| one owner: nothing else keeps the object | `std::unique_ptr<T>` | no reference count, destroyed exactly when its owner is |
| two or more owners | `std::shared_ptr<T>` | the object lives until the last owner lets go |
| a link back to something that owns you, closing a cycle | `std::weak_ptr<T>` | it observes without owning, so the cycle cannot keep itself alive |

Everything below is about how it tells these apart. Moves (what happens to the *old* owner when an object is handed over) are [chapter 5](05-moves.md).

## The C++ problem: you choose, and nobody checks the choice

In C++ every owning pointer is a decision made by hand, at every declaration. The decision depends on how the object is used *everywhere else in the program*, which the declaration
does not show and the compiler does not check. Three things go wrong.

### Choosing too much

<!-- example: 04_wrong_pointer_kind.cpp -->
```cpp
// The opposite mistake: every owner is a shared_ptr "to be safe", also where one owner would do.
// Nothing is wrong with the output; the cost is a reference count on every copy, and an object
// that no longer has a single owner to reason about.
#include <iostream>
#include <memory>

struct Texture {
    int id;
    explicit Texture(int i) : id(i) {}
};

struct Sprite {
    std::shared_ptr<Texture> texture;       // shared: several sprites use one texture
    std::shared_ptr<Sprite> next;           // shared by habit: only the previous sprite owns it
    Sprite(std::shared_ptr<Texture> t) : texture(std::move(t)) {}
};

int main() {
    auto rock = std::make_shared<Texture>(7);
    Sprite a(rock);
    a.next = std::make_shared<Sprite>(rock);
    std::cout << a.next->texture->id << " " << rock.use_count() << std::endl;
    return 0;
}
```

This program is correct and `g++ -Wall -Wextra` is silent. The cost is not an error, it is a habit: `next` is `shared_ptr` "to be safe", so every copy pays a reference count and the
code no longer says that one sprite owns the next. Making it `unique_ptr` is a change at each declaration and each use.

### Choosing wrong, silently: the cycle

<!-- example: 04_cycle_leak.cpp -->
```cpp
// Two nodes of a doubly linked list. Both links are shared_ptr, so the pair keeps itself alive.
#include <iostream>
#include <memory>

int alive = 0;                              // how many Nodes exist

struct Node {
    int v;
    std::shared_ptr<Node> next;
    std::shared_ptr<Node> prev;             // a back link that owns: nothing breaks the cycle
    explicit Node(int x) : v(x) { alive++; }
    ~Node() { alive--; }
};

int main() {
    {
        auto a = std::make_shared<Node>(1);
        a->next = std::make_shared<Node>(2);
        a->next->prev = a;                  // a -> next -> prev -> a
    }                                       // both handles leave scope here
    std::cout << "alive after the scope: " << alive << std::endl;   // prints 2: both nodes leaked
    return 0;
}
```

When this runs it prints `alive after the scope: 2`: both nodes are still alive after the only outside handle is gone. Each keeps the other's count above zero, so neither is ever
destroyed. Nothing is wrong with the types, so the compiler accepts it and says nothing.

### Choosing wrong, loudly: handing out `this`

<!-- example: 04_this_double_free.cpp -->
```cpp
// A method that hands out `this`. Wrapping the raw pointer in a second shared_ptr starts a second
// reference count for the same object, so the object is deleted twice.
#include <iostream>
#include <memory>

struct Node {
    int v;
    explicit Node(int x) : v(x) {}
    std::shared_ptr<Node> self() { return std::shared_ptr<Node>(this); }   // wrong: should derive from enable_shared_from_this
};

int main() {
    auto a = std::make_shared<Node>(1);
    std::shared_ptr<Node> s = a->self();
    std::cout << s->v << std::endl;
    return 0;                               // a and s each delete the Node: double free
}
```

`std::shared_ptr<Node>(this)` starts a second reference count for an object that already has one. The program prints `1` and then both counts delete the same object. (`g++` at `-O2`
may warn about the free; at the `-Wall -Wextra` flags the guide checks, it is silent.) The fix, `std::enable_shared_from_this`, is a base class the author has to know to add.

**Why C++ does not stop it.** `unique_ptr`, `shared_ptr` and `weak_ptr` are three library types. The compiler checks that the types fit together, not that the *choice* was right: whether
a cycle exists, whether a second owner appears three functions later, whether a method hands out `this`. Those are questions about the whole program. A reference count cannot find
a cycle either; that is a property of the graph of objects, and C++ has no such graph in its type system.

**Why CeeLing can.** It is a smaller language with one analysis over the whole program ([chapter 14](14-how-it-works.md)). A CeeLing class has no pointer syntax, so the pointer
type is not something the programmer says, it is an output. The compiler decides every declaration together, from all the uses, and writes consistent code. The price is that the rules
below are deliberately few and partly by name; where they miss, the result is a slower or leaking program, never a dangling one (dangling is [chapter 6](06-dangling-references.md)).

## Rule 1: one owner is a `unique_ptr`

The default. An object with exactly one owner (a field of one object, or a local) is a `unique_ptr`, and `new` becomes `std::make_unique`.

<!-- example: 04_tree_unique.clg -->
```cpp
class Tree {
public:
    int v;
    Tree left;
    Tree right;
    Tree(int x) { v = x; left = nullptr; right = nullptr; }
    int sum() {
        int s = v;
        if (left != nullptr) { s = s + left.sum(); }
        if (right != nullptr) { s = s + right.sum(); }
        return s;
    }
};

int main() {
    Tree root = new Tree(1);
    root.left = new Tree(2);
    root.right = new Tree(3);
    std::cout << root.sum() << std::endl;
    return 0;
}
```
<!-- output: 04_tree_unique.clg -->
```
6
```
<!-- generated: 04_tree_unique.clg "class Tree {" -->
```cpp
class Tree {
public:
    int v{};
    std::unique_ptr<Tree> left;
    std::unique_ptr<Tree> right;
    Tree(int x);
    int sum();
    ~Tree();
};
```

`left` and `right` are each owned by exactly one `Tree`, so they are `unique_ptr`. There is no reference count and the subtree is destroyed with its parent. `root` in `main` is
`unique_ptr` too. An owner that owns a long chain has a consequence, covered below: destroying the chain must not overflow the stack.

## Rule 2: several owners make a `shared_ptr`

An object with two owners cannot be a `unique_ptr`: a `unique_ptr` cannot be copied. The compiler looks for the places where a second owner appears. Each place is a separate rule,
and each has an example, because in each the evidence is different.

### A second local handle

<!-- example: 04_local_copy.clg -->
```cpp
class Item {
public:
    int v;
    Item(int x) { v = x; }
};

void twoHandles() {
    Item a = new Item(1);
    Item b = a;                 // a is used again below: two owners
    b.v = 5;
    std::cout << a.v << " " << b.v << std::endl;
}

void handOver() {
    Item x = new Item(9);
    Item y = x;                 // x is never used again: one owner, moved
    std::cout << y.v << std::endl;
}

int main() {
    twoHandles();
    handOver();
    return 0;
}
```
<!-- output: 04_local_copy.clg -->
```
5 5
9
```
<!-- generated: 04_local_copy.clg "void twoHandles()" -->
```cpp
void twoHandles() {
    std::shared_ptr<Item> a = std::make_shared<Item>(1);
    std::shared_ptr<Item> b = a;
    cling::nn(b, "line 10")->v = 5;
    std::cout << cling::nn(a, "line 11")->v << " " << cling::nn(b, "line 11")->v << std::endl;
}
```
<!-- generated: 04_local_copy.clg "void handOver()" -->
```cpp
void handOver() {
    std::unique_ptr<Item> x = std::make_unique<Item>(9);
    std::unique_ptr<Item> y = std::move(x);
    std::cout << cling::nn(y, "line 17")->v << std::endl;
}
```

`Item b = a;` means two things, depending on what comes next. In `twoHandles`, `a` is read again after the copy, so `a` and `b` are both live owners: both become `shared_ptr` and
the copy is a plain copy of the handle. In `handOver`, `x` is never used again, so the assignment hands the object over: both stay `unique_ptr` and the copy is a move. The same
text means "share" or "transfer" by what follows it, which is the evidence use-after-move checking uses too ([chapter 5](05-moves.md)).

### A field that receives an object someone else still holds

<!-- example: 04_shared_texture.clg -->
```cpp
class Texture {
public:
    string name;
    Texture(string n) { name = n; }
};

class Sprite {
public:
    Texture texture;
    int x;
    Sprite(Texture t, int px) { texture = t; x = px; }
};

int main() {
    Texture rock = new Texture("rock");
    Sprite a = new Sprite(rock, 1);
    Sprite b = new Sprite(rock, 2);
    std::cout << a.texture.name << " " << b.texture.name << std::endl;
    return 0;
}
```
<!-- output: 04_shared_texture.clg -->
```
rock rock
```
<!-- generated: 04_shared_texture.clg "class Sprite {" -->
```cpp
class Sprite {
public:
    std::shared_ptr<Texture> texture;
    int x{};
    Sprite(std::shared_ptr<Texture> t, int px);
};
```

`Sprite`'s constructor stores its parameter into `texture`, and `main` passes the same `rock` to two sprites. The field receives an existing object, so it is `shared_ptr`; the
local `rock` that feeds it becomes `shared_ptr` as well (it must, or it could not be copied into the sprites); the two `Sprite`s have one owner each and stay `unique_ptr`. The
decision is per variable and per field, not per class.

### A field copied to a local

<!-- example: 04_field_alias.clg -->
```cpp
class Part {
public:
    int size;
    Part(int s) { size = s; }
};

class Widget {
public:
    Part part;
    Widget(Part p) { part = p; }
};

int main() {
    Widget w = new Widget(new Part(3));
    Part p = w.part;                    // the field keeps its object, so p is a second owner
    std::cout << p.size << " " << w.part.size << std::endl;
    return 0;
}
```
<!-- output: 04_field_alias.clg -->
```
3 3
```
<!-- generated: 04_field_alias.clg "int main()" -->
```cpp
int main() {
    std::unique_ptr<Widget> w = std::make_unique<Widget>(std::make_shared<Part>(3));
    std::shared_ptr<Part> p = cling::nn(w, "line 15")->part;
    std::cout << cling::nn(p, "line 16")->size << " " << cling::nn(cling::nn(w, "line 16")->part, "line 16")->size << std::endl;
    return 0;
}
```

`Part p = w.part;` leaves the object in the field and in the local, so they are two owners. `Widget.part` was already shared (a parameter stored into a field, as in the last
example); the local `p` is shared because it is a copy of that handle.

### `this` handed out

<!-- example: 04_self_handed_out.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
    Node self() { return this; }
};

int main() {
    Node a = new Node(1);
    Node s = a.self();
    std::cout << s.v << std::endl;
    return 0;
}
```
<!-- output: 04_self_handed_out.clg -->
```
1
```
<!-- generated: 04_self_handed_out.clg "class Node : public" -->
```cpp
class Node : public std::enable_shared_from_this<Node> {
public:
    int v{};
    Node(int x);
    std::shared_ptr<Node> self();
};
```
<!-- generated: 04_self_handed_out.clg "std::shared_ptr<Node> Node::self()" -->
```cpp
std::shared_ptr<Node> Node::self() {
    return shared_from_this();
}
```

This is the CeeLing form of the double free at the start of the chapter. A method that returns `this` creates a second owner, so the compiler does three things the C++ author had
to remember: it adds `std::enable_shared_from_this<Node>` as a base, it writes `shared_from_this()` instead of wrapping the raw pointer, and it creates every `Node` with
`make_shared` (`shared_from_this()` is only valid for an object a `shared_ptr` already owns, which is why `a` in `main` is a `shared_ptr`).

`this` passed as an argument counts the same way:

<!-- example: 04_this_as_argument.clg -->
```cpp
class Node {
public:
    int v;
    Node(int val) { v = val; }
    void useSelf(Node other) { v = v + other.v; }
    void chainSelf() { useSelf(this); }
};

int main() {
    Node n = new Node(2);
    n.chainSelf();
    std::cout << n.v << std::endl;
    return 0;
}
```
<!-- output: 04_this_as_argument.clg -->
```
4
```
<!-- generated: 04_this_as_argument.clg "void Node::chainSelf()" -->
```cpp
void Node::chainSelf() {
    useSelf(shared_from_this());
}
```

### A returned local that is shared

<!-- example: 04_returned_local.clg -->
```cpp
class Item {
public:
    int n;
    Item(int v) { n = v; }
};

class Book {
public:
    std::vector<Item> items;
    Book() { }
    void add(Item i) { items.push_back(i); }
    Item get(int i) {
        Item w = items[i];
        return w;                  // w is shared with the vector, so the result must be a shared handle too
    }
};

int main() {
    Book b = new Book();
    b.add(new Item(2));
    Item x = b.get(0);
    std::cout << x.n << std::endl;
    return 0;
}
```
<!-- output: 04_returned_local.clg -->
```
2
```
<!-- generated: 04_returned_local.clg "std::shared_ptr<Item> Book::get(int i)" -->
```cpp
std::shared_ptr<Item> Book::get(int i) {
    std::shared_ptr<Item> w = cling::at(this->items, i, 13);
    return w;
}
```

`get` returns a local `w` that is a copy of a vector element. The vector keeps its element, so `w` is shared, and the function must return a `shared_ptr` too. The caller's
`Item x = b.get(0)` is then shared in turn. The evidence travels along calls: a method's return kind is decided from its body, and a local initialised from a call whose result is
shared is shared. Methods that depend on each other are solved together, so the order of declarations in the file does not matter.

### An element copied out of a vector

<!-- example: 04_element_copied_out.clg -->
```cpp
class Cmd {
public:
    int n;
    Cmd(int x) { n = x; }
    int call(int x) { return n + x; }
};

class Table {
public:
    std::vector<Cmd> items;
    Table() { }
    void add(Cmd c) { items.push_back(c); }
    int run(int i, int x) {
        Cmd c = items[i];          // the vector keeps its element, so there are now two owners
        return c.call(x);
    }
};

int main() {
    Table t = new Table();
    t.add(new Cmd(10));
    std::cout << t.run(0, 5) << std::endl;
    return 0;
}
```
<!-- output: 04_element_copied_out.clg -->
```
15
```
<!-- generated: 04_element_copied_out.clg "class Table {" -->
```cpp
class Table {
public:
    std::vector<std::shared_ptr<Cmd>> items;
    Table();
    void add(std::shared_ptr<Cmd> c);
    int run(int i, int x);
};
```

`Cmd c = items[i];` leaves the element in the vector, so the vector's elements are shared. That changes the *type of the vector* (`vector<shared_ptr<Cmd>>`), which is the next section.

## Containers: one element kind for the whole program

`std::vector<std::unique_ptr<T>>` and `std::vector<std::shared_ptr<T>>` are different C++ types, so a vector's element kind cannot vary from one `push_back` to another. The compiler
decides it once per container, from every use of that container in the program: if any element is ever copied out (as above), the elements are shared, and every value pushed
into the container is made shared as well.

<!-- example: 04_vector_kinds.clg -->
```cpp
class Cmd {
public:
    int n;
    Cmd(int x) { n = x; }
};

class Registry {
public:
    std::vector<Cmd> plain;        // only ever pushed into: one owner per element
    std::vector<Cmd> copied;       // an element is copied out below: two owners
    Registry() { }
    void addPlain(Cmd p) { plain.push_back(p); }
    void addCopied(Cmd c) { copied.push_back(c); }
    int firstCopied() {
        Cmd c = copied[0];
        return c.n;
    }
    int count() { return plain.size(); }
};

int main() {
    Registry r = new Registry();
    r.addPlain(new Cmd(1));
    r.addPlain(new Cmd(2));
    r.addCopied(new Cmd(1));
    std::cout << r.count() + r.firstCopied() << std::endl;
    return 0;
}
```
<!-- output: 04_vector_kinds.clg -->
```
3
```
<!-- generated: 04_vector_kinds.clg "class Registry {" -->
```cpp
class Registry {
public:
    std::vector<std::unique_ptr<Cmd>> plain;
    std::vector<std::shared_ptr<Cmd>> copied;
    Registry();
    void addPlain(std::unique_ptr<Cmd> p);
    void addCopied(std::shared_ptr<Cmd> c);
    int firstCopied();
    int count();
};
```

`plain` is only pushed into, so its elements are `unique_ptr` and `addPlain` takes a `unique_ptr`. `copied` has an element copied out, so it is a vector of `shared_ptr`.

**Where it stops: containers are told apart by name.** Two containers with the same field name in different classes share one decision:

<!-- example: 04_same_name_containers.clg -->
```cpp
// Two containers called items in two classes: the compiler tells containers apart by name, so they get one element kind.
class Cmd {
public:
    int n;
    Cmd(int x) { n = x; }
};

class A {
public:
    std::vector<Cmd> items;
    A() { }
    void add(Cmd c) { items.push_back(c); }
    int first() { Cmd c = items[0]; return c.n; }
};

class B {
public:
    std::vector<Cmd> items;       // B only pushes, but shares the decision of A.items
    B() { }
    void add(Cmd c) { items.push_back(c); }
};

int main() {
    A a = new A();
    a.add(new Cmd(1));
    B b = new B();
    b.add(new Cmd(2));
    std::cout << a.first() << std::endl;
    return 0;
}
```
<!-- output: 04_same_name_containers.clg -->
```
1
```
<!-- generated: 04_same_name_containers.clg "class B {" -->
```cpp
class B {
public:
    std::vector<std::shared_ptr<Cmd>> items;
    B();
    void add(std::shared_ptr<Cmd> c);
};
```

`B.items` only pushes, but because `A.items` has an element copied out, both are `vector<shared_ptr<Cmd>>`. The result is correct and a little slower than necessary; it is not
unsafe. Give containers distinct names if it matters. A `weak_ptr` is never an element kind. A pushed value that stays unique is moved in, so pushing a local and then using it is a
use-after-move error ([chapter 5](05-moves.md)).

## Rule 3: a back link that closes a cycle is a `weak_ptr`

A doubly linked list, a tree whose children know their parent, an event listener that knows its emitter: each is a cycle of owners. If every link owned, the cycle would keep itself
alive (the leak at the start of the chapter). One link has to only *observe*.

<!-- example: 04_back_link.clg -->
```cpp
class Node {
public:
    int v;
    Node next;
    Node prev;
    Node(int x) { v = x; next = nullptr; prev = nullptr; }
};

int main() {
    Node a = new Node(1);
    a.next = new Node(2);
    a.next.prev = a;
    std::cout << a.next.v << " " << a.next.prev.v << std::endl;
    return 0;
}
```
<!-- output: 04_back_link.clg -->
```
2 1
```
<!-- generated: 04_back_link.clg "class Node {" -->
```cpp
class Node {
public:
    int v{};
    std::unique_ptr<Node> next;
    std::weak_ptr<Node> prev;
    Node(int x);
    ~Node();
};
```
<!-- generated: 04_back_link.clg "int main()" -->
```cpp
int main() {
    std::shared_ptr<Node> a = std::make_shared<Node>(1);
    cling::nn(a, "line 11")->next = std::make_unique<Node>(2);
    cling::nn(cling::nn(a, "line 12")->next, "line 12")->prev = a;
    std::cout << cling::nn(cling::nn(a, "line 13")->next, "line 13")->v << " " << cling::nn(cling::nn(cling::nn(a, "line 13")->next, "line 13")->prev.lock(), "line 13")->v << std::endl;
    return 0;
}
```

`next` is the forward link and owns (`unique_ptr`: nothing else points at the next node). `prev` is the back link and is `weak_ptr`. A `weak_ptr` can only point at an object a
`shared_ptr` owns, so the local `a` is a `shared_ptr` too: every instance of a class that has a weak link into it is created with `make_shared`. Reading through a weak link is
`prev.lock()`, which may be empty; the generated code wraps it in the null check of [chapter 7](07-run-time-checks.md).

**This is the one place where the inference is partly by name.** The compiler recognises a back link in two ways, and only these two:

1. **By name.** A field named `prev`, `previous`, `parent`, `back` or `owner`, in a class that has at least two fields of its own type (the other is the forward link), is weak.
2. **By assignment.** If one function makes two objects of the same class point at each other (`a.down = b; b.up = a;`), the link written second is the back link, whatever it is
   called (a link with one of the names above wins if there is one). The compiler also prints `Detected bidirectional assignment between lines ...` to its error stream.

<!-- example: 04_back_link_by_assignment.clg -->
```cpp
// The names say nothing. Two variables are made to point at each other; the link written second is the back link.
class Node {
public:
    int v;
    Node down;
    Node up;
    Node(int x) { v = x; down = nullptr; up = nullptr; }
};

int main() {
    Node a = new Node(1);
    Node b = new Node(2);
    a.down = b;
    b.up = a;
    std::cout << a.down.up.v << std::endl;
    return 0;
}
```
<!-- output: 04_back_link_by_assignment.clg -->
```
1
```
<!-- generated: 04_back_link_by_assignment.clg "class Node {" -->
```cpp
class Node {
public:
    int v{};
    std::shared_ptr<Node> down;
    std::weak_ptr<Node> up;
    Node(int x);
    ~Node();
};
```

Both rules look at links between objects **of the same class**. A tree with a parent link is the common case they do not see: here `parent` is the only field of type `Node`
(`children` is a vector of them), so rule 1 does not fire, and nothing assigns in both directions inside one function:

<!-- example: 04_parent_needs_hint.clg -->
```cpp
// A tree with a parent link. `parent` is the only field of type Node (children is a vector), the
// name rule needs two, and nothing is assigned in both directions in one function: the link stays shared.
class Node {
public:
    int v;
    std::vector<Node> children;
    Node parent;
    Node(int x) { v = x; parent = nullptr; }
    void add(Node c) {
        c.parent = this;
        children.push_back(c);
    }
};

int main() {
    Node root = new Node(1);
    root.add(new Node(2));
    std::cout << root.children.size() << std::endl;
    return 0;
}
```
<!-- output: 04_parent_needs_hint.clg -->
```
1
```
<!-- generated: 04_parent_needs_hint.clg "class Node :" -->
```cpp
class Node : public std::enable_shared_from_this<Node> {
public:
    int v{};
    std::vector<std::shared_ptr<Node>> children;
    std::shared_ptr<Node> parent;
    Node(int x);
    void add(std::shared_ptr<Node> c);
    ~Node();
};
```

`parent` came out as `shared_ptr`. That is not an error and not unsafe, but it is a reference cycle (`children` owns the child, the child owns the parent), so a tree like this is
never freed. **The compiler does not warn.** The same holds between two different classes, here an `Emitter` that owns `Listener`s which each have an `Emitter source`:

<!-- example: 04_no_hint_cycle.clg -->
```cpp
// No field is called prev, parent, back or owner, and the object is not assigned in both directions
// in one function: the compiler sees no back link and the cycle Emitter -> Listener -> Emitter stays.
class Emitter {
public:
    int count;
    Listener first;
    Emitter() { count = 7; first = nullptr; }
    void attach(Listener l) {
        l.source = this;
        first = l;
    }
};

class Listener {
public:
    Emitter source;
    Listener() { source = nullptr; }
    int sourceCount() { return source.count; }
};

Listener wire() {
    Listener l = new Listener();
    Emitter e = new Emitter();
    e.attach(l);
    return l;
}

int main() {
    Listener l = wire();
    std::cout << l.sourceCount() << std::endl;
    return 0;
}
```
<!-- output: 04_no_hint_cycle.clg -->
```
7
```
<!-- generated: 04_no_hint_cycle.clg "class Listener {" -->
```cpp
class Listener {
public:
    std::shared_ptr<Emitter> source;
    Listener();
    int sourceCount();
};
```

The program prints `7` even though `wire()` returned only the listener: the emitter is still alive, kept by the cycle. CeeLing has no destructors to count, so the leak is not
observable here; it is established by reading the generated field type, `std::shared_ptr<Emitter> source`, together with the C++ leak at the top of the chapter.

### The `//@ptr` hint

When the compiler cannot tell, you tell it, in a comment directly above the field:

```
//@ptr weak Node.parent
//@ptr shared Node.next
//@ptr unique Node.left
```

The form is `//@ptr <unique|shared|weak> Class.member` (`member` alone also works). It is a comment, so it is not part of the grammar and costs nothing when you do not use it.

<!-- example: 04_parent_with_hint.clg -->
```cpp
class Node {
public:
    int v;
    std::vector<Node> children;
    //@ptr weak Node.parent
    Node parent;
    Node(int x) { v = x; parent = nullptr; }
    void add(Node c) {
        c.parent = this;
        children.push_back(c);
    }
};

int main() {
    Node root = new Node(1);
    root.add(new Node(2));
    std::cout << root.children.size() << std::endl;
    return 0;
}
```
<!-- output: 04_parent_with_hint.clg -->
```
1
```
<!-- generated: 04_parent_with_hint.clg "class Node :" -->
```cpp
class Node : public std::enable_shared_from_this<Node> {
public:
    int v{};
    std::vector<std::shared_ptr<Node>> children;
    std::weak_ptr<Node> parent;
    Node(int x);
    void add(std::shared_ptr<Node> c);
};
```

The hint beats every inferred rule. Four things to know:

* **A same-named variable elsewhere can spoil a hint.** The compiler matches some decisions by name across the whole program. If a parameter or local in any function has the
  same name as the hinted field, the field can come out `shared_ptr` with no message. The program below calls its parameter `before` for that reason. (The repository fixture
  `tests/safety/hint_applies_to_fields_only_accepted.clg` has the clashing shape, a parameter called `prev`; compiled today, its field `prev` is `std::shared_ptr`, not weak,
  which disagrees with that fixture's comment. It is not a checked example here.)

<!-- example: 04_hint_field.clg -->
```cpp
class Item {
public:
    Item next;
    //@ptr weak Item.prev
    Item prev;
    int v;
    Item(int x) { v = x; }
};

void link(Item before, Item item) {
    before.next = item;
    item.prev = before;
}

int main() {
    Item a = new Item(1);
    Item b = new Item(2);
    link(a, b);
    std::cout << a.next.v << " " << b.prev.v << std::endl;
    return 0;
}
```
<!-- output: 04_hint_field.clg -->
```
2 1
```
<!-- generated: 04_hint_field.clg "class Item {" -->
```cpp
class Item {
public:
    std::shared_ptr<Item> next;
    std::weak_ptr<Item> prev;
    int v{};
    Item(int x);
    ~Item();
};
```

* **A hint that matches nothing is ignored silently** in a normal compile. The compiler records a "did not match any discovered class pointer" warning, but it appears only in the
  analysis report below, so a typo in `Class.member` leaves the field as inferred. (Checked by misspelling the field in the cross-class example: the field stayed `shared_ptr`.)
* **A hint reaches a local or parameter only when no field has that name.**
* The same mechanism carries pointer demands between separately compiled files ([chapter 12](12-many-files.md)).

A hint cannot make the program unsafe: it changes which pointer type the generated C++ uses. It can make a program abort (next section) or leak, so it carries the same duty as
choosing the type in C++, except that you only need it in the cases above.

## Weak links and what happens when the target is gone

A `weak_ptr` does not keep its target alive. If the target is destroyed and the link is used afterwards, C++ gives an empty pointer and, for `->`, undefined behaviour.

<!-- example: 04_weak_gone.cpp -->
```cpp
// A weak_ptr back link whose target is gone. lock() returns an empty pointer, and the next line uses it.
#include <iostream>
#include <memory>

struct Emitter {
    int count = 7;
};

struct Listener {
    std::weak_ptr<Emitter> owner;
};

int main() {
    Listener l;
    {
        auto e = std::make_shared<Emitter>();
        l.owner = e;
    }                                       // the Emitter dies here
    std::cout << l.owner.lock()->count << std::endl;   // lock() is empty: undefined behaviour
    return 0;
}
```

This compiles silently and crashes. In CeeLing every read of a weak field goes through `lock()` and the null check, so the same mistake stops the program and names the line:

<!-- example: 04_weak_gone.clg -->
```cpp
// The same program with the hint: the link no longer keeps the Emitter alive.
// wire() returns the Listener only, so the Emitter is destroyed there.
class Emitter {
public:
    int count;
    Listener first;
    Emitter() { count = 7; first = nullptr; }
    void attach(Listener l) {
        l.source = this;
        first = l;
    }
};

class Listener {
public:
    //@ptr weak Listener.source
    Emitter source;
    Listener() { source = nullptr; }
    int sourceCount() { return source.count; }
};

Listener wire() {
    Listener l = new Listener();
    Emitter e = new Emitter();
    e.attach(l);
    return l;
}

int main() {
    Listener l = wire();
    std::cout << l.sourceCount() << std::endl;
    return 0;
}
```
<!-- output: 04_weak_gone.clg -->
```
cling: null dereference at line 19
```

It is an `abort()` (exit 3), not an exception, so no `catch` can swallow it. This is the run-time half of the guarantee, in [chapter 7](07-run-time-checks.md). Compare with the
`shared_ptr` version of the same program above: that one never fails and never frees the emitter. A weak link turns a silent leak into a loud, local failure; whether the failure
is reachable is a property of your program, not something the compiler proves.

## Long owning chains do not overflow the stack

An owner that owns the next owner means destroying the head destroys the tail recursively, one stack frame per node.

<!-- example: 04_deep_chain.cpp -->
```cpp
// A list of one million nodes, each owning the next. Building it works. Destroying it does not:
// the destructor of a node destroys its `next`, which destroys its `next`, one stack frame each.
#include <iostream>
#include <memory>

struct Item {
    std::unique_ptr<Item> next;
    int v;
    explicit Item(int x) : v(x) {}
};

int main() {
    auto head = std::make_unique<Item>(0);
    for (int i = 1; i < 1000000; i++) {
        auto n = std::make_unique<Item>(i);
        n->next = std::move(head);
        head = std::move(n);
    }
    std::cout << "built " << head->v << std::endl;
    return 0;                               // head is destroyed here: stack overflow, the process dies
}
```

This builds one million nodes and dies when `head` is destroyed (a stack overflow; exit code 127 on the Windows toolchain used for the guide). The same program in CeeLing works:

<!-- example: 04_long_chain.clg -->
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
    return 0;
}
```
<!-- output: 04_long_chain.clg -->
```
built 999999
```

A class with an owning field of its own type gets a generated destructor that moves its owning links onto a heap worklist and frees them in a loop. A node whose count is not 1 is only
released, never taken apart, so a second owner keeps its own links:

<!-- generated: 04_long_chain.clg "Item::~Item() {" -->
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

**Where it stops** (from the project's own notes, not a checked example): a self-type reached through a `std::vector<Item>` or through another class (A owns B owns A) still recurses, to
the depth of the data's own nesting; and a class with a base class or subclasses keeps the default destructor, so a deep chain of those can still overflow.

## Seeing the compiler's reasons

Every decision has a reason string in the analyzer. In the order they are tried: `explicit @ptr annotation`; `back-reference heuristic matched a backward edge` or `bidirectional
assignment closed a cycle` (weak); `bidirectional ownership needs a shared forward edge`; `copied between a field and another variable, so both are owners`; `member receives an existing
object reference, so ownership is shared`; `instances of this class participate in weak back-references`; `data-flow analysis found multiple live aliases`; and, when none applies,
`default unique ownership`. Later steps add more (`pushed into a container whose elements are shared`, a returned shared local, a stored parameter). The strings are from the compiler
source.

There is no flag that prints them for the default generator. The only way to see them is `--legacy-cpp`, the older generator, which writes the analyzer's report as a comment at the top
of its output (the analyzer itself runs the same way for both generators):

<!-- example: 04_report.clg -->
```cpp
class Node {
public:
    int v;
    Node next;
    Node prev;
    Node(int x) { v = x; next = nullptr; prev = nullptr; }
};

int main() {
    Node a = new Node(1);
    return 0;
}
```
<!-- generated: 04_report.clg "CeeLing pointer analysis report" -->
```cpp
CeeLing pointer analysis report
- Node.a -> std::shared_ptr because instances of this class participate in weak back-references (line 10)
- Node.next -> std::unique_ptr because default unique ownership (line 4)
- Node.prev -> std::weak_ptr because back-reference heuristic matched a backward edge (line 5)
*/
#include <utility>
#include <memory>
class Node {
public:
    int v;
    std::unique_ptr<Node> next;
    std::weak_ptr<Node> prev;
    Node(int x) { v = x; next = nullptr; prev.reset(); }
};
```

(The block starts inside the report comment; the `*/` ends it. The code under it is what the older generator writes, not what the normal compiler writes. Each report line reads `Type.name`: `Node.a` is the local `a` of type `Node`, `Texture.t` is a parameter.)

**Do not rely on the report to tell you the final kind.** It is written when the first decision is made, before the later steps that promote a field to shared (an element copied out,
a parameter that is stored, a returned local). In the next program the normal compiler's output has `std::shared_ptr<Texture> texture`, and the report still says unique:

<!-- example: 04_report_stale.clg -->
```cpp
class Texture {
public:
    int id;
    Texture(int i) { id = i; }
};

class Sprite {
public:
    Texture texture;
    Sprite(Texture t) { texture = t; }
};

int main() {
    Texture rock = new Texture(7);
    Sprite a = new Sprite(rock);
    Sprite b = new Sprite(rock);
    return 0;
}
```
<!-- generated: 04_report_stale.clg "CeeLing pointer analysis report" -->
```cpp
CeeLing pointer analysis report
- Sprite.a -> std::unique_ptr because default unique ownership (line 15)
- Sprite.b -> std::unique_ptr because default unique ownership (line 16)
- Texture.rock -> std::unique_ptr because default unique ownership (line 14)
- Texture.t -> std::unique_ptr because default unique ownership
- Texture.texture -> std::unique_ptr because default unique ownership (line 9)
*/
#include <utility>
#include <memory>
class Texture {
public:
    int id;
    Texture(int i) { id = i; }
};
```

The ground truth is the generated C++: compile with `--cpp out.cpp` and read the field and variable declarations.

## `--pool-shared`: cheaper shared objects

A `shared_ptr` made with `make_shared` costs an allocation for the object and its count. With `--pool-shared` every shared `new` becomes
`std::allocate_shared<T>(cling::PoolAllocator<T>(), ...)`: the object and its count come from a per-thread free list instead of `malloc`.

<!-- example: 04_pool_shared.clg -->
```cpp
// Compiled with --pool-shared: the same program, but every shared object comes from a per-thread free list.
class Node {
public:
    Node next;
    Node prev;
    int v;
    Node(int x) { v = x; next = nullptr; prev = nullptr; }
};

int main() {
    Node first = new Node(1);
    Node second = new Node(2);
    first.next = second;
    second.prev = first;
    Node third = new Node(3);
    second.next = third;
    third.prev = second;
    std::cout << first.v + second.v + third.v << std::endl;
    return 0;
}
```
<!-- output: 04_pool_shared.clg -->
```
6
```
<!-- generated: 04_pool_shared.clg "int main()" -->
```cpp
int main() {
    std::shared_ptr<Node> first = std::allocate_shared<Node>(cling::PoolAllocator<Node>(), 1);
    std::shared_ptr<Node> second = std::allocate_shared<Node>(cling::PoolAllocator<Node>(), 2);
    cling::nn(first, "line 13")->next = second;
    cling::nn(second, "line 14")->prev = first;
    std::shared_ptr<Node> third = std::allocate_shared<Node>(cling::PoolAllocator<Node>(), 3);
    cling::nn(second, "line 16")->next = third;
    cling::nn(third, "line 17")->prev = second;
    std::cout << ((cling::nn(first, "line 18")->v + cling::nn(second, "line 18")->v) + cling::nn(third, "line 18")->v) << std::endl;
    return 0;
}
```

Behaviour does not change; only where the memory comes from. The project's notes record the motivating measurement: on the cJSON port, whose parser is dominated by node allocation,
parse throughput went from 50 to 84 MB/s (the C original measured 79 MB/s on the same machine). What it costs: `unique_ptr` objects are not pooled (a custom deleter would change the
handle's type); memory taken by the pool is kept for reuse and never returned to the system; the free lists are per thread. It is off by default.

## What the inference cannot decide, and what it costs

* **It never refuses to choose; it falls back to shared.** The project's design rule is "ambiguous: reject". For ownership kinds the compiler does not do that: when two uses of a
  name disagree it takes `shared_ptr`, which is always safe to use but slower and can hide a cycle. Rejections in this area come from later checks (a use of a moved-from
  variable, [chapter 5](05-moves.md); a `spawn` capture, [chapter 10](10-threads.md)), not from the kind choice itself.
* **A cycle without a recognised back link leaks, silently.** That is the cost of choosing `shared` when in doubt; the cure is the hint.
* **Some shapes generate C++ that g++ rejects.** While writing this chapter, two shapes were found where two fields assigned from the same local get different kinds and the
  generated C++ does not compile (`conversion from shared_ptr to unique_ptr`): a list class with `head` and `tail` fields that are both assigned one local node, and a class with two
  fields both assigned from one local. They are not in the test suite and not a checked example (the checker only shows programs that work); they were seen by running the compiler.
  No unsafe code is produced, but the error is g++'s, not a clean CeeLing message.
* **`shared_ptr` has a cost and a limit.** Its count is atomic, so copying a handle is slower than copying a `unique_ptr`. And the atomic count protects only the count: the object it
  points to is not protected, so two threads each holding a handle and mutating the object race. CeeLing therefore does not let a shared object into a thread:

<!-- example: 04_shared_not_sendable.clg -->
```cpp
class Counter {
public:
    int n;
    Counter() { n = 0; }
};

class Holder {
public:
    Counter held;
    Holder(Counter c) { held = c; }
};

class Job {
public:
    Job() { }
    virtual void run() { }
};

int main() {
    Counter mine = new Counter();
    Holder holder = new Holder(mine);      // two owners now: mine and holder.held
    spawn(closure Job [mine] () {
        mine.n = mine.n + 1;
    });
    return 0;
}
```
<!-- error: 04_shared_not_sendable.clg -->
```
error[semantic/spawn-captures-object]: `spawn` cannot move `mine` into a new thread: `mine` has more than one owner (it is stored elsewhere or shared), so moving it would leave a live alias in this thread
  --> 04_shared_not_sendable.clg:22:5
  help: an object may be moved into a thread only if nothing else can reach it and nothing about it is hidden: make the data a `struct` (copied), or serialize it to a string and rebuild it inside the thread
```

  The rule is stated exactly in [chapter 10](10-threads.md): only plain values and objects provably owned by one owner cross into `spawn`.
* **The pointer kind is a property of the whole program.** Change a use in one function and a field's type in another file's header can change, which is why separately compiled files
  exchange "demands" (`--ptr-demands-out`, `--ptr-demands`); see [chapter 12](12-many-files.md).

## Summary

| You write | The compiler decides from | C++ it writes |
|---|---|---|
| `Node next;` and the object has one owner | no second owner anywhere | `std::unique_ptr<Node>` |
| a copy of a handle that stays in use | a second live handle (local, field, parameter, `this`, element, returned local) | `std::shared_ptr<Node>` |
| `Node prev;` closing a cycle | a back-link name, or two objects pointing at each other | `std::weak_ptr<Node>` |
| `std::vector<Node>` | every use of that container, by name | `vector<unique_ptr>` or `vector<shared_ptr>`, one for all |
| `//@ptr weak Node.parent` | your word | the kind you named |

Next: [5. Moves](05-moves.md).
