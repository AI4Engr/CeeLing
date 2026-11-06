# 9. Closures

A closure is a small object made on the spot: some captured values and one function body. In C++ it is a lambda, and the lambda is where the language lets a function outlive the
variables it uses. CeeLing has closures too, with one deliberate restriction: **they capture by value, always**, so a closure can be returned, stored and called later without anything
under it disappearing.

## The problem in C++

### A lambda that refers to a frame that is gone

<!-- example: 09_lambda_ref_field.cpp -->
```cpp
// Run it: the output is garbage or a crash. The lambda keeps a reference to `clicks`, which died when setup() returned.
#include <functional>
#include <iostream>

struct Button {
    std::function<void()> onClick;
};

void setup(Button& b) {
    int clicks = 0;
    b.onClick = [&clicks] { clicks = clicks + 1; std::cout << clicks << std::endl; };
}

int main() {
    Button b;
    setup(b);
    b.onClick();
    return 0;
}
```

**Why C++ does not stop it.** `[&clicks]` is an ordinary capture, and `std::function<void()>` is an ordinary type. Neither says how long `clicks` lives; the type of `onClick` is the same
whether the lambda refers to a local, a global or nothing. Whether the lambda outlives its variable is a question about the whole program, and the compiler does not ask it. (g++ with
`-Wall -Wextra` is silent here. Some compilers warn about the most obvious direct cases; none of those warnings is a guarantee.)

### A lambda that refers to `this`

<!-- example: 09_lambda_this.cpp -->
```cpp
// Run it: undefined behaviour. The lambda captured `this`, and the Widget is gone when the lambda runs.
#include <functional>
#include <iostream>
#include <memory>

struct Widget {
    int id = 7;
    std::function<void()> callback() {
        return [this] { std::cout << id << std::endl; };
    }
};

int main() {
    std::function<void()> cb;
    {
        auto w = std::make_unique<Widget>();
        cb = w->callback();
    }
    cb();
    return 0;
}
```

**Why C++ does not stop it.** `[this]` copies a raw pointer. Nothing ties the lambda's life to the object's. It is the same problem as before, with an object in place of a local.

### `std::function` hides who owns what

`std::function` erases the lambda's type, so the signature of a function taking one does not say whether it will keep the callback, call it once, or hand it to another thread, and it does
not say what the callback holds. The cure many C++ programmers choose, capturing a `shared_ptr` by value, brings the next problem: a lambda stored inside the object it captured keeps that
object alive for ever.

<!-- example: 09_function_cycle.cpp -->
```cpp
// Run it: prints 1 and the Button is never destroyed (run it under a leak checker). The callback owns the button, the button owns the callback.
#include <functional>
#include <iostream>
#include <memory>

struct Button {
    int clicks = 0;
    std::function<void()> onClick;
    ~Button() { std::cout << "destroyed" << std::endl; }
};

int main() {
    auto b = std::make_shared<Button>();
    b->onClick = [b] { b->clicks = b->clicks + 1; };   // captures a copy of the shared_ptr: a cycle
    b->onClick();
    std::cout << b->clicks << std::endl;
    return 0;                                          // "destroyed" is never printed
}
```

**Why C++ does not stop it.** `shared_ptr` versus `weak_ptr` is the programmer's choice at every capture, and the compiler only checks that the types fit.

Sections 1 and 3 of [chapter 1](01-why-ceeling.md) show the same family of bug for plain references and for moves.

## What CeeLing does

### The syntax: an interface, a capture list, parameters, a body

```
closure Interface [captures] (parameters) { body }
```

`Interface` is an ordinary class that has **exactly one virtual method**. That method gives the closure its signature: the closure's parameter list must be the same, and its body is the
method's body. The captures are names of locals or parameters that are in scope at that point; each one is copied into the closure. A closure is an expression: pass it to a function,
return it, store it, call it through the interface.

<!-- example: 09_closure_basic.clg -->
```cpp
class Callback {
public:
    Callback() { }
    virtual void call(int x) { }
};

void each(Callback cb, int n) {
    cb.call(n);
}

int main() {
    int total = 100;
    each(closure Callback [total] (int index) {
        std::cout << (total + index) << std::endl;
    }, 3);
    return 0;
}
```
<!-- output: 09_closure_basic.clg -->
```
103
```

`each` takes a `Callback` and neither knows nor cares that the object it received was made from a closure. There is no `std::function`: the interface is a class you wrote, so its name says
what the callback is for, and its one method is called like any other (`cb.call(n)`).

A closure may return a value (the interface's method says so), and its capture list may be empty:

<!-- example: 09_closure_returns.clg -->
```cpp
class Transform {
public:
    Transform() { }
    virtual int apply(int x) { return x; }
};

int main() {
    int factor = 3;
    Transform times = closure Transform [factor] (int x) {
        return x * factor;
    };
    Transform twice = closure Transform [] (int x) {      // no captures at all
        return x * 2;
    };
    std::cout << times.apply(7) << " " << twice.apply(21) << std::endl;
    return 0;
}
```
<!-- output: 09_closure_returns.clg -->
```
21 42
```

Several captures are separated by commas, in any mix of types:

<!-- example: 09_closure_two_captures.clg -->
```cpp
class Callback {
public:
    Callback() { }
    virtual void call(int x) { }
};

void each(Callback cb, int n) {
    cb.call(n);
}

int main() {
    int multiplier = 2;
    string label = "result";
    each(closure Callback [multiplier, label] (int x) {
        std::cout << label << (multiplier * x) << std::endl;
    }, 5);
    return 0;
}
```
<!-- output: 09_closure_two_captures.clg -->
```
result10
```

**Where it stops.** The interface is a class declared in the same program; a closure cannot be made for a library type (`std::function`, a callback type from another C++ library). Two
rejections guard the interface rule. A class with no virtual method, or with more than one, is not an interface, because there would be no single method for the body to be:

<!-- example: 09_interface_shape_rejected.clg -->
```cpp
class NotAnInterface {
public:
    NotAnInterface() { }
    void ordinaryMethod(int x) { }
};

void each(NotAnInterface cb) {
}

int main() {
    int total = 100;
    each(closure NotAnInterface [total] (int x) {
        std::cout << x << std::endl;
    });
    return 0;
}
```
<!-- error: 09_interface_shape_rejected.clg -->
```
Error: error[semantic/closure-interface-shape]: interface `NotAnInterface` has 0 virtual method(s), not exactly one (line 12, column 10). a closure's interface must declare EXACTLY ONE virtual method, so there is a single unambiguous method for the closure body to override
```

and a name that is not a class of this program is not one either:

<!-- example: 09_unknown_interface_rejected.clg -->
```cpp
class Callback {
public:
    Callback() { }
    virtual void call(int x) { }
};

void each(Callback cb) {
    cb.call(1);
}

int main() {
    int total = 100;
    each(closure NoSuchInterface [total] (int x) {
        std::cout << x << std::endl;
    });
    return 0;
}
```
<!-- error: 09_unknown_interface_rejected.clg -->
```
Error: error[semantic/closure-unknown-interface]: closure names interface `NoSuchInterface`, which is not a class declared in this program (line 13, column 10). a closure's interface must be an ordinary CeeLing class with exactly one virtual method -- CeeLing cannot desugar a closure against an interface it did not compile itself
```

The parameter list has to match the interface's method in count and types. Otherwise the generated method would be an overload instead of an override, and the closure would never run:

<!-- example: 09_signature_rejected.clg -->
```cpp
class Callback {
public:
    Callback() { }
    virtual void call(int x) { }
};

void each(Callback cb) {
    cb.call(1);
}

int main() {
    int total = 100;
    each(closure Callback [total] (string wrongType) {
        std::cout << wrongType << std::endl;
    });
    return 0;
}
```
<!-- error: 09_signature_rejected.clg -->
```
Error: error[semantic/closure-signature-mismatch]: closure parameter 1 has type `string`, interface `Callback::call` declares `int` (line 13, column 10). a closure's parameter types must match its interface method's exactly
```

### Capture is by value, and `&` cannot even be written

In C++ `[&]` and `[=]` are two modes of one capture clause, and which one a lambda uses decides whether it can safely escape. CeeLing has no modes: the capture list is a list of bare names.
There is no `&` and no `=` in the grammar, so the dangerous form is not a case the compiler detects and refuses; it cannot be written down.

<!-- example: 09_ampersand_rejected.clg -->
```cpp
class Callback {
public:
    Callback() { }
    virtual void call(int x) { }
};

void each(Callback cb) {
    cb.call(1);
}

int main() {
    int total = 100;
    each(closure Callback [&total] (int x) {
        total = total + x;
    });
    return 0;
}
```
<!-- error: 09_ampersand_rejected.clg -->
```
Error: Syntax error at line 11, column 12, node type 'ERROR', near: {
    int total = 100;
    each(closure Callback [&total] (int x)
```

The message is the parser's and does not mention `&`: that is what "not in the language" looks like. The same holds for `this`. It cannot be captured, because it is a borrow of the
enclosing object and a closure would have to hold it past the object's life:

<!-- example: 09_this_capture_rejected.clg -->
```cpp
class Callback {
public:
    Callback() { }
    virtual void call(int x) { }
};

void each(Callback cb) {
    cb.call(1);
}

class Widget {
public:
    int id;
    Widget(int i) { id = i; }
    void doIt() {
        each(closure Callback [this] (int x) {
            std::cout << x << std::endl;
        });
    }
};

int main() {
    return 0;
}
```
<!-- error: 09_this_capture_rejected.clg -->
```
Error: error[semantic/closure-unknown-capture]: closure captures `this`, which is not in scope here (line 16, column 14). every name in a closure's capture list must be a local variable or parameter already declared before the closure expression -- if this closure is NESTED inside another closure's body, note that it can only capture its immediately enclosing closure's own PARAMETERS, never that closure's own CAPTURES (which are fields reachable only through the enclosing closure's `this`, and closures cannot capture `this`)
```

The cost is real: **a closure can never change a local of the function that made it.** A captured name is the closure's own copy, and the next example shows that the copy changes while the
original stays put.

<!-- example: 09_closure_state.clg -->
```cpp
class Step {
public:
    Step() { }
    virtual int next() { return 0; }
};

Step counter(int start) {
    return closure Step [start] () {
        start = start + 1;          // changes the closure's own copy
        return start;
    };
}

int main() {
    int n = 10;
    Step s = counter(n);            // counter() has returned: its frame is gone
    std::cout << s.next() << " " << s.next() << " " << n << std::endl;
    return 0;
}
```
<!-- output: 09_closure_state.clg -->
```
11 12 10
```

`counter` has returned by the time `s.next()` runs, so its parameter `start` is gone; the closure works anyway because the value it uses is its own. The `10` at the end is the caller's
`n`, which was never touched. It also means a closure made in a loop gets a snapshot of the loop variable, not a reference to the one variable that ends at its last value:

<!-- example: 09_closure_vector.clg -->
```cpp
class Callback {
public:
    Callback() { }
    virtual void call(int x) { }
};

int main() {
    std::vector<Callback> cbs;
    int i = 0;
    while (i < 3) {
        cbs.push_back(closure Callback [i] (int x) {
            std::cout << (i * 10 + x);
        });
        i = i + 1;
    }
    int k = 0;
    for (Callback c : cbs) {
        if (k > 0) {
            std::cout << " ";
        }
        c.call(1);
        k = k + 1;
    }
    std::cout << std::endl;
    return 0;
}
```
<!-- output: 09_closure_vector.clg -->
```
1 11 21
```

A capture name must exist where the closure is written. A name that does not is an error, not a guess:

<!-- example: 09_unknown_capture_rejected.clg -->
```cpp
class Callback {
public:
    Callback() { }
    virtual void call(int x) { }
};

void each(Callback cb) {
    cb.call(1);
}

int main() {
    each(closure Callback [doesNotExist] (int x) {
        std::cout << x << std::endl;
    });
    return 0;
}
```
<!-- error: 09_unknown_capture_rejected.clg -->
```
Error: error[semantic/closure-unknown-capture]: closure captures `doesNotExist`, which is not in scope here (line 12, column 10). every name in a closure's capture list must be a local variable or parameter already declared before the closure expression -- if this closure is NESTED inside another closure's body, note that it can only capture its immediately enclosing closure's own PARAMETERS, never that closure's own CAPTURES (which are fields reachable only through the enclosing closure's `this`, and closures cannot capture `this`)
```

### Closures that escape

Returned from a function (`counter` above), stored in a field, passed to a constructor: all are fine, because there is nothing a closure points at that can die before it does.

<!-- example: 09_closure_field.clg -->
```cpp
class Callback {
public:
    Callback() { }
    virtual void call(int x) { }
};

class Holder {
public:
    Callback stored;
    int callCount;
    Holder(Callback c) {
        stored = c;
        callCount = 0;
    }
    void run(int x) {
        if (callCount > 0) {
            std::cout << " ";
        }
        callCount = callCount + 1;
        stored.call(x);
    }
};

int main() {
    int base = 1000;
    Holder h = new Holder(closure Callback [base] (int x) {
        std::cout << (base + x);
    });
    h.run(1);
    h.run(2);
    std::cout << std::endl;
    return 0;
}
```
<!-- output: 09_closure_field.clg -->
```
1001 1002
```

The closure in `main` is made, given to `Holder`'s constructor and stored in its field. It is called twice later; `base` is read from the closure's own copy each time.

**Where it stops.** Two limits found by running the compiler; they are gaps in the implementation, not rules of the language, so there is no example of them here. A closure assigned to an
existing object's field (`h.stored = closure ...;`, outside a constructor) currently produces C++ that g++ rejects (a `unique_ptr` field is assigned a `shared_ptr`); hand the closure to the
constructor, as above. And a capture name that is repeated (`[total, total]`), or an enclosing local used in the body but not listed in the capture list, passes the CeeLing compiler and
fails in g++.

### What capturing an object means

A captured number is copied. A captured **object** is not: objects are handles, and copying a handle makes a second owner of the same object. The closure becomes an owner, so the compiler
gives that object shared ownership (the `shared_ptr` of [chapter 4](04-ownership.md)) and neither side can outlive the other's use of it.

<!-- example: 09_closure_object.clg -->
```cpp
class Counter {
public:
    int n;
    Counter() { n = 0; }
};

class Action {
public:
    Action() { }
    virtual void run() { }
};

int main() {
    Counter c = new Counter();
    Action a = closure Action [c] () {
        c.n = c.n + 1;                 // the closure holds the same Counter as main
    };
    a.run();
    c.n = c.n + 10;
    a.run();
    std::cout << c.n << std::endl;
    return 0;
}
```
<!-- output: 09_closure_object.clg -->
```
12
```

`main` and the closure see the same `Counter`: the closure added 1, `main` added 10, the closure added 1 again. That is the same aliasing a field plus a getter gives (Java has it too), and it
is safe against dangling because the object lives as long as its last owner. It is **not** safe across threads, and `spawn` therefore refuses a closure that captures an object it did not
move in ([chapter 10](10-threads.md)).

**Where it stops.** Capturing is a share, never a copy of the object. If you want an independent object, make one before the closure and capture that.

### A closure cannot capture an observer

`ref` ([chapter 6](06-dangling-references.md)) means "I look, I do not own", and the checker keeps a `ref` from outliving its owner by never letting it leave its block. A closure is a thing
that leaves the block, so copying a `ref` into one would put a non-owning reference in a place that outlives it: the old bug through a new door. It is rejected:

<!-- example: 09_ref_capture_rejected.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

class Callback {
public:
    Callback() { }
    virtual void call(int x) { }
};

void each(Callback cb, int n) {
    cb.call(n);
}

int main() {
    Node owner = new Node(5);
    ref Node observed = owner;
    each(closure Callback [observed] (int x) {
        std::cout << x << std::endl;
    }, 1);
    return 0;
}
```
<!-- error: 09_ref_capture_rejected.clg -->
```
Error: error[semantic/closure-captures-observer]: closure captures `observed`, a `ref` observer (line 20, column 10). a `ref` observer is a non-owning reference into its owner's stack frame; capturing it by value would copy that reference into a FIELD that can outlive the observer's own scope, reintroducing exactly the dangling-reference hole this language exists to prevent -- capture the observer's OWNER instead, or restructure so the closure does not need it
```

The fix is to capture the owner (`owner` here), which makes the closure a co-owner. Likewise a closure's own parameter list obeys the rule every parameter list obeys, no `ref`:

<!-- example: 09_ref_param_rejected.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

class Callback {
public:
    Callback() { }
    virtual void call(int x) { }
};

void each(Callback cb) {
    cb.call(1);
}

int main() {
    int total = 100;
    each(closure Callback [total] (ref Node n) {
        std::cout << total << std::endl;
    });
    return 0;
}
```
<!-- error: 09_ref_param_rejected.clg -->
```
Error: error[semantic/observer_param]: `ref` observers cannot be function parameters (line 19, column 36). Function arguments always transfer ownership (move); pass by value and let the caller keep observers local.
```

### Closures inside closures

A closure body may contain another closure, to any depth. The inner closure may capture the **parameters** of the closure around it, but not that closure's captures:

<!-- example: 09_closure_nested.clg -->
```cpp
class Callback {
public:
    Callback() { }
    virtual void call(int x) { }
};

void each(Callback cb) {
    cb.call(1);
}

int main() {
    int total = 100;
    each(closure Callback [total] (int x) {
        each(closure Callback [x] (int y) {     // x is the outer closure's parameter
            std::cout << (x + y) << std::endl;
        });
    });
    return 0;
}
```
<!-- output: 09_closure_nested.clg -->
```
2
```

An outer capture lives in a field of the outer closure and could only be reached through `this`, and `this` cannot be captured, so the compiler refuses the direct form:

<!-- example: 09_nested_capture_rejected.clg -->
```cpp
class Callback {
public:
    Callback() { }
    virtual void call(int x) { }
};

void each(Callback cb) {
    cb.call(1);
}

int main() {
    int total = 100;
    each(closure Callback [total] (int x) {
        each(closure Callback [total] (int y) {
            std::cout << y << std::endl;
        });
    });
    return 0;
}
```
<!-- error: 09_nested_capture_rejected.clg -->
```
Error: error[semantic/closure-unknown-capture]: closure captures `total`, which is not in scope here (line 14, column 14). every name in a closure's capture list must be a local variable or parameter already declared before the closure expression -- if this closure is NESTED inside another closure's body, note that it can only capture its immediately enclosing closure's own PARAMETERS, never that closure's own CAPTURES (which are fields reachable only through the enclosing closure's `this`, and closures cannot capture `this`)
```

The workaround is to copy the value into a local inside the outer body and capture the local:

<!-- example: 09_closure_nested_local.clg -->
```cpp
class Callback {
public:
    Callback() { }
    virtual void call(int x) { }
};

void each(Callback cb) {
    cb.call(1);
}

int main() {
    int total = 100;
    each(closure Callback [total] (int x) {
        int t = total;                          // copy the outer capture into a local ...
        each(closure Callback [t] (int y) {     // ... and capture the local
            std::cout << (t + y) << std::endl;
        });
    });
    return 0;
}
```
<!-- output: 09_closure_nested_local.clg -->
```
101
```

This is a known limitation of the implementation (it is listed in the internal notes), not a safety rule.

## What a closure becomes

A closure is not a new kind of value in the compiler. Before any checker runs, each closure is rewritten into an ordinary class that derives from the interface, with one field per capture, a
constructor that copies them in, and the body as the overriding method. The closure expression itself becomes `new` of that class. This is `09_closure_object.clg` after the rewrite:

<!-- generated: 09_closure_object.clg "class __closure_09_closure_object_0 :" -->
```cpp
class __closure_09_closure_object_0 : public Action {
public:
    std::shared_ptr<Counter> c;
    __closure_09_closure_object_0(std::shared_ptr<Counter> p_c);
    void run() override;
};
```
<!-- generated: 09_closure_object.clg "int main()" -->
```cpp
int main() {
    std::shared_ptr<Counter> c = std::make_shared<Counter>();
    std::unique_ptr<Action> a = std::make_unique<__closure_09_closure_object_0>(c);
    cling::nn(a, "line 18")->run();
    cling::nn(c, "line 19")->n = (cling::nn(c, "line 19")->n + 10);
    cling::nn(a, "line 20")->run();
    std::cout << cling::nn(c, "line 21")->n << std::endl;
    return 0;
}
```

The class is named `__closure_<file>_<n>`; the field `c` is a `shared_ptr` because the object is shared (previous section), and `main` constructs the class like any other. The analysis of
[chapters 4 to 6](04-ownership.md) then runs on this class: nothing after the rewrite knows closures exist. Two consequences follow.

* **The guarantees are the ones for classes, no more and no fewer.** A captured object is an owner like a field; a closure passed to a function is an object passed to a function, so it is
  moved under the same rules (and the same use-after-move check applies); a closure cannot hold a dangling observer because no `ref` field can exist.
* **The cost is the cost of a class**: a heap object per closure and a virtual call. There is no measurement of it in this repository.

`spawn(closure Job [values] () { ... })` ([chapter 10](10-threads.md)) is this same syntax with an extra check on what may be captured, and is explained there.

## Summary

| Rule | Why | Example |
|---|---|---|
| The interface is a class with exactly one virtual method | the body needs one method to be | `09_interface_shape_rejected` |
| The interface is a class of this program | the closure becomes its subclass | `09_unknown_interface_rejected` |
| Parameters match the method exactly | otherwise the closure would overload, not override | `09_signature_rejected` |
| Capture by value only; no `&`, no `=` | a closure owns what it uses, so it can escape | `09_ampersand_rejected`, `09_closure_state` |
| A capture name must be in scope | no guessing | `09_unknown_capture_rejected` |
| `this` cannot be captured | it is a borrow of the enclosing object | `09_this_capture_rejected` |
| An object capture is a share | the closure becomes a co-owner | `09_closure_object` |
| A `ref` observer cannot be captured | it would outlive its owner's block | `09_ref_capture_rejected` |
| A closure has no `ref` parameter | no parameter list has one | `09_ref_param_rejected` |
| An inner closure captures only the outer closure's parameters | outer captures are reachable only through `this` | `09_nested_capture_rejected` |
