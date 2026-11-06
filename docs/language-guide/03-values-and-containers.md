# 3. Values, text and containers

This chapter is about the things that are not objects: numbers, characters, `string`, `std::vector<T>`, `std::map<K,V>`, and the few library functions a program may call. They are the
types you reach for first, and they are where C++'s unchecked defaults (signed `char`, unchecked `[]`, `c_str()`, a `map` that inserts when you read) do most of their damage. Classes,
structs and inheritance are [chapter 2](02-language.md); who owns an object is [chapter 4](04-ownership.md).

Every example prints with `std::cout << ... << std::endl`, the only output form; numbers print exactly as C++ prints them (`1e+30`, `1.250000` from `std::to_string`).

## Numbers and truth values

CeeLing has `int`, `int64`, `float`, `double` and `bool`. A number literal is digits with an optional fraction and an optional exponent (`0.25`, `1.0e30`, `2.5E-3`, `1e5`); `true` and
`false` are the booleans. The usual operators work (`+ - * / %`, comparisons, `&& || !`, `& | ~ << >>`); there is no `^`.

<!-- example: 03_literals.clg -->
```cpp
int main() {
    int count = 7;
    float ratio = 0.25;
    double big = 1.0e30;
    double tiny = 2.5E-3;
    bool ok = count > 3 && ratio < 1;
    char letter = 'x';
    std::cout << (count / 2) << " " << (count % 2) << " " << ratio << " " << big << " " << tiny << " " << ok << " " << letter << std::endl;
    return 0;
}
```
<!-- output: 03_literals.clg -->
```
3 1 0.25 1e+30 0.0025 1 x
```

There is no `unsigned`, no `short`, no `size_t` in the language, and neither `x++` nor `x += 1` exists: you write `x = x + 1`. The language is small on purpose, so that every operation the
analysis has to reason about is one it knows.

### `int64`, and why it is not called `long`

**The problem in C++.** The width of `long` depends on the platform:

<!-- example: 03_long_width.cpp -->
```cpp
// Run it: the first number is 4 on 64-bit Windows and 8 on 64-bit Linux; the second is the same everywhere.
#include <iostream>

int main() {
    std::cout << sizeof(long) << " " << sizeof(long long) << std::endl;
    return 0;
}
```

**Why C++ does not stop it.** The standard only promises `long` is at least 32 bits. Code that stores a file size or a timestamp in a `long` is correct on one platform and truncates on the
other, and g++ has no reason to warn.

**What CeeLing does.** `int64` is a signed 64-bit integer and lowers to C++ `long long`, which the standard guarantees to be at least 64 bits. It is not called `long` because that name
would suggest the platform-dependent width.

<!-- example: 03_int64.clg -->
```cpp
int main() {
    int64 big = 5000000000;           // does not fit in an int
    int64 doubled = big * 2;
    int64 halved = big >> 1;
    int64 square = int64(65536) * int64(65536);
    std::cout << doubled << " " << halved << " " << square << std::endl;
    return 0;
}
```
<!-- output: 03_int64.clg -->
```
10000000000 2500000000 4294967296
```

**Where it stops.** `int` is 32 bits. CeeLing adds **no** overflow check, no narrowing check and no division-by-zero check: `2147483647 + 1` wraps (it is undefined behaviour in C++, and
the generated code is C++), `int x = 3.7;` truncates, and `5 / 0` crashes the program. These were verified by running them, and the design notes do not list them among the run-time
checks. If you write `long` or `size_t`, the compiler does not know the name and passes it through to C++ unchecked; do not rely on that, use `int` or `int64`.

## `byte` and `char`

**The problem in C++.** Whether `char` is signed is up to the platform, and a byte that came from a file is usually meant to be unsigned:

<!-- example: 03_char_signed.cpp -->
```cpp
// Run it: on this toolchain char is signed, so idx is -56 and the store lands 56 ints before the array.
#include <iostream>
#include <vector>

int main() {
    std::vector<char> data;
    data.push_back(static_cast<char>(200));   // a byte that came from a file or a socket
    int counts[256] = {0};
    int idx = data[0];                        // char is signed here: idx is -56, not 200
    std::cout << idx << std::endl;
    counts[idx] = 1;                          // writes before the start of the array
    std::cout << counts[0] << std::endl;
    return 0;
}
```

**Why C++ does not stop it.** `char` is the type of both "a letter" and "a byte", and its signedness is implementation-defined. The conversion to `int` is legal, so the table index above is
-56 and the store goes before the array. (g++ with `-Wall -Wextra` is silent.)

**What CeeLing does.** It has both, with one meaning each. `char` is a character, written `'x'` (escapes `\n \r \t \b \f \0 \\ \' \" \xHH`). `byte` is an unsigned 8-bit value and lowers
to `unsigned char`: the same value stays 200, and a vector index made from it is in range. There is no `char*` or array type anywhere, so the C-string shape cannot be written.

<!-- example: 03_byte.clg -->
```cpp
int main() {
    std::vector<byte> data;
    data.push_back(200);            // a byte from a file or a socket
    std::vector<int> counts;
    counts.resize(256, 0);
    int idx = data[0];              // byte is unsigned: idx is 200
    counts[idx] = 1;
    byte half = data[0] >> 1;
    std::cout << idx << " " << int(half) << " " << counts[200] << std::endl;
    return 0;
}
```
<!-- output: 03_byte.clg -->
```
200 100 1
```

Indexing the `counts` vector with `idx` is also bounds-checked ([chapter 7](07-run-time-checks.md)); the C++ array was not.

**Where it stops.** Printing a `byte` directly prints the character with that code (C++'s `ostream` does this for `unsigned char`), so the example converts with `int(...)` first.

## Casts between primitives

**The problem in C++.** `(int)x` is one syntax for several different operations. The compiler picks which one from the types, and the reader cannot tell:

<!-- example: 03_cast_c_style.cpp -->
```cpp
#include <iostream>

struct Node {
    int v;
    explicit Node(int x) : v(x) {}
};

int main() {
    double d = 3.7;
    int i = (int)d;                      // a numeric conversion ...
    Node n(1);
    Node* p = &n;
    long long address = (long long)p;    // ... and the same syntax reinterprets a pointer as a number
    Node* q = (Node*)address;            // ... and back
    std::cout << i << " " << q->v << std::endl;
    return 0;
}
```

**Why C++ does not stop it.** A C-style cast is defined to try `static_cast`, then `const_cast`, then `reinterpret_cast`, whichever works. The second use above turns a pointer into a number
and back, and is legal C++.

**What CeeLing does.** There is no C-style cast (`(int)d` is a syntax error, and `static_cast`, `reinterpret_cast` and `const_cast` are rejected as unknown calls: [chapter 8](08-sealed-escape-hatches.md)).
The one spelling is a call to a primitive's name, `int(x)`, `int64(x)`, `double(x)`, `float(x)`, `char(x)`, `byte(x)`, and it accepts a single primitive only:

<!-- example: 03_cast_primitive.clg -->
```cpp
int main() {
    double d = 3.7;
    int i = int(d);                           // truncates toward zero
    int neg = int(-1.9);
    double back = double(i);
    char c = char(65);
    byte b = byte(300);                       // wraps modulo 256, like unsigned char
    int64 wide = int64(i) * int64(1000000000);
    std::cout << i << " " << neg << " " << back << " " << c << " " << int(b) << " " << wide << std::endl;
    return 0;
}
```
<!-- output: 03_cast_primitive.clg -->
```
3 -1 3 A 44 3000000000
```

`int(double)` truncates toward zero; `byte(300)` wraps to 44, as `unsigned char` does. Anything that is not a primitive is refused, so a cast cannot be used to turn a string or an
object into a number:

<!-- example: 03_cast_string.clg -->
```cpp
int main() {
    string s = "12";
    int x = int(s);
    return 0;
}
```
<!-- error: 03_cast_string.clg -->
```
error[semantic/forbidden-construct]: `int(...)` converts one primitive to another; the argument here is not a single primitive
  --> 03_cast_string.clg:3:13
  help: Primitive conversions take exactly one int/float/double/char/byte/int64 (or bool) value. A class, container or string cannot be cast to a primitive -- that would be a type-confusion hatch (guide chapter 13, #13).
```

<!-- example: 03_cast_object.clg -->
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
<!-- error: 03_cast_object.clg -->
```
error[semantic/forbidden-construct]: `int(...)` converts one primitive to another; the argument here is not a single primitive
  --> 03_cast_object.clg:9:13
  help: Primitive conversions take exactly one int/float/double/char/byte/int64 (or bool) value. A class, container or string cannot be cast to a primitive -- that would be a type-confusion hatch (guide chapter 13, #13).
```

For text to number there is `std::stoi` (below). **Where it stops.** There is no `bool(x)`; write a comparison. The primitive casts do not check range: `byte(300)` is 44 on purpose, and
`int(1e20)` is undefined in C++.

## `string`

A `string` is a value: copying it copies the text, and it cannot dangle. Literals are in double quotes; `+` joins; `length()` / `size()`, `empty()`, `substr(pos, len)`, `substr(pos)`,
`find(text)`, `append(text)`, `append(count, char)`, `push_back(char)`, `reserve(n)` and `clear()` are the methods. `s[i]` reads or writes one `char`, a range-for visits the characters, and
`==`, `!=`, `<` compare the contents.

<!-- example: 03_string_basics.clg -->
```cpp
int main() {
    string name = "world";
    string greeting = "hello, " + name;
    std::cout << greeting << " " << greeting.length() << std::endl;
    int where = greeting.find("wor");
    int missing = greeting.find("xyz");
    std::cout << greeting.substr(7, 5) << " " << where << " " << missing << std::endl;
    greeting[0] = 'H';                        // indexing reads and writes a char
    char last = greeting[greeting.length() - 1];
    int os = 0;
    for (char c : greeting) {                 // range-for over the characters
        if (c == 'o') {
            os = os + 1;
        }
    }
    std::cout << greeting << " " << last << " " << os << " " << (name == "world") << " " << (name < "xylophone") << std::endl;
    return 0;
}
```
<!-- output: 03_string_basics.clg -->
```
hello, world 12
world 7 -1
Hello, world d 2 1 1
```

`find` gives the position, and -1 when the text is absent (the `-1` is what C++'s `npos` becomes when stored in an `int`). Building a string with `s = s + c` copies the whole string on every
character; `push_back` and `append` add in place:

<!-- example: 03_string_build.clg -->
```cpp
int main() {
    string s = "ab";
    s.reserve(16);
    s.push_back('c');                // appends in place
    s.push_back(char(100));
    s.append("e");
    s.append(2, 'x');
    std::cout << s << " " << s.length() << std::endl;
    return 0;
}
```
<!-- output: 03_string_build.clg -->
```
abcdexx 7
```

### Indexing is checked

`s[i]` and `v[i]` are checked at run time: a bad index prints the line, the index and the size, and the program stops (it is `abort()`, so no `catch` can swallow it). The mechanism and the
switch that turns it off are in [chapter 7](07-run-time-checks.md); here it is on a string:

<!-- example: 03_string_oob.clg -->
```cpp
int main() {
    string s = "hello";
    int i = 9;
    std::cout << s[i] << std::endl;
    return 0;
}
```
<!-- output: 03_string_oob.clg -->
```
cling: index out of bounds at line 4 (index 9, size 5)
```

**Where it stops.** The check covers `[]`. `substr` with a position past the end is not checked by CeeLing: the C++ library throws `std::out_of_range`, and `catch (string e)` does not catch
it, so the program terminates (this is what the design notes list as not covered):

<!-- example: 03_string_substr.clg -->
```cpp
int main() {
    string s = "abc";
    try {
        std::cout << s.substr(10, 1) << std::endl;
    } catch (string e) {
        std::cout << "caught" << std::endl;
    }
    return 0;
}
```
<!-- output: 03_string_substr.clg -->
```
terminate called after throwing an instance of 'std::out_of_range'
  what():  basic_string::substr: __pos (which is 10) > this->size() (which is 3)
```

### Escapes are a whitelist

A literal is copied into the C++ unchanged, so any escape that means something different to a reader than to C++ would be a silent miscompile. The accepted escapes are
`\n \r \t \b \f \0 \\ \" \xHH` (and `\'` in a `char`); everything else is an error.

**The problem in C++.** `\x` swallows every hex digit that follows:

<!-- example: 03_hex_greedy.cpp -->
```cpp
// Run it: prints 1. C++ reads \x41BC as ONE character (and g++ warns that it is out of range), not as A, B, C.
#include <iostream>
#include <string>

int main() {
    std::string s = "\x41BC";
    std::cout << s.size() << std::endl;
    return 0;
}
```

**Why C++ does not stop it.** It is the language's rule for `\x` (the compiler can only warn that the value is out of range, and here it does).

**What CeeLing does.** `\x` takes exactly two digits, so `"\x41\x42"` is `AB` and `"\x4"` is an error. Octal escapes (`\012`) and `\u` are refused, because what they produce depends on the
execution character set; write the bytes with `\xHH`.

<!-- example: 03_string_escapes.clg -->
```cpp
int main() {
    string quoted = "say \"hi\"";
    string tabbed = "a\tb";
    string hex = "\x41\x42";
    char q = '\"';
    std::cout << quoted << " " << quoted.length() << " " << tabbed.length() << " " << hex << " " << q << std::endl;
    return 0;
}
```
<!-- output: 03_string_escapes.clg -->
```
say "hi" 8 3 AB "
```

<!-- example: 03_escape_hex.clg -->
```cpp
int main() {
    string s = "\x4";
    return 0;
}
```
<!-- error: 03_escape_hex.clg -->
```
Error: Invalid escape sequence in literal: `/x` must be followed by exactly two hex digits at line 2, column 16, node type 'string_literal', near: "/x4". Suggestion: check whether the statement is missing a trailing ';'.
```

<!-- example: 03_escape_octal.clg -->
```cpp
int main() {
    string s = "a\012b";
    return 0;
}
```
<!-- error: 03_escape_octal.clg -->
```
Error: Invalid escape sequence in literal: `/0` followed by another octal digit is an octal escape; write `/xHH` instead at line 2, column 16, node type 'string_literal', near: "a/012b". Suggestion: check whether the statement is missing a trailing ';'.
```

<!-- example: 03_escape_unicode.clg -->
```cpp
int main() {
    string s = "caf\u00e9";
    return 0;
}
```
<!-- error: 03_escape_unicode.clg -->
```
Error: Invalid escape sequence in literal: `/u`/`/U` are not supported (the bytes they produce depend on the execution character set); write the UTF-8 bytes with `/xHH` at line 2, column 16, node type 'string_literal', near: "caf/u00e9". Suggestion: check whether the statement is missing a trailing ';'.
```

### No `c_str()`

**The problem in C++.** `c_str()` hands out a raw pointer into the string, valid only while the string lives:

<!-- example: 03_c_str.cpp -->
```cpp
// Run it: prints garbage. The string inside label() is destroyed when label() returns.
#include <iostream>
#include <string>

const char* label(int n) {
    std::string s = "item " + std::to_string(n);
    return s.c_str();
}

int main() {
    const char* p = label(7);
    std::cout << p << std::endl;
    return 0;
}
```

**Why C++ does not stop it.** The pointer is a plain `const char*`; it carries no lifetime and g++ stays silent.

**What CeeLing does.** The scan of `std::string` finds that `c_str` returns a raw pointer, so the rule refuses it, and the message says so. A raw pointer has nowhere legal to go in CeeLing, which is the same
reason `.get()` is refused ([chapter 8](08-sealed-escape-hatches.md)).

<!-- example: 03_c_str.clg -->
```cpp
int main() {
    string s = "hello";
    s.c_str();
    return 0;
}
```
<!-- error: 03_c_str.clg -->
```
error[semantic/forbidden-construct]: `c_str` is not available on `string`: it returns a raw pointer, and raw pointers have nowhere legal to go
  --> 03_c_str.clg:3:5
  help: CeeLing admits a method of the standard library only when the checker can follow what it hands back; the rule and the scanned facts are in library_summary.cpp and facts/std-*.clfacts.
```

### Passing a string

A `string` parameter is a value. If the body only reads it, the compiler passes it as `const std::string&` so the call does not copy the text; if the body changes it, the parameter stays a
copy and the caller's string is untouched.

<!-- example: 03_string_param.clg -->
```cpp
string shout(string s) {
    s.append("!");
    return s;
}

int countChars(string s) {
    return s.length();
}

int main() {
    string text = "hello";
    string loud = shout(text);
    std::cout << loud << " " << text << " " << countChars(loud) << std::endl;
    return 0;
}
```
<!-- output: 03_string_param.clg -->
```
hello! hello 6
```

<!-- generated: 03_string_param.clg "int countChars(" -->
```cpp
int countChars(const std::string& s) {
    return s.length();
}
```

`shout` appends to its parameter, so it keeps a by-value `std::string`; `text` printed `hello` afterwards. The read-only case is skipped for constructors, `virtual` and `override`
methods and classes in an inheritance hierarchy (their signatures must stay identical), which keep the copy. This inference is described in the internal notes, not in the design notes.

## The library functions a program may call

**The problem in C++.** Every library call is a promise the compiler cannot check. `std::sort(a.begin(), b.end())` compiles and is undefined, `std::max` returns a reference to one of its
arguments (so `const auto& m = std::max(f(), g());` dangles), and the signature of `std::string::c_str` does not say it borrows.

<!-- example: 03_sort_mismatch.cpp -->
```cpp
// Do not run it: the two iterators belong to different vectors, so the range [a.begin(), b.end()) does not exist.
#include <algorithm>
#include <vector>

int main() {
    std::vector<int> a = {3, 1, 2};
    std::vector<int> b = {9, 8, 7};
    std::sort(a.begin(), b.end());
    return 0;
}
```

**Why C++ does not stop it.** A function signature says what types go in and out, not which output borrows from which input or which input is consumed. That fact is in the documentation, in
prose.

**What CeeLing does.** It answers with a rule, not a library: **a call to a function it has no entry for is rejected**. Its table of library facts is small and written by hand
(`src/core/generator/library_summary.cpp`). The free functions a program can call are:

| Name | What it does |
|---|---|
| `std::stoi(string)` | text to `int` |
| `std::stod(string)` | text to `double` |
| `std::to_string(number)` | number to a fresh `string` |
| `cling::readFileToString`, `cling::fileExists`, `cling::readFileToBytes` | files (below) |
| `cling::spawn` and the worker-pool functions | produced by `spawn` ([chapter 10](10-threads.md)) |

(The table also holds one Eigen entry, `Eigen::MatrixXd::Identity`; those belong to [chapter 11](11-talking-to-cpp.md).) The methods of `std::vector` and `string` are **not on a list**. The compiler holds what a scan of the library's own declarations found for every method
(`facts/std-vector.clfacts`, `facts/std-string.clfacts`, generated, from the MSVC STL and libstdc++), and a rule admits a method when everything it hands back is something the checker can follow: nothing
(`void`), a number or `bool`, a fresh `string`, or the receiver itself (`append` returns `*this`). Today that admits, for `std::vector`, `assign capacity clear empty max_size pop_back push_back reserve resize shrink_to_fit size`
(and CeeLing's own `copyWithin`), and for `string`, `append assign capacity clear compare empty find find_first_not_of find_first_of find_last_not_of find_last_of length max_size pop_back push_back reserve resize rfind
shrink_to_fit size substr`. A method that changes the receiver also invalidates every observer of it; that comes from the scan too (a non-const member with no `const` twin), so `clear` and `assign` do
what `push_back` does without anyone having listed them. The two functions that take or return only primitives need no borrow facts, which is why they could go on the list of free functions.

What the rule refuses, and says why in the message: `begin end cbegin ...` and `insert erase` (an iterator comes back, and iterators are not tracked), `front back at emplace_back` (a reference to an element comes back; only
`v[i]` is tracked), `data c_str` (a raw pointer), `swap` (the other operand would be moved out), `get_allocator`, and, for a string, `copy` and `replace` (an overload takes a pointer to writable memory). It is the same default-reject, now with a reason.

<!-- example: 03_std_functions.clg -->
```cpp
int main() {
    int n = std::stoi("42");
    double d = std::stod("2.5");
    string line = "n=" + std::to_string(n + 1) + " half=" + std::to_string(d / 2) + " text=" + std::to_string(n);
    std::cout << line << std::endl;
    return 0;
}
```
<!-- output: 03_std_functions.clg -->
```
n=43 half=1.250000 text=42
```

Anything else is refused. `std::sort` is the usual surprise:

<!-- example: 03_unknown_call.clg -->
```cpp
int main() {
    std::vector<int> v;
    v.push_back(3);
    v.push_back(1);
    std::sort(v.begin(), v.end());
    return 0;
}
```
<!-- error: 03_unknown_call.clg -->
```
error[semantic/forbidden-construct]: call to unknown function/method `std::sort`
  --> 03_unknown_call.clg:5:5
  help: CeeLing has no borrow/ownership summary for anything it did not compile itself; unknown calls are rejected by default rather than silently trusted (the default-reject rule). Define `std::sort` in CeeLing source, or call an existing CeeLing function/method instead.
error[semantic/forbidden-construct]: `begin` is not available on `std::vector<int>`: it returns an iterator, and iterators are not tracked yet, so nothing would stop one from dangling
  --> 03_unknown_call.clg:5:15
  help: CeeLing admits a method of the standard library only when the checker can follow what it hands back; the rule and the scanned facts are in library_summary.cpp and facts/std-*.clfacts.
error[semantic/forbidden-construct]: `end` is not available on `std::vector<int>`: it returns an iterator, and iterators are not tracked yet, so nothing would stop one from dangling
  --> 03_unknown_call.clg:5:26
  help: CeeLing admits a method of the standard library only when the checker can follow what it hands back; the rule and the scanned facts are in library_summary.cpp and facts/std-*.clfacts.
```

The message says what to do: write the function in CeeLing, or declare it as an `ext_fn` ([chapter 11](11-talking-to-cpp.md)), the explicit trust boundary. This is the "default-reject" rule of
[chapter 13](13-contract-and-threats.md): the cost is that much of `<algorithm>` is not available; the gain is that there is no unknown call through which a dangling reference could get past
the checker. (`v.clear()` and `v.empty()` used to be refused here too; they are admitted now. `v.insert(...)`, `v.front()` and `v.begin()` are refused with a reason, see above.) The two `begin()` calls in the
program above are refused as well, which is why the message appears three times: `sort` takes iterators, and CeeLing admits no way to make one.

**Where it stops.** `std::stoi` and `std::stod` report bad text by throwing a C++ exception that `catch (string e)` does not catch (the same exit as `substr`), so a program that parses
untrusted text cannot recover from a bad number today:

<!-- example: 03_stoi_bad.clg -->
```cpp
int main() {
    try {
        int n = std::stoi("abc");
        std::cout << n << std::endl;
    } catch (string e) {
        std::cout << "caught" << std::endl;
    }
    return 0;
}
```
<!-- output: 03_stoi_bad.clg -->
```
terminate called after throwing an instance of 'std::invalid_argument'
  what():  stoi
```

## `std::vector<T>`

A vector is declared without an initial value (an empty container is always safe), grows with `push_back`, shrinks with `pop_back`, has `size()`, is indexed with `v[i]` (checked) and visited with
a range-for.

<!-- example: 03_vector_basics.clg -->
```cpp
int main() {
    std::vector<int> v;
    v.push_back(10);
    v.push_back(20);
    v.push_back(30);
    v[1] = 25;
    int total = 0;
    for (int x : v) {
        total = total + x;
    }
    v.pop_back();
    std::cout << v.size() << " " << v[1] << " " << total << std::endl;
    return 0;
}
```
<!-- output: 03_vector_basics.clg -->
```
2 25 65
```

A vector of objects holds the objects themselves, and the elements are reached with the same `.`:

<!-- example: 03_vector_objects.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

int main() {
    std::vector<Node> nodes;
    nodes.push_back(new Node(1));
    nodes.push_back(new Node(2));
    nodes.push_back(new Node(3));
    std::cout << nodes[0].v;
    int i = 1;
    while (i < nodes.size()) {
        std::cout << " " << nodes[i].v;
        i = i + 1;
    }
    std::cout << std::endl;
    return 0;
}
```
<!-- output: 03_vector_objects.clg -->
```
1 2 3
```

Pushing a plain local object into a vector gives the vector the object, exactly as passing it to a function does ([chapter 5](05-moves.md)), so the local is dead afterwards:

<!-- example: 03_vector_push_moves.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

int main() {
    std::vector<Node> nodes;
    Node a = new Node(1);
    nodes.push_back(a);
    std::cout << a.v << std::endl;
    return 0;
}
```
<!-- error: 03_vector_push_moves.clg -->
```
error[semantic/use-after-move]: use of moved-from value `a`
  --> 03_vector_push_moves.clg:11:18
  help: `a` was moved earlier; assign a new value before using it
```

(If the object is already shared with something else, `push_back` shares it instead and the local stays usable; whether it is shared is decided by the analysis of
[chapter 4](04-ownership.md).) A reference into a vector is invalidated by `push_back`, `pop_back` and `resize`: that is chapter 1 section 2 and [chapter 6](06-dangling-references.md).

**Where it stops.** `pop_back()` on an empty vector, and iterators past `end()`, are not checked (the design notes). Element types seen in the tests are primitives, `string` and objects.

### `resize`, `reserve` and `copyWithin`: primitive elements only

**The problem in C++.** `resize` on a vector of owning pointers makes empty ones:

<!-- example: 03_resize_null.cpp -->
```cpp
// Run it: crashes. resize(3) fills the new slots with empty unique_ptrs, and nothing in the type says so.
#include <iostream>
#include <memory>
#include <vector>

struct Node {
    int v;
    explicit Node(int x) : v(x) {}
};

int main() {
    std::vector<std::unique_ptr<Node>> nodes;
    nodes.resize(3);
    std::cout << nodes[0]->v << std::endl;
    return 0;
}
```

**Why C++ does not stop it.** An empty `unique_ptr` and a full one have the same type, and `resize(n)` is defined to fill with default values.

**What CeeLing does.** `resize(n)` and `resize(n, value)` exist for vectors of primitives (and `string`), where a default or a copied value is a real value. On a vector of objects they are
rejected: `resize(n)` would create null handles, `resize(n, x)` would duplicate an owner. Objects go in through `push_back`, which the ownership analysis understands. `reserve(n)` and
`copyWithin` are restricted the same way (`copyWithin` computes a byte count from the element size, which means nothing for an object).

<!-- example: 03_vector_resize.clg -->
```cpp
int main() {
    std::vector<int> v;
    v.reserve(8);              // capacity only: size() and the elements are unchanged
    v.push_back(1);
    v.push_back(2);
    v.push_back(3);
    v.resize(6, 0);            // grows to 6, new slots hold 0
    v.copyWithin(3, 0, 3);     // copy v[0..3) over v[3..6)
    int i = 0;
    while (i < v.size()) {
        if (i > 0) {
            std::cout << " ";
        }
        std::cout << v[i];
        i = i + 1;
    }
    std::cout << std::endl;
    return 0;
}
```
<!-- output: 03_vector_resize.clg -->
```
1 2 3 1 2 3
```

<!-- example: 03_resize_objects.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

int main() {
    std::vector<Node> nodes;
    nodes.resize(3);
    return 0;
}
```
<!-- error: 03_resize_objects.clg -->
```
error[semantic/forbidden-construct]: `resize` on `std::vector<Node>`: element type is not primitive
  --> 03_resize_objects.clg:9:5
  help: `resize` is only allowed on vectors of primitive elements (int, byte, char, ...). On a vector of objects it would fill the new slots with null smart pointers, or duplicate an owner for resize(n, x). Build the elements with push_back instead.
```

`reserve` leaves `size()` and every element unchanged, but when it grows capacity it moves the buffer, so like `resize` and `push_back` it invalidates references into the vector (the C++
standard says the same, and CeeLing treats it that way: an observer of the vector is dead after a `reserve`).

`v.copyWithin(dst, src, len)` copies `len` elements within one vector, like Rust's `copy_within`, and handles overlapping ranges correctly. In C++ you would pick between `std::copy`,
`std::copy_backward` and `std::memmove` yourself; the first is undefined if the destination starts inside the source:

<!-- example: 03_copy_overlap.cpp -->
```cpp
// std::copy requires that the destination does not start inside the source range: this is undefined behaviour.
// g++ happens to print "1 2 1 2 0 0 0" here, another library or optimisation level may print something else.
#include <algorithm>
#include <iostream>
#include <vector>

int main() {
    std::vector<int> v = {1, 2, 0, 0, 0, 0, 0};
    std::copy(v.begin(), v.begin() + 5, v.begin() + 2);
    for (int x : v) std::cout << x << " ";
    std::cout << std::endl;
    return 0;
}
```

**Where it stops.** `copyWithin` does **not** repeat a pattern. Copying 5 elements from index 0 to index 2 of `[1, 2, ...]` reads the 5 elements as they are and writes them once; it does
not produce `1 2 1 2 1 2 1`, which is what a DEFLATE-style back-reference needs. A caller that needs the repeating copy must write a loop. The example pins the real result:

<!-- example: 03_copy_within_overlap.clg -->
```cpp
int main() {
    std::vector<int> v;
    v.push_back(1);
    v.push_back(2);
    v.resize(7, 0);
    v.copyWithin(2, 0, 5);     // the source and the destination overlap
    int i = 0;
    while (i < v.size()) {
        if (i > 0) {
            std::cout << " ";
        }
        std::cout << v[i];
        i = i + 1;
    }
    std::cout << std::endl;
    return 0;
}
```
<!-- output: 03_copy_within_overlap.clg -->
```
1 2 1 2 0 0 0
```

### `sort`, `reverse`, `fill`, `indexOf`, `contains`: whole-vector algorithms

C++ spells these with iterators (`std::sort(v.begin(), v.end())`), and CeeLing refuses iterators. So a vector of numbers, `bool`, `char`, `byte` or `string` has five methods of CeeLing's own that take the whole vector: `v.sort()` (ascending; NaN last),
`v.reverse()`, `v.fill(x)`, `v.indexOf(x)` (the first position, or -1) and `v.contains(x)`. They take no comparator and no range, work on primitives only (ordering objects needs a comparison CeeLing cannot express), and change neither the size nor the
storage, so a span of the vector survives them.

<!-- example: 03_vector_algorithms.clg -->
```cpp
int main() {
    std::vector<int> v;
    v.push_back(5);
    v.push_back(2);
    v.push_back(9);
    v.push_back(1);
    v.sort();                              // no iterators, no comparator: the whole vector, ascending
    std::cout << v[0] << " " << v[1] << " " << v[2] << " " << v[3] << " | ";
    v.reverse();
    std::cout << v[0] << " " << v[1] << " " << v[2] << " " << v[3] << " | ";
    std::cout << v.indexOf(2) << " " << v.indexOf(7) << " | " << v.contains(9) << " " << v.contains(4) << std::endl;
    return 0;
}
```
x
```

### Passing a vector to a function

**The problem in C++.** A parameter declared `std::vector<int>` copies the whole vector on every call. The fix is `const std::vector<int>&`, and that reintroduces the reference:

<!-- example: 03_vector_alias.cpp -->
```cpp
// Run it: undefined behaviour. The Holder keeps a reference to a vector that was a temporary.
#include <iostream>
#include <vector>

struct Holder {
    const std::vector<int>& data;
    explicit Holder(const std::vector<int>& d) : data(d) {}
};

std::vector<int> make() { return {1, 2, 3}; }

int main() {
    Holder h(make());                      // the temporary dies at the end of this statement
    std::cout << h.data[0] << std::endl;   // reads a destroyed vector
    return 0;
}
```

**Why C++ does not stop it.** `const std::vector<int>&` may bind a temporary, and storing the reference in a member is legal. The signature says nothing about whether the callee keeps it. (g++
is silent on this one.)

<!-- example: 03_vector_param_copy.cpp -->
```cpp
// Run it: prints 0. The parameter is a whole copy of the caller's vector, made on every call.
#include <iostream>
#include <vector>

bool sharesStorage(std::vector<int> param, const std::vector<int>& caller) {
    return param.data() == caller.data();
}

int main() {
    std::vector<int> v = {1, 2, 3};
    std::cout << sharesStorage(v, v) << std::endl;
    return 0;
}
```

**What CeeLing does.** A vector parameter is a value, and passing a vector is handing it over, the same rule as an object. The compiler decides how from the body:

* a parameter the body only **reads** (subscript reads, `size()`, a range-for, or passing it on to another read-only parameter) is passed as `const std::vector<T>&` automatically, so no copy
  and no move happens and the caller keeps its vector;
* a parameter the body **changes or keeps** stays a by-value parameter and the call moves the caller's vector in; using the caller's vector afterwards is a use-after-move error.

<!-- example: 03_vector_param.clg -->
```cpp
int sumAll(std::vector<int> data) {
    int total = 0;
    for (int x : data) {
        total = total + x;
    }
    return total;
}

int main() {
    std::vector<int> v;
    v.push_back(1);
    v.push_back(2);
    v.push_back(3);
    int s = sumAll(v);
    std::cout << s << " " << v.size() << std::endl;    // v is still here
    return 0;
}
```
<!-- output: 03_vector_param.clg -->
```
6 3
```

<!-- generated: 03_vector_param.clg "int sumAll(" -->
```cpp
int sumAll(const std::vector<int>& data) {
    int total = 0;
    for (int x : data) {
        total = (total + x);
    }
    return total;
}
```

(The call site in the generated code still says `std::move(v)`; a `const&` parameter takes nothing from it, which is why `v.size()` prints 3.) The rule's motivating case is
zlib-cling's `decodeSymbol`, which copied four tables of about 512 entries per symbol: passing them by `const&` measured 9.7 times faster decompression (the design notes).

<!-- example: 03_vector_moved.clg -->
```cpp
void grow(std::vector<int> data) {
    data.push_back(99);
}

int main() {
    std::vector<int> v;
    v.push_back(1);
    grow(v);
    std::cout << v[0] << std::endl;
    return 0;
}
```
<!-- error: 03_vector_moved.clg -->
```
error[semantic/use-after-move]: use of moved-from value `v`
  --> 03_vector_moved.clg:9:18
  help: `v` was moved earlier; assign a new value before using it
```

A class that keeps a vector keeps its own copy, so nothing refers to the caller's temporary. There is no way to write a reference member, which closes the C++ example above by
construction ([chapter 6](06-dangling-references.md)):

<!-- example: 03_vector_field.clg -->
```cpp
class Holder {
public:
    std::vector<int> data;
    Holder(std::vector<int> d) { data = d; }
    int first() { return data[0]; }
};

std::vector<int> make() {
    std::vector<int> v;
    v.push_back(1);
    return v;
}

int main() {
    Holder h = new Holder(make());
    std::cout << h.first() << std::endl;
    return 0;
}
```
<!-- output: 03_vector_field.clg -->
```
1
```

**Where it stops.** The by-`const&` inference covers `std::vector` of primitives and `string`; it does not cover `std::map` or vectors of objects. If the argument is a field and the callee might
change the object that owns that field, the compiler passes an explicit copy, which is correct but slower.

## `std::map<K,V>`

**The problem in C++.** Reading `m[k]` inserts `k` if it is absent:

<!-- example: 03_map_read_inserts.cpp -->
```cpp
// Run it: prints "no pears" and then 2. The read of "pear" inserted an entry.
#include <iostream>
#include <map>
#include <string>

int main() {
    std::map<std::string, int> stock;
    stock["apple"] = 3;
    if (stock["pear"] == 0) {
        std::cout << "no pears" << std::endl;
    }
    std::cout << stock.size() << std::endl;
    return 0;
}
```

**Why C++ does not stop it.** `operator[]` is one function for both the read and the write, so it must be able to create the entry, and it is not `const`.

**What CeeLing does.** It keeps the C++ meaning (insert-or-update) and restricts everything else. A map is declared with `std::map<K, V>`, both **primitive** (`int`, `float`, `double`, `bool`,
`char`, `byte`, `int64`, `string`), and is used through subscripts only:

<!-- example: 03_map.clg -->
```cpp
int main() {
    std::map<string, int> stock;
    stock["apple"] = 3;
    stock["apple"] = stock["apple"] + 2;
    std::cout << stock["apple"] << " " << stock["pear"] << std::endl;
    return 0;
}
```
<!-- output: 03_map.clg -->
```
5 0
```

A key or value that is an object is rejected because a map entry would need an ownership decision (one owner or several) that the analysis does not make for maps; primitives have none:

<!-- example: 03_map_object_value.clg -->
```cpp
class Node {
public:
    int v;
    Node(int x) { v = x; }
};

int main() {
    std::map<int, Node> m;
    return 0;
}
```
<!-- error: 03_map_object_value.clg -->
```
Error: error[semantic/forbidden-construct]: `std::map<int, Node>` is not supported (line 8, column 5). CeeLing's std::map support is restricted to primitive key/value types (int, float, double, bool, string) for now -- class-typed map entries need an element-ownership decision (unique_ptr vs shared_ptr) that isn't implemented yet.
```

A map passed to a function is handed over like a vector:

<!-- example: 03_map_moved.clg -->
```cpp
void sink(std::map<int, string> m) {
}

int main() {
    std::map<int, string> m;
    m[1] = "one";
    sink(m);
    m[2] = "two";
    return 0;
}
```
<!-- error: 03_map_moved.clg -->
```
error[semantic/use-after-move]: use of moved-from value `m`
  --> 03_map_moved.clg:8:5
  help: `m` was moved earlier; assign a new value before using it
```

**Methods.** A map has the methods whose results the checker can follow, decided by the same rule as for `std::vector` and `string` (see above; the facts are `facts/std-map.clfacts`, scanned from the MSVC STL and
libstdc++ with two key types): `size`, `empty`, `max_size`, `count` and `clear`, and `erase(key)`, which returns how many entries it removed. The overloads of `erase` that take an iterator can never be called from
CeeLing, so they do not count against the name. Every method that mutates the map kills the observers of it, as `push_back` does for a vector.

<!-- example: 03_map_methods.clg -->
```cpp
int main() {
    std::map<string, int> stock;
    stock["apple"] = 3;
    stock["pear"] = 0;
    int removed = stock.erase("apple");
    std::cout << removed << " " << stock.count("apple") << " " << stock.count("pear") << " " << stock.size() << std::endl;
    return 0;
}
```
<!-- output: 03_map_methods.clg -->
```
1 0 1 1
```

**Where it stops.** `find`, `at`, `begin`, `insert`, `emplace`, `lower_bound` and the rest are refused, each with its reason: they return an iterator, a reference to an element or a pair, and none of those is tracked.
In the example above the lookup of a missing key still returns `0` and silently adds the entry, exactly as in C++; `count` is how you ask whether a key is there without adding it. There is no iteration.

<!-- example: 03_map_find_refused.clg -->
```cpp
int main() {
    std::map<string, int> stock;
    stock["apple"] = 3;
    stock.find("apple");
    return 0;
}
```
<!-- error: 03_map_find_refused.clg -->
```
error[semantic/forbidden-construct]: `find` is not available on `std::map<string, int>`: it returns an iterator, and iterators are not tracked yet, so nothing would stop one from dangling
  --> 03_map_find_refused.clg:4:5
  help: CeeLing admits a method of the standard library only when the checker can follow what it hands back; the rule and the scanned facts are in library_summary.cpp and facts/std-*.clfacts.
```

## Reading files

C++ needs a stream, a string stream and a decision about text or binary mode; the version below opens in text mode, which on Windows rewrites line endings and corrupts binary data:

<!-- example: 03_read_file.cpp -->
```cpp
// The boilerplate for "read a whole file". The stream is opened in text mode: on Windows that translates CRLF, which corrupts binary data.
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

std::string readFileToString(const std::string& path) {
    std::ifstream fin(path);
    if (!fin) {
        return std::string();
    }
    std::stringstream ss;
    ss << fin.rdbuf();
    return ss.str();
}

int main() {
    std::cout << readFileToString("no_such_file.txt").size() << std::endl;
    return 0;
}
```

**Why C++ does not stop it.** Nothing is wrong with the code the compiler can see; the failure is a property of the platform and the data.

CeeLing has no stream types, so three helpers are built into the compiler and defined in the generated file only when used: `cling::fileExists(path)` (a `bool`),
`cling::readFileToString(path)` (the text, in text mode) and `cling::readFileToBytes(path)` (a `std::vector<int>`, one element per byte, 0 to 255, read in binary mode so a stray `0x0D` is
kept). A file that cannot be opened gives an empty string or an empty vector, never an exception, which is why `fileExists` is separate: an empty file and a missing one look the same
otherwise. They take and return only `string`, `bool` and a vector of numbers, so they have nothing to borrow.

<!-- example: 03_read_file.clg -->
```cpp
int main() {
    string path = "no_such_file.txt";
    bool there = cling::fileExists(path);
    string text = cling::readFileToString(path);
    std::vector<int> bytes = cling::readFileToBytes(path);
    std::cout << "exists=" << there << " chars=" << text.length() << " bytes=" << bytes.size() << std::endl;
    return 0;
}
```
<!-- output: 03_read_file.clg -->
```
exists=0 chars=0 bytes=0
```

**Where it stops.** There is no way to write a file from CeeLing; that is an `ext_fn` ([chapter 11](11-talking-to-cpp.md)). The helpers' own C++ is trusted, not analysed.

## `Channel<T>`

`Channel<int> c;` is a queue that a spawned job and its creator can share. It is the one piece of shared state the language allows between threads, and it is explained, with its limits, in
[chapter 10](10-threads.md).

## Summary

| Feature | C++ problem | CeeLing | Example |
|---|---|---|---|
| `int64` | `long` has a platform-dependent width | 64 bits everywhere | `03_int64` |
| `byte`, `char` | `char` signedness is platform-defined | unsigned byte and a separate character | `03_byte` |
| casts | `(T)x` hides a `reinterpret_cast` | `int(x)` on primitives only | `03_cast_primitive`, `03_cast_string` |
| string escapes | greedy `\x`, octal, `\u` | a whitelist, `\xHH` exactly two digits | `03_string_escapes`, `03_escape_hex` |
| `s[i]`, `v[i]` | unchecked | checked, aborts with the line | `03_string_oob` |
| `c_str()` | raw pointer into a string | refused: the scan finds it returns a raw pointer | `03_c_str` |
| library calls | signatures hide borrows | only listed calls are accepted | `03_unknown_call` |
| `resize`, `reserve`, `copyWithin` | null elements | primitives only | `03_resize_objects` |
| vector parameters | copy or dangling reference | read-only ones by `const&`, others moved | `03_vector_param`, `03_vector_moved` |
| `std::map` | a read inserts | kept; primitives only; `size empty count clear erase(key)` only | `03_map`, `03_map_object_value` |
