# 5. Moves

In CeeLing you do not write `std::move`. Passing an object to a function, assigning it, returning it or storing it hands ownership over, and the compiler writes the `std::move`
at the place where the old name is no longer used. Because it inserts the moves, it must also know where they leave a name empty, and it refuses to compile a program that uses an
emptied name. This chapter is about both halves: what is moved, and what the compiler rejects afterwards.

The mechanism that tracks "this name no longer has its object" is the same table that tracks dangling references ([chapter 6](06-dangling-references.md), [chapter 14](14-how-it-works.md)).
The pointer kinds that a move works on (`unique_ptr` moves, `shared_ptr` is copied) are [chapter 4](04-ownership.md).

## The C++ problem: `std::move` is a cast

### Forgetting it copies

<!-- example: 05_cpp_silent_copy.cpp -->
```cpp
// Forgetting std::move: the vector is copied, one million ints, on every call. Nothing says so.
#include <iostream>
#include <vector>

long long consume(std::vector<int> data) {      // by value: the caller's argument is copied unless it is moved in
    return (long long)data.size();
}

int main() {
    std::vector<int> big(1000000, 7);
    long long n = consume(big);                 // a copy; write std::move(big) to hand it over
    std::cout << n << " " << big.size() << std::endl;
    return 0;
}
```

`consume` takes its vector by value, so the call copies a million ints. `g++ -Wall -Wextra` says nothing; the program is correct and slow. Writing `std::move(big)` would avoid the
copy, but the compiler does not ask for it, and a reader of `consume(big)` cannot tell whether the argument survives.

### Moving too early leaves a usable husk

<!-- example: 05_cpp_moved_then_used.cpp -->
```cpp
// Moving too early: the source is still a name you can use. For std::vector and std::string it is
// usually empty afterwards, but the standard only promises "valid but unspecified".
#include <iostream>
#include <string>
#include <vector>

void consume(std::vector<std::string> v) { std::cout << "consumed " << v.size() << std::endl; }

int main() {
    std::vector<std::string> names;
    names.push_back("a");
    names.push_back("b");
    consume(std::move(names));
    std::cout << names.size() << std::endl;     // compiles; prints 0 with libstdc++, but nothing requires it
    names.push_back("c");                       // and the object is still usable, which hides the bug
    return 0;
}
```

`std::move(names)` does not move anything. It is a cast that lets the callee take the contents. The variable `names` stays, with a valid but unspecified value, and nothing stops the
next line from using it. With libstdc++ the vector is empty afterwards, so the program prints `0`; the standard does not promise it, and the code after it keeps working, which
hides the mistake. For a `unique_ptr` the same mistake is a null dereference.

### Moving in a loop, moving on one path

<!-- example: 05_cpp_move_in_loop.cpp -->
```cpp
// A move inside a loop: the first round takes the string, every later round sees an empty one.
#include <iostream>
#include <string>
#include <vector>

int main() {
    std::string line = "hello";
    std::vector<std::string> lines;
    for (int i = 0; i < 3; i++) {
        lines.push_back(std::move(line));       // round 1 moves "hello" out; rounds 2 and 3 push ""
    }
    for (const auto& s : lines) std::cout << "[" << s << "]";
    std::cout << std::endl;                     // [hello][][] with libstdc++
    return 0;
}
```

<!-- example: 05_cpp_move_in_branch.cpp -->
```cpp
// A move on one path only: after the if, the object may or may not have been emptied.
#include <iostream>
#include <memory>

struct Node {
    int v;
    explicit Node(int x) : v(x) {}
};

void consume(std::unique_ptr<Node>) {}

int main(int argc, char**) {
    auto n = std::make_unique<Node>(1);
    if (argc > 0) {
        consume(std::move(n));                  // taken whenever the program has a name
    }
    std::cout << n->v << std::endl;             // null dereference on that path
    return 0;
}
```

The first program prints `[hello][][]`: the move in round one takes the string and the later rounds push empty ones. The second dereferences an empty `unique_ptr` on the path where
the call happened (it crashes). Neither compiles with a warning.

### Storing a parameter

<!-- example: 05_cpp_stored_param.cpp -->
```cpp
// Storing a by-value parameter into a field without std::move copies it a second time.
#include <iostream>
#include <vector>

struct Holder {
    std::vector<int> data;
    void take(std::vector<int> bytes) { data = bytes; }              // copy: bytes is a local, and this is its last use
    void takeMoved(std::vector<int> bytes) { data = std::move(bytes); }
};

int main() {
    Holder h;
    h.take(std::vector<int>(1000000, 1));
    h.takeMoved(std::vector<int>(1000000, 2));
    std::cout << h.data.size() << std::endl;
    return 0;
}
```

`take` copies its by-value parameter into the field although `bytes` is a local that is about to be destroyed; `takeMoved` does not. Whoever writes `take` has to know that this is the
last use of `bytes`.

**Why C++ does not stop any of this.** `std::move` is a cast to an rvalue reference. It has no effect on the variable, so the type of `names` after the call is the same as before it,
and the language has no notion of "this name is dead now". Whether a variable is used later is a question about the control flow of the whole function (loops, branches, exceptions),
and a C++ compiler does not track it for user types. Linters and `clang-tidy`'s `bugprone-use-after-move` check part of it; they are a separate tool and a heuristic, not part of the
language.

**Why CeeLing can.** It controls both sides: it writes the moves, so it knows exactly which statements empty which names, and it follows control flow through `if`, loops,
`break`, `continue` and `try`. A name that has been emptied is simply not usable until it is given a new value. The cost is that the analysis is conservative: where it cannot prove a
use is safe on every path, it says no.

## What CeeLing does: ownership goes with the value

A plain assignment or a call argument hands ownership over, and the compiler writes `std::move` where the source is not used again.

<!-- example: 05_hand_over.clg -->
```cpp
class Item {
public:
    int v;
    Item(int x) { v = x; }
};

void drop(Item gone) { }                    // takes the object and lets it go

int main() {
    Item x = new Item(9);
    Item y = x;                             // x is never used again: ownership moves to y
    std::cout << y.v << " ";
    drop(y);                                // y is never used again: ownership moves into drop
    std::cout << "done" << std::endl;
    return 0;
}
```
<!-- output: 05_hand_over.clg -->
```
9 done
```
<!-- generated: 05_hand_over.clg "int main()" -->
```cpp
int main() {
    std::unique_ptr<Item> x = std::make_unique<Item>(9);
    std::unique_ptr<Item> y = std::move(x);
    std::cout << cling::nn(y, "line 12")->v << " ";
    drop(std::move(y));
    std::cout << "done" << std::endl;
    return 0;
}
```

`Item y = x;` became `std::move(x)` because `x` is never mentioned again. `drop(y)` became `drop(std::move(y))` for the same reason. A parameter that the function does not use still
takes ownership (an unused parameter is not treated as read-only; the reason is the positive-evidence rule below).

**A shared handle is copied, never moved.** When the object has several owners ([chapter 4](04-ownership.md)) the argument is a `shared_ptr`, and passing it copies the handle and
leaves the source alive. Moving it would invalidate a name that is, by definition, still one of the owners.

<!-- example: 05_shared_not_moved.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

class Holder {
public:
    Node stored;
    Holder() { }
    void keep(Node n) { stored = n; }
};

int main() {
    Node a = new Node(1);
    Holder h = new Holder();
    h.keep(a);                             // n is stored, so a and h.stored share one object
    std::cout << a.v << " " << h.stored.v << std::endl;
    return 0;
}
```
<!-- output: 05_shared_not_moved.clg -->
```
1 1
```
<!-- generated: 05_shared_not_moved.clg "void Holder::keep" -->
```cpp
void Holder::keep(std::shared_ptr<Node> n) {
    this->stored = n;
}
```
<!-- generated: 05_shared_not_moved.clg "int main()" -->
```cpp
int main() {
    std::shared_ptr<Node> a = std::make_shared<Node>(1);
    std::unique_ptr<Holder> h = std::make_unique<Holder>();
    cling::nn(h, "line 17")->keep(a);
    std::cout << cling::nn(a, "line 18")->v << " " << cling::nn(cling::nn(h, "line 18")->stored, "line 18")->v << std::endl;
    return 0;
}
```

`keep` stores its parameter in a field, so `a` and `h.stored` are two owners, `a` is a `shared_ptr`, `h.keep(a)` copies it, and `a` is still usable on the next line. The same
call in a program where `a` stays unique would have moved it, and the next line would be an error.

## Use after a move is a compile error

### An object, a vector, a map, a struct

The simplest case is a name used after it was given away. [Chapter 1](01-why-ceeling.md) shows it for an object; here it is for the other things an argument can be.

An object returned or stored by the callee is handed over:

<!-- example: 05_object_returned.clg -->
```cpp
class Box {
public:
    int value;
    Box(int v) { value = v; }
};

Box keep(Box n) {
    return n;                              // hands n to the caller: not a read
}

int main() {
    Box b = new Box(1);
    Box kept = keep(b);
    std::cout << b.value << std::endl;
    return 0;
}
```
<!-- error: 05_object_returned.clg -->
```
error[semantic/use-after-move]: use of moved-from value `b`
  --> 05_object_returned.clg:14:18
  help: `b` was moved earlier; assign a new value before using it
```

A vector that the callee changes is handed over (the callee owns a modified copy; the caller's name is dead):

<!-- example: 05_use_after_move_vector.clg -->
```cpp
void consume(std::vector<string> v) {
    v.push_back("z");                      // changes its argument, so the argument is handed over
    std::cout << v.size() << std::endl;
}

int main() {
    std::vector<string> names;
    names.push_back("a");
    consume(names);
    std::cout << names.size() << std::endl;
    return 0;
}
```
<!-- error: 05_use_after_move_vector.clg -->
```
error[semantic/use-after-move]: use of moved-from value `names`
  --> 05_use_after_move_vector.clg:10:18
  help: `names` was moved earlier; assign a new value before using it
```

A map is always handed over to a by-value parameter (the read-only rule below is for vectors of numbers and for strings and objects, not maps):

<!-- example: 05_map_moved.clg -->
```cpp
void sink(std::map<int, string> m) {
    m[3] = "c";
}

int main() {
    std::map<int, string> m;
    m[1] = "one";
    sink(m);
    m[2] = "two";
    return 0;
}
```
<!-- error: 05_map_moved.clg -->
```
error[semantic/use-after-move]: use of moved-from value `m`
  --> 05_map_moved.clg:9:5
  help: `m` was moved earlier; assign a new value before using it
```

A `struct` is a value type and is copied in an assignment or a declaration, so it does not die there:

<!-- example: 05_struct_copied.clg -->
```cpp
struct Point {
public:
    int v;
    Point(int x) { v = x; }
};

int main() {
    Point a = new Point(1);
    Point b = a;            // a struct is a value: this copies, and a stays usable
    Point c = a;
    std::cout << b.v << " " << c.v << std::endl;
    return 0;
}
```
<!-- output: 05_struct_copied.clg -->
```
1 1
```
<!-- generated: 05_struct_copied.clg "int main()" -->
```cpp
int main() {
    Point a = Point(1);
    Point b = a;
    Point c = a;
    std::cout << b.v << " " << c.v << std::endl;
    return 0;
}
```

but passed to a function that does not only read it, it moves, exactly like an object:

<!-- example: 05_struct_consumed.clg -->
```cpp
struct Point {
public:
    int v;
    Point(int x) { v = x; }
};

void sink(Point n) { }

int main() {
    Point a = new Point(1);
    sink(a);
    std::cout << a.v << std::endl;
    return 0;
}
```
<!-- error: 05_struct_consumed.clg -->
```
error[semantic/use-after-move]: use of moved-from value `a`
  --> 05_struct_consumed.clg:12:18
  help: `a` was moved earlier; assign a new value before using it
```

A `string` is different again: strings are copied into a by-value parameter and never die, and a string that the callee only reads is passed by `const std::string&`, so it is not
even copied:

<!-- example: 05_string_copied.clg -->
```cpp
void shout(string s) {
    s = s + "!";
    std::cout << s << std::endl;
}

void show(string s) {
    std::cout << s << std::endl;
}

int main() {
    string t = "hi";
    shout(t);           // a string is a value: the callee gets a copy, t stays usable
    show(t);            // read only: passed by const reference, not copied at all
    std::cout << t << std::endl;
    return 0;
}
```
<!-- output: 05_string_copied.clg -->
```
hi!
hi
hi
```
<!-- generated: 05_string_copied.clg "void shout" -->
```cpp
void shout(std::string s) {
    s = (s + "!");
    std::cout << s << std::endl;
}
```
<!-- generated: 05_string_copied.clg "void show" -->
```cpp
void show(const std::string& s) {
    std::cout << s << std::endl;
}
```

### Moves in loops

A move inside a loop of something declared outside the loop is a use-after-move on the next round, even though no line after the move uses the name:

<!-- example: 05_loop_move.clg -->
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
        std::cout << a.v << std::endl;     // the second time round, a is gone
        sink(a);
    }
    return 0;
}
```
<!-- error: 05_loop_move.clg -->
```
error[semantic/use-after-move]: use of moved-from value `a`
  --> 05_loop_move.clg:14:22
  help: `a` was moved earlier; assign a new value before using it
error[semantic/use-after-move]: use of moved-from value `a`
  --> 05_loop_move.clg:15:14
  help: `a` was moved earlier; assign a new value before using it
```

The checker follows the loop back to its head until the set of dead names stops changing, so it sees what the second round would do. Two things make the same shape legal. A move
followed by a `break` never comes back to the use:

<!-- example: 05_loop_move_break.clg -->
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
        std::cout << a.v << std::endl;
        sink(a);
        break;                             // the loop never comes back to the use
    }
    return 0;
}
```
<!-- output: 05_loop_move_break.clg -->
```
1
```

and a move followed by a new value, in the same round, leaves the name alive at the head of the next round:

<!-- example: 05_revive.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

void sink(Node unused) { }

int main() {
    Node cur = new Node(1);
    int i = 0;
    while (i < 3) {
        i = i + 1;
        std::cout << cur.v << ";";
        sink(cur);                 // cur is moved here ...
        cur = new Node(i + 1);     // ... and has a new value before the next iteration uses it
    }
    std::cout << std::endl;
    return 0;
}
```
<!-- output: 05_revive.clg -->
```
1;2;3;
```
<!-- generated: 05_revive.clg "int main()" -->
```cpp
int main() {
    std::unique_ptr<Node> cur = std::make_unique<Node>(1);
    int i = 0;
    while ((i < 3)) {
        i = (i + 1);
        std::cout << cling::nn(cur, "line 14")->v << ";";
        sink(std::move(cur));
        cur = std::make_unique<Node>((i + 1));
    }
    std::cout << std::endl;
    return 0;
}
```

### Assigning a new value revives a name

`cur = new Node(i + 1);` after `sink(cur)` gives `cur` a new object; it is usable again, and in the example above the next round's `std::cout << cur.v` is accepted. This is the only
way back: no other statement makes a moved-from name usable.

### `break` and `continue`

The state at a `break` or `continue` flows to the code after the loop, because the loop may end there. A move on that path makes the name dead after the loop:

<!-- example: 05_break_then_use.clg -->
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
            sink(a);
            break;                         // leaves the loop with a moved
        }
    }
    std::cout << a.v << std::endl;         // after the loop, a may or may not have been moved
    return 0;
}
```
<!-- error: 05_break_then_use.clg -->
```
error[semantic/use-after-move]: use of moved-from value `a`
  --> 05_break_then_use.clg:19:18
  help: `a` was moved earlier; assign a new value before using it
```

<!-- example: 05_continue_then_use.clg -->
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
            sink(a);
            continue;
        }
    }
    std::cout << a.v << std::endl;
    return 0;
}
```
<!-- error: 05_continue_then_use.clg -->
```
error[semantic/use-after-move]: use of moved-from value `a`
  --> 05_continue_then_use.clg:15:18
  help: `a` was moved earlier; assign a new value before using it
error[semantic/use-after-move]: use of moved-from value `a`
  --> 05_continue_then_use.clg:19:18
  help: `a` was moved earlier; assign a new value before using it
```

### `if` and `else`: dead on any path is dead

After a branch, a name is usable only if it is usable on every path through it. One branch that moves is enough:

<!-- example: 05_branch_move.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

void sink(Node n) { }

int main() {
    Node a = new Node(1);
    bool full = true;
    if (full) {
        sink(a);                           // moved on this path only
    }
    std::cout << a.v << std::endl;         // dead on any path is dead
    return 0;
}
```
<!-- error: 05_branch_move.clg -->
```
error[semantic/use-after-move]: use of moved-from value `a`
  --> 05_branch_move.clg:15:18
  help: `a` was moved earlier; assign a new value before using it
```

If every path that moves also gives the name a new value, it is alive again at the join:

<!-- example: 05_branch_reassigned.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

void sink(Node n) { }

int main() {
    Node a = new Node(1);
    bool full = true;
    if (full) {
        sink(a);
        a = new Node(2);                   // every path that moved a gives it a new value
    }
    std::cout << a.v << std::endl;
    return 0;
}
```
<!-- output: 05_branch_reassigned.clg -->
```
2
```

### `try` and `catch`

A handler may run after the call in the `try` body has already taken the object, so a name moved in the body is dead in the handler:

<!-- example: 05_try_move.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

void sink(Node n) { }

int main() {
    Node a = new Node(1);
    try {
        sink(a);
    } catch (string e) {
        std::cout << a.v << std::endl;     // the handler may run after sink took a
    }
    return 0;
}
```
<!-- error: 05_try_move.clg -->
```
error[semantic/use-after-move]: use of moved-from value `a`
  --> 05_try_move.clg:14:22
  help: `a` was moved earlier; assign a new value before using it
```

**Where it stops.** The checker follows local variables and parameters by name, over `if`, loops, `break`, `continue` and `try`. It decides from control flow alone, not from
values: in `05_branch_move` the condition happens to be `true`, and the program is rejected anyway, because the checker does not evaluate conditions. Where it cannot prove a use is safe on
every path it says no, so a correct program can be refused. Writing the program so that each name has one clear last use is always enough.

## Read-only parameters are not consumed

A function that only reads its parameter should not take the caller's object. C++ has `const T&` for that; CeeLing has no reference parameters, so the compiler infers it.

An object parameter is read-only if the function body only reads fields of it (`n.value`), compares it with `nullptr`, or passes it on to something that is itself read-only. Then the
caller's name stays alive:

<!-- example: 05_read_only_object.clg -->
```cpp
class Box {
public:
    int value;
    Box(int v) { value = v; }
};

int describe(Box n) {
    return n.value * 2;                    // only reads n
}

int main() {
    Box a = new Box(1);
    std::cout << "original=" << a.value << " doubled=" << describe(a) << " original-after=" << a.value << std::endl;
    return 0;
}
```
<!-- output: 05_read_only_object.clg -->
```
original=1 doubled=2 original-after=1
```
<!-- generated: 05_read_only_object.clg "int describe" -->
```cpp
int describe(std::shared_ptr<Box> n) {
    return (cling::nn(n, "line 8")->value * 2);
}
```

The parameter is a `shared_ptr` that the caller copies, so `a` is alive afterward. Anything that makes the parameter an owner (returning it, storing it) makes it a consuming
parameter again, as `05_object_returned` above shows. A parameter that is never used is not read-only: there must be at least one real read, otherwise "unused" would silently
change the meaning of `drop(x)`.

For a `std::vector` of numbers the saving is bigger, because the alternative is copying the whole vector. A vector parameter that is only subscripted for reading, asked for `.size()`,
iterated by a range-for, or passed on to another read-only vector parameter becomes `const std::vector<T>&`:

<!-- example: 05_vector_read_only.clg -->
```cpp
int sumAll(std::vector<int> data) {
    int total = 0;
    int i = 0;
    while (i < data.size()) {
        total = total + data[i];
        i = i + 1;
    }
    return total;
}

int main() {
    std::vector<int> v;
    v.push_back(1);
    v.push_back(2);
    v.push_back(3);
    int s = sumAll(v);
    std::cout << s << " " << v.size() << std::endl;
    return 0;
}
```
<!-- output: 05_vector_read_only.clg -->
```
6 3
```
<!-- generated: 05_vector_read_only.clg "int sumAll" -->
```cpp
int sumAll(const std::vector<int>& data) {
    int total = 0;
    int i = 0;
    while ((i < data.size())) {
        total = (total + cling::at(data, i, 5));
        i = (i + 1);
    }
    return total;
}
```
<!-- generated: 05_vector_read_only.clg "int main()" -->
```cpp
int main() {
    std::vector<int> v;
    v.push_back(1);
    v.push_back(2);
    v.push_back(3);
    int s = sumAll(std::move(v));
    std::cout << s << " " << v.size() << std::endl;
    return 0;
}
```

Two details in the generated code. The parameter is a `const&`, so nothing is copied. And the call is still written `sumAll(std::move(v))`; passing an rvalue to a `const&` parameter
binds it without moving anything, so `v` keeps its contents, and the checker does not mark it dead. (The `std::move` here is harmless; the read-only classification is what exempts the
argument from the use-after-move rule.) A vector parameter that is mutated (`push_back`, `pop_back`, `resize`, a subscript write), stored or returned stays by value and consumes its
argument, as `05_use_after_move_vector` shows.

What this was worth in the one case measured (zlib-cling, a sibling project; not re-run for this guide): its `decodeSymbol` copied four tables of about 512 entries on every decoded symbol, and passing them
as `const&` alone made decompression 9.7 times faster.

**Known gap, found while writing this chapter.** The read-only classification is looked up by *parameter name* somewhere in the pipeline. In a program with `int describe(Box n)` (which only
reads `n`) and `void sink(Box n) { }` (which does not), the call `sink(a); ... a.value` is not rejected by CeeLing. What happens then is that the generated C++ does not compile (g++:
`could not convert 'a' from 'std::shared_ptr<Box>' to 'std::unique_ptr<Box>'`), so no unsafe program is produced, but the error is g++'s and not the use-after-move message. Renaming `sink`'s
parameter gives the proper message. With a `struct` the same name clash is accepted and compiles, since the call copies. It is not in the test suite and not a checked example here, because
the checker shows only what works. Give parameters of different functions different names until it is fixed. The same name matching explains the hint problem in
[chapter 4](04-ownership.md).

When the argument is a *field* rather than a local, `const&` is only safe if the callee cannot change that field's storage while it runs (it is "heap-pure": it mutates no container or
object it does not own locally, and reassigns no object field). If the compiler cannot prove that, it passes an explicit copy at that call site instead. The result is still correct, just
not maximally fast. This applies to `std::vector` of numbers only; `string` read-only parameters are `const std::string&` as shown above.

## Storing a parameter: the last-use exception

`data = bytes;` where `bytes` is a vector parameter and `data` a field is a **share** by the general rule: the field and the parameter are both live afterwards, because in general a
parameter stored in a field is still used (a linked-list node is attached to a field and used again). So the general rule is a copy. That is the C++ program `take` at the start of the
chapter, and it is wrong for the common case in which `bytes` is never touched again.

The compiler proves the narrower fact for one shape. If a method or function body is straight-line code (no `if`, `while`, `for`, `try` or closure anywhere in it), contains exactly one
`field = parameter;` where the parameter is a vector of numbers, and no later statement mentions the parameter, that one assignment is a move:

<!-- example: 05_field_last_use.clg -->
```cpp
class Holder {
public:
    std::vector<int> data;
    Holder() { }
    void take(std::vector<int> bytes) {
        data = bytes;                      // the last use of bytes: moved, not copied
    }
    int sum() {
        int total = 0;
        int i = 0;
        while (i < data.size()) {
            total = total + data[i];
            i = i + 1;
        }
        return total;
    }
};

int main() {
    Holder h = new Holder();
    std::vector<int> v;
    v.push_back(1);
    v.push_back(2);
    h.take(v);
    std::cout << h.sum() << std::endl;
    return 0;
}
```
<!-- output: 05_field_last_use.clg -->
```
3
```
<!-- generated: 05_field_last_use.clg "void Holder::take" -->
```cpp
void Holder::take(std::vector<int> bytes) {
    this->data = std::move(bytes);
}
```

If the parameter is used after the store, the store must copy:

<!-- example: 05_field_stays_live.clg -->
```cpp
class Holder {
public:
    std::vector<int> data;
    Holder() { }
    int take(std::vector<int> bytes) {
        data = bytes;
        return bytes.size();               // bytes is used after the store: the store must copy
    }
};

int main() {
    Holder h = new Holder();
    std::vector<int> v;
    v.push_back(1);
    v.push_back(2);
    int n = h.take(v);
    std::cout << n << " " << h.data.size() << std::endl;
    return 0;
}
```
<!-- output: 05_field_stays_live.clg -->
```
2 2
```
<!-- generated: 05_field_stays_live.clg "int Holder::take" -->
```cpp
int Holder::take(std::vector<int> bytes) {
    this->data = bytes;
    return bytes.size();
}
```

and the rule gives up on any branching in the body, even after the assignment, trading missed moves for a rule small enough to audit:

<!-- example: 05_field_branchy.clg -->
```cpp
// The rule gives up on any if/while/for/try in the body, even after the store: the store copies.
class Holder {
public:
    std::vector<int> data;
    Holder() { }
    void take(std::vector<int> bytes, bool keep) {
        data = bytes;
        if (keep) {
            data.push_back(3);
        }
    }
};

int main() {
    Holder h = new Holder();
    std::vector<int> v;
    v.push_back(1);
    v.push_back(2);
    h.take(v, true);
    std::cout << h.data.size() << std::endl;
    return 0;
}
```
<!-- output: 05_field_branchy.clg -->
```
3
```
<!-- generated: 05_field_branchy.clg "void Holder::take" -->
```cpp
void Holder::take(std::vector<int> bytes, bool keep) {
    this->data = bytes;
    if (keep) {
        this->data.push_back(3);
    }
}
```

A missed move is a lost optimisation, never a wrong result: the copy is what the general rule specifies. A second qualifying assignment in the same body is treated as ambiguous and not
moved either. This is for vectors of numbers only.

The failure that motivated all the care is real: an earlier version of this optimisation emitted a move for every field assignment and generated `this->data = std::move(bytes);` followed by
a use of `bytes`. It compiled, ran and printed the wrong count. That is why the move is limited to a proven last use.

## Reading the generated C++ to see the moves

You do not have to trust the rules: the moves are in the output. Compile with `--cpp out.cpp` and look at the call sites.

<!-- example: 05_read_generated.clg -->
```cpp
class Item {
public:
    int price;
    Item(int p) { price = p; }
};

class Cart {
public:
    std::vector<Item> items;
    Cart() { }
    void add(Item it) { items.push_back(it); }
    int total() { return items.size(); }
};

int count(std::vector<int> values) {
    return values.size();
}

void takeAll(std::vector<int> values) {
    values.push_back(1);
}

int main() {
    Cart cart = new Cart();
    Item pen = new Item(2);
    cart.add(pen);                 // pen is not used again: moved into the cart
    std::vector<int> v;
    v.push_back(1);
    v.push_back(2);
    v.push_back(3);
    std::cout << cart.total() + 1 << " " << count(v) << " ";   // read only: passed by const reference
    takeAll(v);                    // changes its argument: v is handed over
    std::cout << cart.total() << std::endl;
    return 0;
}
```
<!-- output: 05_read_generated.clg -->
```
2 3 1
```
<!-- generated: 05_read_generated.clg "int count" -->
```cpp
int count(const std::vector<int>& values) {
    return values.size();
}
```
<!-- generated: 05_read_generated.clg "int main()" -->
```cpp
int main() {
    std::unique_ptr<Cart> cart = std::make_unique<Cart>();
    std::unique_ptr<Item> pen = std::make_unique<Item>(2);
    cling::nn(cart, "line 26")->add(std::move(pen));
    std::vector<int> v;
    v.push_back(1);
    v.push_back(2);
    v.push_back(3);
    std::cout << (cling::nn(cart, "line 31")->total() + 1) << " " << count(std::move(v)) << " ";
    takeAll(std::move(v));
    std::cout << cling::nn(cart, "line 33")->total() << std::endl;
    return 0;
}
```

* `->add(std::move(pen))`: `pen` is a unique object and is not used again; the cart's vector takes it.
* `count(const std::vector<int>& values)`: read only. The call is written with `std::move(v)`, which only binds; `v` is still `3` long when `takeAll` runs.
* `takeAll(std::move(v))`: `takeAll` changes its parameter, so `v` is handed over, and a later use of `v` would have been rejected.

A rule of thumb for a reader of the output: a `std::move` on an argument whose parameter is a plain `std::unique_ptr`/`std::vector`/`std::map` is a real hand-over; on a `const&` or
`shared_ptr` parameter it is harmless.

## Summary

| Situation | What the compiler writes | Using the name afterwards |
|---|---|---|
| unique object assigned or passed, not used again | `std::move(x)` | rejected until reassigned |
| object with several owners | a copy of the `shared_ptr` | allowed |
| `struct` in a declaration or assignment | a copy | allowed |
| `struct`, vector or map to a function that changes, stores or returns it | `std::move` | rejected |
| parameter only read (object) | copy of the handle | allowed |
| `std::vector` of numbers, only read | `const std::vector<T>&` | allowed |
| `string` | a copy, or `const std::string&` if only read | allowed |
| `field = vectorParam;` at the proven last use | `std::move` | (no later use exists) |
| move inside a loop, on one branch, before `break`/`continue`, inside `try` | `std::move` | rejected on every path where it could be dead |

Next: [6. Dangling references](06-dangling-references.md).
