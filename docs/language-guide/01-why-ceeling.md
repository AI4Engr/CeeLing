# 1. Why CeeLing

CeeLing is a small language that looks like Java and compiles to ordinary C++. You write objects and method calls; the compiler works out who owns what, writes the smart pointers
and the moves, and **refuses to produce a program in which a reference can outlive the object it refers to**. The output is plain C++ that links with the standard library, Eigen
or any other C++ code you already have.

It exists for one reason: **a C++ program that compiles can still be wrong in ways the C++ compiler cannot see**, and the usual remedies (review, sanitizers, a rewrite in another
language) either miss the bug, find it only when the bad path runs, or throw away the libraries you need.

## The promise

> **If a program compiles under CeeLing, it contains no dangling references and no use of a moved-from object. Out-of-range indexing and null dereference of an object stop the
> program on the spot and name the source line, instead of corrupting memory.**

That sentence is the whole contract. Everything else in this guide is how it is kept, what it costs, and where it stops. It is tested, not asserted: `tests/safety/` holds more
than 300 programs that must be accepted or rejected exactly as written, and every example in this guide is compiled by the same compiler each time the guide is checked.

Threads are covered in one narrow, tested way: a thread started with `spawn` can reach only its own copies, objects moved into it, and `Channel` state, so there are no data races through `spawn` or `Channel` (chapter 10). What the promise is **not**: it is not Rust's guarantee, it does not make a program "thread-safe" (deadlock and state reached through C++ are unchecked), and it does not cover C++ you call (`ext_fn`, chapter 11) or libraries you link. The exact scope is in [chapter 13](13-contract-and-threats.md) and the comparison with Rust in [chapter 17](17-comparisons.md).

## What C++ cannot tell you, and CeeLing can

Each section below is one thing that goes wrong in C++. The C++ program is real: it compiles with `g++ -Wall -Wextra` and, in most cases, says nothing. Below it is the same idea
in CeeLing, and what the compiler does.

### 1. A reference that outlives its object

<!-- example: 01_dangling_after_move.cpp -->
```cpp
#include <iostream>
#include <memory>

struct Node {
    int key;
    explicit Node(int k) : key(k) {}
};

void sink(std::unique_ptr<Node>) {}      // takes the object and destroys it

int main() {
    auto root = std::make_unique<Node>(1);
    Node& n = *root;                       // n looks at the object
    sink(std::move(root));                 // the object is destroyed here
    std::cout << n.key << std::endl;       // undefined behaviour: reads a destroyed object
    return 0;
}
```

**Why C++ cannot catch it.** A C++ reference carries no lifetime. The type of `n` is `Node&`, whatever it points at, and the compiler never asks whether the thing it points at is still
there. Moving `root` into `sink` is an ordinary call. Nothing connects the two.

**What CeeLing does.** It records that `n` *observes* `root`, and that handing `root` to `sink` ends `root`'s ownership. Using `n` afterwards is an error:

<!-- example: 01_dangling_after_move.clg -->
```cpp
class Node {
public:
    int key;
    Node(int k) { key = k; }
};

void sink(Node n) { }

int main() {
    Node root = new Node(1);
    ref Node n = root;      // n only looks at the object; root still owns it
    sink(root);             // root hands the object over: it is gone for root, and for n
    std::cout << n.key << std::endl;
    return 0;
}
```
<!-- error: 01_dangling_after_move.clg -->
```
error[semantic/use-after-move]: use of dangling observer `n` (its owner `root` is gone)
  --> 01_dangling_after_move.clg:13:18
  help: `n` observes `root`, which no longer has a live owner
```

The mechanism (provenance and death events) is [chapter 6](06-dangling-references.md) and [chapter 14](14-how-it-works.md). Nothing was annotated: there are no lifetimes to write. The
only extra word is `ref`, which says "I look, I do not own".

### 2. A reference into a container that moved

<!-- example: 01_invalidated_reference.cpp -->
```cpp
#include <iostream>
#include <vector>

struct Node {
    int v;
    explicit Node(int x) : v(x) {}
};

int main() {
    std::vector<Node> v;
    v.push_back(Node(1));
    Node& n = v[0];
    v.push_back(Node(2));                  // reallocates: n now points at freed memory
    std::cout << n.v << std::endl;         // undefined behaviour
    return 0;
}
```

**Why C++ cannot catch it.** Whether `push_back` reallocates depends on a run-time capacity. The standard documents "invalidates references" in prose; the type system has no way to say
it. This is the most common dangling bug in real C++ and Java does not have it at all.

**What CeeLing does.** Its table of library facts says `push_back` invalidates every reference into the vector, so those observers die at that call.

<!-- example: 01_invalidated_reference.clg -->
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
    v.push_back(new Node(2));     // may move the elements to new memory
    std::cout << n.v << std::endl;
    return 0;
}
```
<!-- error: 01_invalidated_reference.clg -->
```
error[semantic/use-after-move]: use of dangling observer `n` (its owner `v` is gone)
  --> 01_invalidated_reference.clg:12:18
  help: `n` observes `v`, which no longer has a live owner
```

### 3. Using an object after it was moved

<!-- example: 01_use_after_move.cpp -->
```cpp
#include <iostream>
#include <memory>

struct Node {
    int v;
    explicit Node(int x) : v(x) {}
};

void sink(std::unique_ptr<Node>) {}

int main() {
    auto a = std::make_unique<Node>(1);
    sink(std::move(a));                    // a is now empty
    std::cout << a->v << std::endl;        // null dereference; for other types: a valid but unspecified value
    return 0;
}
```

**Why C++ cannot catch it.** `std::move` is only a cast. The moved-from object still exists, still has a name, and the language promises nothing about its state beyond "can be
destroyed". Linters guess; the compiler does not track it.

**What CeeLing does.** Passing an object to a function or assigning it hands ownership over, and that is tracked: the source is dead until you give it a new value. Because CeeLing
inserts the moves itself, tracking them is not optional, it is what makes inserting them safe.

<!-- example: 01_use_after_move.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

void sink(Node n) { }

int main() {
    Node a = new Node(1);
    sink(a);                 // passing an object hands it over
    std::cout << a.v << std::endl;
    return 0;
}
```
<!-- error: 01_use_after_move.clg -->
```
error[semantic/use-after-move]: use of moved-from value `a`
  --> 01_use_after_move.clg:12:18
  help: `a` was moved earlier; assign a new value before using it
```

### 4. Indexing past the end

<!-- example: 01_out_of_bounds.cpp -->
```cpp
#include <iostream>
#include <vector>

int main() {
    std::vector<int> v;
    v.push_back(1);
    v.push_back(2);
    int i = 5;
    std::cout << v[i] << std::endl;        // reads memory past the vector: undefined behaviour, usually no crash
    return 0;
}
```

**Why C++ cannot catch it.** `operator[]` is unchecked on purpose, for speed, and `i` is only known at run time.

**What CeeLing does.** Every `v[i]` on a vector or string is compiled to a checked access. A bad index stops the program and says where:

<!-- example: 01_out_of_bounds.clg -->
```cpp
int main() {
    std::vector<int> v;
    v.push_back(1);
    v.push_back(2);
    int i = 5;
    std::cout << v[i] << std::endl;
    return 0;
}
```
<!-- output: 01_out_of_bounds.clg -->
```
cling: index out of bounds at line 6 (index 5, size 2)
```

It is `abort()`, not an exception, so no `catch` can swallow it. The cost and the switch that turns it off are in [chapter 7](07-run-time-checks.md).

### 5. Dereferencing an empty pointer

<!-- example: 01_null_deref.cpp -->
```cpp
#include <iostream>
#include <memory>

struct Node {
    std::unique_ptr<Node> next;
    int v;
    explicit Node(int x) : v(x) {}
};

int main() {
    auto n = std::make_unique<Node>(1);
    std::cout << n->v << std::endl;
    std::cout << n->next->v << std::endl;  // next is empty: undefined behaviour
    return 0;
}
```

**Why C++ cannot catch it.** An empty `unique_ptr` and a full one have the same type. `->` on an empty one is undefined, not an error.

**What CeeLing does.** Every member access on an object goes through a null check that names the line. (Proving "never null" statically would reject most real programs; checking
costs a branch.)

<!-- example: 01_null_deref.clg -->
```cpp
class Node {
public:
    Node next;
    int v;
    Node(int x) { v = x; }
};

int main() {
    Node n = new Node(1);
    std::cout << n.v << std::endl;
    std::cout << n.next.v << std::endl;     // the last node has no next
    return 0;
}
```
<!-- output: 01_null_deref.clg -->
```
1
```

### 6. The back door from a smart pointer to a raw one

<!-- example: 01_back_door.cpp -->
```cpp
#include <iostream>
#include <memory>

struct Node {
    int v;
    explicit Node(int x) : v(x) {}
};

int main() {
    Node* raw = nullptr;
    {
        auto p = std::make_unique<Node>(1);
        raw = p.get();                     // a raw pointer: the smart pointer no longer knows about it
    }
    std::cout << raw->v << std::endl;      // undefined behaviour: the object died with p
    return 0;
}
```

**Why C++ cannot close it.** `.get()`, `T*`, casts and `new`/`delete` are part of the language and of every library interface. A safe subset has to be a *different* language, because the
unsafe operations are the ordinary ones.

**What CeeLing does.** They do not exist. Raw pointers, `.get()`, C-style casts, `reinterpret_cast`, `const_cast`, raw `union` and `delete` are all errors, so one `.get()` cannot quietly void
everything else:

<!-- example: 01_back_door.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

int main() {
    Node p = new Node(1);
    int r = p.get();         // in C++ this is how a raw pointer escapes from a smart pointer
    return 0;
}
```
<!-- error: 01_back_door.clg -->
```
error[semantic/forbidden-construct]: '.get()' is forbidden on a CeeLing object ('p' is a Node)
  --> 01_back_door.clg:9:13
  help: '.get()' would extract a raw pointer, which has no legal use in CeeLing (CLAUDE.md hatch #14) -- use the object directly, or add a named accessor method to the class if you need a specific field.
```

Raw pointer types are rejected as well, and the full list is [chapter 8](08-sealed-escape-hatches.md):

<!-- example: 01_raw_pointer.clg -->
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
<!-- error: 01_raw_pointer.clg -->
```
error[semantic/forbidden-construct]: raw pointer types (`T*`) are forbidden (found `Node*`)
  --> 01_raw_pointer.clg:9:5
  help: pointers have no legal path through CeeLing (CLAUDE.md hatch #15); declare a plain object variable and let the compiler choose the smart-pointer type.
```

### 7. A closure that captures a frame that is gone

<!-- example: 01_lambda_by_reference.cpp -->
```cpp
#include <functional>
#include <iostream>

std::function<int(int)> adder(int base) {
    return [&](int x) { return base + x; };    // captures base by reference: it lives in adder's frame
}

int main() {
    auto add1000 = adder(1000);
    std::cout << add1000(1) << std::endl;      // undefined behaviour: base is gone
    return 0;
}
```

**Why C++ cannot catch it.** `[&]` is a perfectly good capture; whether the lambda outlives the variable is a question about the whole program. (This one compiles with no warning.)

**What CeeLing does.** It does not try to analyse it. Capture is **by value only**, and the capture list is a list of names: there is no `&` to write. A closure therefore owns what it
captured and can go anywhere.

<!-- example: 01_closure_by_value.clg -->
```cpp
class Callback {
public:
    Callback() { }
    virtual void call(int x) { }
};

Callback adder(int base) {
    return closure Callback [base] (int x) {     // base is copied into the closure
        std::cout << (base + x);
    };
}

int main() {
    Callback add1000 = adder(1000);              // adder has returned; its local base is long gone
    add1000.call(1);
    std::cout << " ";
    add1000.call(2);
    std::cout << std::endl;
    return 0;
}
```
<!-- output: 01_closure_by_value.clg -->
```
1001 1002
```

More in [chapter 9](09-closures.md).

### 8. Two threads, one object

<!-- example: 01_data_race.cpp -->
```cpp
#include <memory>
#include <thread>

struct Counter {
    int n = 0;
};

int main() {
    auto c = std::make_shared<Counter>();
    std::thread t([c] { c->n = c->n + 1; });   // the copy of the shared_ptr is safe, the object it points to is not
    c->n = c->n + 1;                           // data race: two threads, one object, no lock
    t.join();
    return 0;
}
```

**Why C++ cannot catch it.** Copying a `shared_ptr` is thread-safe; touching what it points to is not, and the type does not say which of its owners are on which thread.

**What CeeLing does.** A thread is started with `spawn`, and only *values* cross into it: numbers, strings, vectors of those, plain structs, or an object that is **moved** in so the
creating thread can no longer touch it. Here `c` is moved into the job, so the use after the `spawn` is the same error as in section 3:

<!-- example: 01_data_race.clg -->
```cpp
class Counter {
public:
    int n;
    Counter() { n = 0; }
};

class Job {
public:
    Job() { }
    virtual void run() { }
};

int main() {
    Counter c = new Counter();
    spawn(closure Job [c] () {
        c.n = c.n + 1;        // in the new thread ...
    });
    c.n = c.n + 1;            // ... and here, at the same time
    return 0;
}
```
<!-- error: 01_data_race.clg -->
```
error[semantic/use-after-move]: use of moved-from value `c`
  --> 01_data_race.clg:18:11
  help: `c` was moved earlier; assign a new value before using it
error[semantic/use-after-move]: use of moved-from value `c`
  --> 01_data_race.clg:18:5
  help: `c` was moved earlier; assign a new value before using it
```

Threads that need to talk use a `Channel<T>`, the one piece of state a spawned job shares with its creator: a mutex-guarded queue of plain values. The handle is copied into the job and
both handles name the same queue, so the creator keeps using its own:

<!-- example: 01_channel_sum.clg -->
```cpp
class Job {
public:
    Job() { }
    virtual void run() { }
};

int main() {
    Channel<int> numbers;
    spawn(closure Job [numbers] () {         // the Channel handle is copied in; both handles name one queue
        int i = 0;
        while (i < 100) {
            numbers.send(i);                 // a copy of an int crosses, nothing else is shared
            i = i + 1;
        }
        numbers.close();
    });
    int total = 0;
    int received = 0;
    while (numbers.wait(5000)) {
        total = total + numbers.take();
        received = received + 1;
    }
    std::cout << total << " " << received << std::endl;
    return 0;
}
```
<!-- output: 01_channel_sum.clg -->
```
4950 100
```

This is a narrow claim and it is stated exactly in [chapter 10](10-threads.md): in checked code, threads share only `Channel` state, so there are no data races through `spawn` or
`Channel`. It is **not** "thread-safe".

### 9. Choosing the wrong smart pointer

<!-- example: 01_cycle_leak.cpp -->
```cpp
#include <iostream>
#include <memory>

struct Node {
    int v;
    std::shared_ptr<Node> next;
    std::shared_ptr<Node> prev;            // should have been weak_ptr; nothing says so
    explicit Node(int x) : v(x) {}
};

int main() {
    auto a = std::make_shared<Node>(1);
    a->next = std::make_shared<Node>(2);
    a->next->prev = a;                     // a and its next now keep each other alive forever
    std::cout << a->next->v << std::endl;
    return 0;                              // neither node is ever destroyed: a leak
}
```

**Why C++ cannot catch it.** `unique_ptr`, `shared_ptr` and `weak_ptr` are the programmer's decision at every declaration. The compiler checks that the types fit together, not that the
choice was right.

**What CeeLing does.** You write `Node next;` and `Node prev;`. The compiler looks at how the objects are used, gives single ownership `unique_ptr`, shared ownership `shared_ptr`, and
a link that closes a cycle `weak_ptr`:

<!-- example: 01_cycle.clg -->
```cpp
class Node {
public:
    int v;
    Node next;       // forward link: owns the next node
    Node prev;       // back link: the compiler sees the cycle and does not let it own
    Node(int x) { v = x; }
};

int main() {
    Node a = new Node(1);
    a.next = new Node(2);
    a.next.prev = a;
    std::cout << a.next.v << std::endl;
    return 0;
}
```
<!-- generated: 01_cycle.clg "class Node {" -->
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

This is [chapter 4](04-ownership.md).

## What you write

The ordinary case has none of the above in it. This is a whole program:

<!-- example: 01_hello_objects.clg -->
```cpp
class Account {
public:
    int balance;
    Account(int start) { balance = start; }
    void deposit(int amount) { balance = balance + amount; }
};

int main() {
    Account account = new Account(100);    // no pointer type, no delete, no std::move
    account.deposit(50);
    std::cout << account.balance << std::endl;
    return 0;
}
```

and this is what `main` becomes:

<!-- generated: 01_hello_objects.clg "int main()" -->
```cpp
int main() {
    std::unique_ptr<Account> account = std::make_unique<Account>(100);
    cling::nn(account, "line 10")->deposit(50);
    std::cout << cling::nn(account, "line 11")->balance << std::endl;
    return 0;
}
```

`cling::nn` is the null check of section 5. You never write it, and you can read the output: it is meant to be code you could have written, and it is plain C++ that needs no
CeeLing to build ([chapter 11](11-talking-to-cpp.md)).

## What it costs, honestly

- **It rejects some correct programs.** The analysis is conservative: where it cannot prove an observer is safe (for example, an observer bound to the result of a call, whose owner
  it cannot see) it says no and asks you to write it another way. The language is smaller than C++ on purpose, so that the analysis can be complete. Rejected code is *outside the
  language*, not a bug.
- **The run-time checks are not free.** Bounds checks cost a few percent on a table-heavy program (measured: 5.5 % on decompression, 11 % on compression in a zlib port); null checks
  cost a branch per access. Both can be switched off per build (`--no-bounds-checks`, `--no-null-checks`), and then a claim about the program is a claim about how it was built.
- **The edge of the guarantee is real.** It is trusted at `ext_fn` declarations (a wrong signature is believed), in the libraries you call, and in the generated code's own C++
  compiler. Chapter 13 lists the open gaps one by one.
- **There is no borrow annotation.** That is deliberate: lifetimes in signatures would need every library redesigned around them, and the point is to keep the C++ ecosystem. The price is
  that the checker has fewer facts than Rust's and refuses more.

## Evidence

The claims are tested on a real library: all of cJSON, ported to CeeLing, against the 33 vulnerabilities published for it in 2026. Of the 13 memory-safety issues, 9 cannot happen or are
caught at run time, 3 are partly open and 1 (CPU exhaustion) is not prevented; of the other 20, 17 are logic errors that stay logic errors and 3 are undefined behaviour or shared state (the language removes two of them). Details, and the equivalence tests that make the
port credible, are in [chapter 15](15-case-study-cjson.md).

## Where to go next

| If you want to... | read |
|---|---|
| see what the language has | [2. The language](02-language.md), [3. Values, text and containers](03-values-and-containers.md) |
| understand each guarantee, with the C++ it replaces | chapters [4](04-ownership.md) to [10](10-threads.md) |
| call C++ or split a program into files | [11](11-talking-to-cpp.md), [12](12-many-files.md) |
| judge how far to trust it | [13](13-contract-and-threats.md), [14](14-how-it-works.md), [15](15-case-study-cjson.md), [16](16-case-study-app-cling.md), [17](17-comparisons.md), [18](18-status.md) |

Every example in this guide lives under [`examples/`](examples/) and is checked by `python3 docs/language-guide/tools/guide.py check`; the C++ twins sit in the `cpp/` folders beside them.
