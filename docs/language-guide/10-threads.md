# 10. Threads: only values cross

Threads are **partly** supported, and the part that is supported is checked. This is what works:

| Feature | What it gives you |
|---|---|
| `spawn(closure Job [...] () {...})` | starts a job on a worker thread; only plain values (copied) and, under five conditions, one moved object may cross into it |
| `Channel<T>` | a queue of plain values that two threads share, guarded by a mutex |
| the worker pool | jobs run on a capped, lazily grown pool, first in first out; `cling::setWorkerLimit(n)` |
| failure handlers | a job that throws is contained; `spawn(job, closure Failure ...)` and `onJobFailure(...)` answer for it |
| the exit rule | jobs finish before the program ends; `cling::setExitGraceMs(ms)` abandons a runaway one |

What is checked is exactly what *crosses into* a new thread. What stays unchecked (deadlock and waiting forever, `ext_fn` and shim code, files, the number of threads) is the last section. The result
is never called "thread-safe". A spawned job is a closure, so [chapter 9](09-closures.md) comes first; "moved" is [chapter 5](05-moves.md).

## The problem in C++

Starting a thread is one line. Handing it data safely is not, because C++ lets a thread capture anything.

### Two threads, one object

<!-- example: 10_shared_counter.cpp -->
```cpp
#include <iostream>
#include <memory>
#include <thread>

struct Counter {
    int n = 0;
};

int main() {
    auto c = std::make_shared<Counter>();
    std::thread t([c] {                        // the lambda holds its own copy of the shared_ptr: that copy is safe
        for (int i = 0; i < 100000; i++) c->n = c->n + 1;
    });
    for (int i = 0; i < 100000; i++) c->n = c->n + 1;   // both threads write the one Counter: a data race
    t.join();
    std::cout << c->n << std::endl;            // not 200000 in general; the standard gives no answer at all
    return 0;
}
```

**Why C++ does not stop it.** Copying a `shared_ptr` is thread-safe: the reference count is atomic. The object it points to has no such guarantee, and the type does not say which owner is on which thread.
The lambda captures "by value", which looks careful, yet the copy is a second handle to the *same* `Counter`. This is a data race, so the behaviour is undefined. Compiled with `-O2` and run three times
it printed `200000` each time: a race that gives the right answer is still a race. ThreadSanitizer finds races on the paths that run; the compiler does not.

### A thread that outlives its frame, and a shared container

<!-- example: 10_detached_reference.cpp -->
```cpp
#include <thread>

void start() {
    int hits = 0;
    std::thread t([&] { hits = hits + 1; });   // captures the local by reference
    t.detach();                                // start() returns, hits is gone, the thread may still write to it
}

int main() {
    start();
    return 0;
}
```

<!-- example: 10_vector_push_back.cpp -->
```cpp
#include <iostream>
#include <thread>
#include <vector>

int main() {
    std::vector<int> v;
    std::thread t([&v] {
        for (int i = 0; i < 1000; i++) v.push_back(i);
    });
    for (int i = 0; i < 1000; i++) v.push_back(i);   // two threads reallocating one vector: heap corruption is possible
    t.join();
    std::cout << v.size() << std::endl;
    return 0;
}
```

**Why C++ does not stop it.** `[&]` and `detach()` are both legal, and whether the thread outlives `hits` is a question about the whole program (the dangling reference of [chapter 6](06-dangling-references.md),
with a thread added). Two reallocating `push_back`s can corrupt the heap, and nothing in `std::vector`'s type says "not for two threads". `g++ -Wall -Wextra` is silent on all three programs.

C++ has the right tools (`std::mutex`, `std::atomic`, ThreadSanitizer). None of them is a compile-time check that *your* code uses them where it must. CeeLing's answer is narrower, and it can be
checked: there is no `[&]` to write, no file-scope variable and no raw pointer, so a thread can reach a value only by being given it, and the one place a value is given, the capture list, is checked before any C++ is written.

## `spawn`: a job is a closure, and only values cross

A thread is started with `spawn` and a closure *literal* whose interface is any class with the single virtual method `void run()` (here `Job`).

<!-- example: 10_spawn_values.clg -->
```cpp
class Job {
public:
    Job() { }
    virtual void run() { }
};

int main() {
    int n = 7;
    string word = "hello";
    std::vector<int> data;
    data.push_back(1);
    data.push_back(2);
    data.push_back(3);
    spawn(closure Job [n, word, data] () {
        int total = 0;
        for (int x : data) {
            total = total + x;
        }
        std::cout << "job " << n << " " << word << " " << total << std::endl;
    });
    n = 8;                     // the job has its own copy of n
    data.push_back(100);       // and of the vector
    return 0;
}
```
<!-- output: 10_spawn_values.clg -->
```
job 7 hello 6
```

`n`, `word` and `data` are **copied** into the job. The creating thread changed its own `n` and `data` afterwards and the job did not see it. `main` returned while the job might still run; the
program waits for it (see "Leaving the program").

| Captured | What happens | Why that is safe |
|---|---|---|
| primitive, `string`, `std::vector` of those | **copied** | the job has its own copy |
| a `struct` of those (its base struct and every field are checked in turn) | **copied** | a struct is stored bare, with no handle, so it cannot hide a subclass |
| a `Channel<T>` | the handle is **copied**, both name one queue | the queue is the one shared thing, and it has a lock |
| a class object, under five conditions | **moved** | the creating thread can no longer name it |
| anything else | **compile error** | |

<!-- example: 10_spawn_struct.clg -->
```cpp
class Job {
public:
    Job() { }
    virtual void run() { }
};

struct Inner {
public:
    int a;
    Inner(int x) { a = x; }
};

struct Packet {
public:
    Inner head;
    std::vector<Inner> items;
    Packet(int x) {
        head = new Inner(x);
        items.push_back(new Inner(2));
        items.push_back(new Inner(7));
    }
};

int main() {
    Packet p = new Packet(3);
    spawn(closure Job [p] () {
        std::cout << "job " << p.head.a << " " << p.items[0].a << " " << p.items[1].a << std::endl;
    });
    p.head.a = 99;             // changes the creator's copy only
    return 0;
}
```
<!-- output: 10_spawn_struct.clg -->
```
job 3 2 7
```

### A class object can be moved in, if nothing else can reach it

An object may have other owners, may be a subclass declared in another file, and may hold handles to more objects. CeeLing hands one to a thread only if all five hold:

1. **The class is flat and closed:** every field a plain value, defined in this file, no base class, inherited by nobody, no virtual method, never hands out `this`.
2. **Its dynamic type is exactly the class:** the variable is a local of the capturing function, created *only* by `new T(...)`, not from a call, a parameter, a field or another variable.
3. **It has one owner:** the pointer inference ([chapter 4](04-ownership.md)) said `unique`.
4. **The capture moves it:** later use is use-after-move, and so is a `spawn` in a loop of an object declared outside the loop.
5. **The job holds it uniquely:** the one class where "a parameter stored in a field makes both shared" is switched off.

<!-- example: 10_spawn_object_moved.clg -->
```cpp
class Buffer {
public:
    std::vector<int> data;
    int n;
    Buffer() { n = 0; }
    void add(int x) {
        data.push_back(x);
        n = n + 1;
    }
    int sum() {
        int total = 0;
        for (int x : data) {
            total = total + x;
        }
        return total;
    }
};

class Job { public: Job() { } virtual void run() { } };

int main() {
    Buffer b = new Buffer();
    b.add(1);
    b.add(2);
    b.add(3);
    spawn(closure Job [b] () {
        std::cout << "sum " << b.sum() << " n " << b.n << std::endl;
    });
    return 0;
}
```
<!-- output: 10_spawn_object_moved.clg -->
```
sum 6 n 3
```
<!-- generated: 10_spawn_object_moved.clg "int main()" -->
```cpp
int main() {
    std::unique_ptr<Buffer> b = std::make_unique<Buffer>();
    cling::nn(b, "line 23")->add(1);
    cling::nn(b, "line 24")->add(2);
    cling::nn(b, "line 25")->add(3);
    cling::spawn(std::make_shared<__closure_10_spawn_object_moved_0>(std::move(b)));
    return 0;
}
```

`b` is moved into the job: no copy of the data, no shared pointer, and `b` no longer exists for the creating thread.

### What is rejected

The messages are the compiler's own and say what to do instead (almost always: make the data a `struct`, or a `string` rebuilt in the thread).

**Use after the spawn, and a spawn in a loop.** After a `spawn` the object belongs to the new thread, so this is the ordinary use-after-move error.

<!-- example: 10_reject_after_spawn.clg -->
```cpp
class Job { public: Job() { } virtual void run() { } };

class Buffer {
public:
    int n;
    Buffer() { n = 0; }
};

int main() {
    Buffer b = new Buffer();
    spawn(closure Job [b] () { b.n = 1; });
    b.n = 2;                            // the object now belongs to the new thread

    Buffer c = new Buffer();
    int i = 0;
    while (i < 2) {
        spawn(closure Job [c] () { c.n = 1; });   // the second round would move a moved-from handle
        i = i + 1;
    }
    return 0;
}
```
<!-- error: 10_reject_after_spawn.clg -->
```
error[semantic/use-after-move]: use of moved-from value `b`
  --> 10_reject_after_spawn.clg:12:5
  help: `b` was moved earlier; assign a new value before using it
error[semantic/use-after-move]: use of moved-from value `c`
  help: `c` was moved earlier; assign a new value before using it
```

**Two owners.** `mine` and `holder.held` name one `Counter`, so it is shared; moving it would leave the other alias live here. This holds whatever pointer kind was inferred: a
captured `shared_ptr` is exactly the loophole a naive `spawn` would leave open. (A `std::vector` of objects is refused for the same reason, since it carries handles that may be shared:
`tests/safety/spawn_captures_object_vector_rejected.clg`.)

<!-- example: 10_reject_shared_object.clg -->
```cpp
class Job { public: Job() { } virtual void run() { } };

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

int main() {
    Counter mine = new Counter();
    Holder holder = new Holder(mine);      // two owners of one Counter
    spawn(closure Job [mine] () {
        mine.n = mine.n + 1;
    });
    return 0;
}
```
<!-- error: 10_reject_shared_object.clg -->
```
error[semantic/spawn-captures-object]: `spawn` cannot move `mine` into a new thread: `mine` has more than one owner (it is stored elsewhere or shared), so moving it would leave a live alias in this thread
  --> 10_reject_shared_object.clg:18:5
  help: an object may be moved into a thread only if nothing else can reach it and nothing about it is hidden: make the data a `struct` (copied), or serialize it to a string and rebuild it inside the thread
```

**The class is not flat or not closed.** A class-typed field, a base class, a subclass, a virtual method, a `this` handed out: in each case a handle may hide a different object than its type says. One file, five errors:

<!-- example: 10_reject_class_shapes.clg -->
```cpp
class Job { public: Job() { } virtual void run() { } };

class Inner { public: int a; Inner() { a = 0; } };
class Outer {
public:
    Inner inner;                       // a class-typed field
    Outer() { inner = new Inner(); }
};

class Base { public: int a; Base() { a = 0; } };
class Derived : public Base {          // a base class; and Base is inherited
public:
    int b;
    Derived() { b = 0; }
};

class Shape {                          // a virtual method
public:
    int a;
    Shape() { a = 0; }
    virtual int area() { return a; }
};

class Node {
public:
    int n;
    Node() { n = 0; }
    Node self() { return this; }       // hands out this
};

int main() {
    Outer o = new Outer();
    spawn(closure Job [o] () { std::cout << o.inner.a << std::endl; });
    Derived d = new Derived();
    spawn(closure Job [d] () { std::cout << d.b << std::endl; });
    Base x = new Base();
    spawn(closure Job [x] () { std::cout << x.a << std::endl; });
    Shape s = new Shape();
    spawn(closure Job [s] () { std::cout << s.area() << std::endl; });
    Node a = new Node();
    spawn(closure Job [a] () { a.n = 1; });
    return 0;
}
```
<!-- error: 10_reject_class_shapes.clg -->
```
error[semantic/spawn-captures-object]: `spawn` cannot move `o` into a new thread: field `inner` of class `Outer` is not a plain value (type `Inner`); only primitives, string, vectors of those and plain structs are allowed
  --> 10_reject_class_shapes.clg:33:5
  help: an object may be moved into a thread only if nothing else can reach it and nothing about it is hidden: make the data a `struct` (copied), or serialize it to a string and rebuild it inside the thread
error[semantic/spawn-captures-object]: `spawn` cannot move `d` into a new thread: class `Derived` has a base class
  --> 10_reject_class_shapes.clg:35:5
  help: an object may be moved into a thread only if nothing else can reach it and nothing about it is hidden: make the data a `struct` (copied), or serialize it to a string and rebuild it inside the thread
error[semantic/spawn-captures-object]: `spawn` cannot move `x` into a new thread: class `Base` is inherited by another class in this program
  --> 10_reject_class_shapes.clg:37:5
  help: an object may be moved into a thread only if nothing else can reach it and nothing about it is hidden: make the data a `struct` (copied), or serialize it to a string and rebuild it inside the thread
error[semantic/spawn-captures-object]: `spawn` cannot move `s` into a new thread: class `Shape` has a virtual method (a subclass could be hiding behind it)
  --> 10_reject_class_shapes.clg:39:5
  help: an object may be moved into a thread only if nothing else can reach it and nothing about it is hidden: make the data a `struct` (copied), or serialize it to a string and rebuild it inside the thread
error[semantic/spawn-captures-object]: `spawn` cannot move `a` into a new thread: class `Node` hands out `this`, so its objects are shared
  --> 10_reject_class_shapes.clg:41:5
  help: an object may be moved into a thread only if nothing else can reach it and nothing about it is hidden: make the data a `struct` (copied), or serialize it to a string and rebuild it inside the thread
```

**The dynamic type is not provable.** A function result, a parameter and a field can each hold a subclass, or an object kept elsewhere. Only `new T(...)` in the same function proves what it is. The example is a function
result; a parameter and a field are refused the same way (`spawn_object_parameter_rejected.clg`, `spawn_object_field_capture_rejected.clg` in `tests/safety`).

<!-- example: 10_reject_from_call.clg -->
```cpp
class Job { public: Job() { } virtual void run() { } };

class Buffer {
public:
    int n;
    Buffer() { n = 0; }
};

Buffer make() {
    Buffer b = new Buffer();
    return b;
}

int main() {
    Buffer b = make();                  // a function result: which object is this?
    spawn(closure Job [b] () { b.n = 1; });
    return 0;
}
```
<!-- error: 10_reject_from_call.clg -->
```
error[semantic/spawn-captures-object]: `spawn` cannot move `b` into a new thread: `b` is not created only by `new Buffer(...)` in this function, so it might hold a subclass object or an object also reachable from elsewhere
  --> 10_reject_from_call.clg:16:5
  help: an object may be moved into a thread only if nothing else can reach it and nothing about it is hidden: make the data a `struct` (copied), or serialize it to a string and rebuild it inside the thread
```

**Forms that would skip the check.** Only a closure literal is accepted, so the capture list is the one place to check; a hand-written call of the runtime helper `cling::spawn` is refused for the same reason; the interface
method must be `void run()` (a job has no caller to return to; `spawn_interface_not_run_rejected.clg`); a `spawn` inside a spawned body is refused (no unbounded multiplication of threads); and a body may not name an enclosing local it did not capture.

<!-- example: 10_reject_named_job.clg -->
```cpp
class Job { public: Job() { } virtual void run() { } };

int main() {
    Job j = new Job();
    spawn(j);
    return 0;
}
```
<!-- error: 10_reject_named_job.clg -->
```
Error: error[semantic/spawn-form]: `spawn` takes a closure literal, and optionally a second one as failure handler (line 5, column 5). write `spawn(closure Job [values] () { ... })` or `spawn(closure Job [values] () { ... }, closure Failure [values] (string message) { ... })` -- a named object cannot be spawned, because only the capture lists are checked for what crosses into the new thread
```

<!-- example: 10_reject_nested.clg -->
```cpp
class Job { public: Job() { } virtual void run() { } };

int main() {
    int n = 1;
    spawn(closure Job [n] () {
        int m = n;
        spawn(closure Job [m] () {
            std::cout << m << std::endl;
        });
    });
    return 0;
}
```
<!-- error: 10_reject_nested.clg -->
```
Error: error[semantic/spawn-nested]: `spawn` inside a `spawn` body (line 7, column 9). a spawned job may not start more threads (v1) -- start both from the creating thread
```

<!-- example: 10_reject_uncaptured.clg -->
```cpp
class Job { public: Job() { } virtual void run() { } };

int main() {
    int hidden = 5;
    int n = 1;
    spawn(closure Job [n] () {
        std::cout << hidden << std::endl;
    });
    return 0;
}
```
<!-- error: 10_reject_uncaptured.clg -->
```
Error: error[semantic/spawn-uncaptured-name]: the `spawn` body uses `hidden`, which belongs to the enclosing scope but is not in the capture list (line 6, column 11). a spawned job sees only what it captures (copies of plain values); add `hidden` to the capture list if it is a plain value, or pass what you need another way
```

The last way for two threads to meet in C++ is a variable at file scope. CeeLing has none:

<!-- example: 10_reject_global.clg -->
```cpp
int hits = 0;                  // in C++ every thread can reach this

int main() {
    hits = hits + 1;
    return 0;
}
```
<!-- error: 10_reject_global.clg -->
```
error[semantic/forbidden-construct]: statement at file scope
  --> 10_reject_global.clg:1:1
  help: File-scope variables are not supported (global mutable state has no owner and no lifetime the checker can reason about). Put the value in a function, or pass it as a parameter or a class field.
```

**Where it stops.** The rule is conservative. A tree of objects (cJSON's `Item`, with `weak` parent links) can never be sent: serialize it to a `string` and rebuild it in the thread, as app-cling does with its JSON
arguments. Data that travels into a thread is a `struct` or a `string`, and the program is arranged around that.

## `Channel<T>`: the one thing threads share

A job takes values in, but it also needs to hand something back, or ask the creating thread to do what only it may do. C++ writes a mutex and a queue by hand at every use. CeeLing has one built-in queue the
compiler understands. `Channel<int> c;` creates it. A handle is a value: assigning, passing, returning and capturing it **copy the handle**, and every copy names the same queue. The methods are an exact list; any
other name is an "unknown call" error and argument counts are checked.

| Call | Meaning |
|---|---|
| `c.send(v)` | copies `v` in; `false` if the channel is closed or already holds 1024 values. Never blocks, so memory cannot grow without bound |
| `c.wait(ms)` | `true` once a value can be taken; `0` polls, `-1` waits as long as it takes; `false` on timeout or when closed and empty |
| `c.take()` | removes and returns one value; throws `"channel empty"` if none |
| `c.size()`, `c.close()`, `c.isClosed()` | count; refuse further sends (queued values can still be taken); state |

<!-- example: 10_channel_producer.clg -->
```cpp
class Job { public: Job() { } virtual void run() { } };

int main() {
    Channel<int> c;
    spawn(closure Job [c] () {
        int i = 0;
        while (i < 100) {
            c.send(i);
            i = i + 1;
        }
        c.close();
    });
    int total = 0;
    int received = 0;
    while (c.wait(5000)) {
        total = total + c.take();
        received = received + 1;
    }
    std::cout << total << " " << received << " " << c.isClosed() << std::endl;
    return 0;
}
```
<!-- output: 10_channel_producer.clg -->
```
4950 100 1
```

A job sends a hundred numbers and the creating thread sums them. The creator keeps its own handle after the `spawn`, which a captured *object* could never allow. What crosses is a copy of each `int`.
The runtime type, `cling::Channel<T>`, is a template in the generated file: a handle to mutex-guarded state with no move operations, so `std::move(handle)` copies and a creator's handle is never emptied.

**What `T` may be.** What a `spawn` may capture: primitives, `string`, `std::vector` of those, a `struct` of those. A struct may hold a `Channel`, so a request can carry the channel its answer returns on:

<!-- example: 10_channel_request_reply.clg -->
```cpp
class Job { public: Job() { } virtual void run() { } };

struct Request {
public:
    string path;
    Channel<string> reply;
    Request(string p, Channel<string> r) { path = p; reply = r; }
};

int main() {
    Channel<Request> requests;
    Channel<string> result;
    spawn(closure Job [requests, result] () {
        string collected = "";
        int i = 0;
        while (i < 2) {
            Channel<string> reply;
            requests.send(new Request("/etc/x", reply));
            if (reply.wait(5000)) {
                collected = reply.take();
            }
            i = i + 1;
        }
        result.send(collected);
        result.close();
    });
    int served = 0;
    while (served < 2 && requests.wait(5000)) {
        Request r = requests.take();
        r.reply.send("FILE:" + r.path);
        served = served + 1;
    }
    string text = "";
    if (result.wait(5000)) {
        text = result.take();
    }
    std::cout << served << " " << text << std::endl;
    return 0;
}
```
<!-- output: 10_channel_request_reply.clg -->
```
2 FILE:/etc/x
```

This is why `Channel` exists: the worker needs something only the creating thread may do (in app-cling, reading a file through a policy object that lives on the main thread), so it sends a request that contains its
own reply channel. A send copies, so the sender keeps its value; the edges are deliberate:

<!-- example: 10_channel_copies.clg -->
```cpp
int main() {
    std::vector<int> v;
    v.push_back(1);
    Channel<std::vector<int>> vc;
    vc.send(v);                      // a copy goes in: v is still ours
    v.push_back(2);
    std::vector<int> got = vc.take();
    std::cout << v.size() << " " << got.size() << std::endl;

    Channel<string> s;
    Channel<string> alias = s;       // a second handle to the SAME queue
    alias.send("hello");
    std::cout << s.take() << std::endl;
    return 0;
}
```
<!-- output: 10_channel_copies.clg -->
```
2 1
hello
```

<!-- example: 10_channel_edges.clg -->
```cpp
int main() {
    Channel<int> c;
    try {
        int x = c.take();
    } catch (string message) {
        std::cout << "take on empty: " << message << std::endl;
    }
    int accepted = 0;
    int i = 0;
    while (i < 1100) {
        if (c.send(i)) {
            accepted = accepted + 1;
        }
        i = i + 1;
    }
    std::cout << "accepted " << accepted << std::endl;
    c.close();
    std::cout << "send after close: " << c.send(1) << std::endl;
    int drained = 0;
    while (c.wait(0)) {
        int y = c.take();
        drained = drained + 1;
    }
    std::cout << "drained " << drained << " more: " << c.wait(0) << std::endl;
    return 0;
}
```
<!-- output: 10_channel_edges.clg -->
```
take on empty: channel empty
accepted 1024
send after close: 0
drained 1024 more: 0
```

Everything else is a compile error: a channel of objects (shown), a channel of channels, a channel inside a `std::vector`, `ref Channel` (a channel is already a handle: copy it), an unknown method (shown), and a
channel in an `ext_fn` signature (it would cross the trusted boundary of [chapter 11](11-talking-to-cpp.md) unobserved; shown). An object is still refused when a channel is captured beside it. The unshown ones are
`tests/safety/channel_nested_channel_rejected.clg`, `channel_in_vector_rejected.clg`, `channel_ref_observer_rejected.clg` and `channel_spawn_captures_class_rejected.clg`.

<!-- example: 10_channel_class_rejected.clg -->
```cpp
class Thing {
public:
    int n;
    Thing(int k) { n = k; }
};

int main() {
    Channel<Thing> c;
    return 0;
}
```
<!-- error: 10_channel_class_rejected.clg -->
```
Error: error[semantic/forbidden-construct]: `Channel<Thing>` would carry `Thing`, which is not a plain value (line 8, column 5). a Channel carries what a `spawn` may capture: primitives, string, std::vector of those, and `struct`s made only of those. They are copied through the queue, so no object is shared between threads. Send a `struct`, or serialize the object to a string first
```

<!-- example: 10_channel_ext_fn_rejected.clg -->
```cpp
ext_fn int sendAll(Channel<int> c);

int main() {
    return 0;
}
```
<!-- error: 10_channel_ext_fn_rejected.clg -->
```
Error: error[semantic/forbidden-construct]: ext_fn parameter `c` type `Channel<int>` is not supported (line 1, column 1). ext_fn signatures are restricted to primitive types (int, float, double, bool, string, char) and std::vector<primitive> for now -- a class-typed ext_fn parameter or return needs a hand-written borrow-summary annotation that isn't designed yet (see the internal notes' ext_fn entry).
```

<!-- example: 10_channel_unknown_method_rejected.clg -->
```cpp
int main() {
    Channel<int> c;
    c.send(1);
    int x = c.peek();
    return 0;
}
```
<!-- error: 10_channel_unknown_method_rejected.clg -->
```
error[semantic/forbidden-construct]: call to unknown function/method `peek`
  --> 10_channel_unknown_method_rejected.clg:4:13
  help: CeeLing has no borrow/ownership summary for anything it did not compile itself; unknown calls are rejected by default rather than silently trusted (the default-reject rule). Define `peek` in CeeLing source, or call an existing CeeLing function/method instead.
```

**Where it stops.** The checker proves that threads share only channel state and that a channel carries copies. It cannot see that a reply is never sent, that nobody closes a channel, or that every consumer waits on a producer that will never run.

## The worker pool

`spawn` does not create a thread per call. Jobs go into a first-in, first-out queue and run on a **capped pool**, created lazily: a worker starts only while jobs wait and the limit is not reached. The spawner
never blocks. The limit is the larger of 4 and the number of cores; `cling::setWorkerLimit(n)` or `-DCLING_MAX_WORKERS=n` (when compiling the generated C++) changes it.

<!-- example: 10_pool_limit_one.clg -->
```cpp
class Job { public: Job() { } virtual void run() { } };

int main() {
    cling::setWorkerLimit(1);
    int one = 1;
    int two = 2;
    int three = 3;
    spawn(closure Job [one] () {
        std::cout << one;
    });
    spawn(closure Job [two] () {
        std::cout << two;
    });
    spawn(closure Job [three] () {
        std::cout << three;
    });
    return 0;
}
```
<!-- output: 10_pool_limit_one.clg -->
```
123
```

With one worker and a FIFO queue, the jobs print in spawn order. With the default pool the digits could come out in any order.

The cap has a consequence the compiler cannot see. A job that waits holds one of the slots, and the job that would satisfy it may be queued behind. Here the first job waits for a value only the second can send,
and the second cannot start until the first ends:

<!-- example: 10_pool_starvation.clg -->
```cpp
class Job { public: Job() { } virtual void run() { } };

int main() {
    cling::setWorkerLimit(1);              // one job at a time
    Channel<int> c;
    spawn(closure Job [c] () {             // runs first and holds the only slot
        if (c.wait(200)) {
            std::cout << "waiting job: got " << c.take() << std::endl;
        } else {
            std::cout << "waiting job: timed out" << std::endl;
        }
    });
    spawn(closure Job [c] () {             // queued behind it: cannot start until the first job ends
        c.send(1);
        std::cout << "producer ran" << std::endl;
    });
    return 0;
}
```
<!-- output: 10_pool_starvation.clg -->
```
waiting job: timed out
producer ran
```

It ends only because the wait has a timeout: give every `wait` a timeout.

## When a job throws

A job that throws never reaches `std::terminate`. With no handler the message goes to stderr and the job ends; the program carries on.

<!-- example: 10_failure_unhandled.clg -->
```cpp
class Job { public: Job() { } virtual void run() { } };

int main() {
    int n = 1;
    spawn(closure Job [n] () {
        throw "job failed";
    });
    std::cout << "main done" << std::endl;
    return 0;
}
```
<!-- output: 10_failure_unhandled.clg -->
```
main done
```

(`cling: unhandled error in spawned job: job failed` is on stderr; the block shows stdout.) A failure handler is a second closure whose interface is a class with the single virtual method `void fail(string message)`.
It runs on the same worker with what was thrown, and follows the job's capture rule (plain values only), so it cannot smuggle an object in:

<!-- example: 10_failure_handler.clg -->
```cpp
class Job { public: Job() { } virtual void run() { } };

class Failure {
public:
    Failure() { }
    virtual void fail(string message) { }
};

int main() {
    int id = 7;
    spawn(closure Job [id] () {
        throw "boom";
    }, closure Failure [id] (string message) {
        std::cout << "failed: " << message << " " << id << std::endl;
    });
    return 0;
}
```
<!-- output: 10_failure_handler.clg -->
```
failed: boom 7
```

<!-- example: 10_failure_object_rejected.clg -->
```cpp
class Job { public: Job() { } virtual void run() { } };

class Failure {
public:
    Failure() { }
    virtual void fail(string message) { }
};

class Counter {
public:
    int n;
    Counter() { n = 0; }
};

int main() {
    Counter c = new Counter();
    int id = 1;
    spawn(closure Job [id] () {
        throw "boom";
    }, closure Failure [c] (string message) {
        c.n = 1;
    });
    return 0;
}
```
<!-- error: 10_failure_object_rejected.clg -->
```
Error: error[semantic/spawn-captures-object]: `spawn` captures `c` of type `Counter`, which is not a plain value (line 20, column 8). only plain values may cross into a new thread: primitives, string, std::vector of those, and `struct`s made only of those. They are copied, so no memory is shared with the creating thread. A class object (whatever pointer kind was inferred for it) may have another live owner here and may be a subclass declared elsewhere, so it is refused -- make the data a `struct`, or serialize it to a string first (e.g. printUnformatted(item)) and rebuild it inside the thread
```

`onJobFailure(closure Failure [...] (string message) {...})` registers a default for the spawns that follow **from the calling thread**; `cling::clearJobFailure()` removes it. A framework can arrange the reply
once per request:

<!-- example: 10_failure_default.clg -->
```cpp
class Job { public: Job() { } virtual void run() { } };

class Failure {
public:
    Failure() { }
    virtual void fail(string message) { }
};

int main() {
    int request = 5;
    onJobFailure(closure Failure [request] (string message) {
        std::cout << "handler saw: " << message << " (req " << request << ")" << std::endl;
    });
    spawn(closure Job [request] () {
        throw "first failed";
    });
    cling::clearJobFailure();
    spawn(closure Job [request] () {
        throw "second failed";       // no handler now: the message goes to stderr
    });
    return 0;
}
```
<!-- output: 10_failure_default.clg -->
```
handler saw: first failed (req 5)
```

The second job's message went to stderr. The default belongs to the *thread*: clear it when a request is over, or the next request handled there inherits it. A handler that throws is reported on stderr, not rethrown.
`onJobFailure` takes a closure literal for the same reason `spawn` does, and cannot be called from inside a job.

## Leaving the program

When `main` returns, the pool is drained and joined: every queued and running job runs to the end first (every example above that prints from a job relies on it: `main` has already returned).

The price is that a job that never ends keeps the process from ever exiting. `cling::setExitGraceMs(ms)` bounds the wait; then the process ends where it stands, and stderr says how many jobs were abandoned.

<!-- example: 10_exit_runaway.clg -->
```cpp
class Job { public: Job() { } virtual void run() { } };

int main() {
    cling::setExitGraceMs(300);
    int never = 0;
    spawn(closure Job [never] () {
        while (true) {
            std::cout << "";
        }
    });
    std::cout << "main done" << std::endl;
    return 0;
}
```
<!-- output: 10_exit_runaway.clg -->
```
main done
```

An abandoned job is not unwound: no destructors run, nothing is flushed on its behalf, and the exit status is still 0. The grace period is opt-in because the safe default is to lose no work.

## The claim, exactly

For `spawn`:

> **In CeeLing code that was checked, a spawned job can reach only its own copies, objects moved in (which the creating thread can no longer touch) and its own allocations, so there are no data races through `spawn`.**

For `Channel`:

> **In CeeLing code that was checked, threads share only Channel state, which is guarded by a mutex and carries copies of plain values, so there are no data races through `spawn` or `Channel`.**

That is all. It is not "thread-safe", and these are **not checked**:

- **Deadlock and waiting forever.** CeeLing has no locks, yet a thread can wait for a message that never comes. This compiles:

<!-- example: 10_wait_forever.clg -->
```cpp
class Job { public: Job() { } virtual void run() { } };

int main() {
    Channel<int> c;
    int n = 1;
    spawn(closure Job [n] () {
        std::cout << n << std::endl;     // this job never sends anything
    });
    bool got = c.wait(-1);                // wait as long as it takes: forever, here
    return 0;
}
```

  `wait(-1)` on a thread that must stay responsive is a bug the compiler does not see; a waiting job also holds a pool slot (above), and a job that never ends blocks exit unless `setExitGraceMs` is set.
- **Who closes a channel**, and whether a reply is ever sent. A failed job with no handler leaves whoever waited for it waiting.
- **`ext_fn` and shim code.** A worker may call a trusted function that touches shared state ([chapter 11](11-talking-to-cpp.md)); a `Channel` cannot be passed to one.
- **Anything outside CeeLing's memory:** files, sockets, windows. Two jobs writing one file race as in any language.
- **The number of threads** beyond the pool's cap, and the order in which queued jobs run.
- **Hidden state in a trusted type.** Objects are checked by their fields' types; a library type whose internals share state is not looked into.
- **Correctness.** "No data race" is not "right": app-cling had a file watch that answered before the operating system was watching. Not a memory-safety bug, so the language could not catch it; the test suite did.

The claim is not larger because proving that no two threads touch the same memory with a write needs exclusivity tracking, a read/write distinction and `Send`/`Sync`-style propagation through every type: the
lifetime and mutability machinery CeeLing gave up to keep C++ libraries and to write no annotations ([chapter 17](17-comparisons.md) compares with Rust). So the model is narrow instead: threads share nothing
but a channel. Whoever needs the strongest guarantee, or race freedom for arbitrary shared state, should use Rust.

## What backs it

- **The compiler:** `tests/safety/spawn_*` and `channel_*` programs; accepted ones are compiled *and run*, so "the job really got a copy" is asserted, not read off the source.
- **app-cling,** a desktop-application framework written in CeeLing: its `src/app.clg` has 7 `spawn(closure Job ...)` expressions (the `proc.run` command, the dialogs, the close confirmation, the notification) and a
  `Channel` carries file requests from workers to the main thread. Its `tests/run_sanitizers.sh` runs the end-to-end tests under AddressSanitizer, UndefinedBehaviorSanitizer and ThreadSanitizer, plus *negative
  controls*: the same run against a build with a lock removed must report races, or the clean run would prove nothing. (Not re-run for this guide.)
- **cjson-cling** has `tests/run_threads.sh`: eight threads each parse, print and drop their own documents through the pooled port.

Next: [chapter 11, Talking to C++](11-talking-to-cpp.md), the other trusted boundary.
