# CeeLing

CeeLing is a restricted, memory-safe dialect that looks like C++ and transpiles to plain C++. You write Java-like code (no `*`, no `&`, no `new`/`delete`, no `std::move`, no lifetime annotations); the compiler infers who owns what, picks `unique_ptr` / `shared_ptr` / `weak_ptr`, inserts the moves, and rejects the program if it could contain a dangling reference.

```
class Node {
public:
    int key;
    Node next;
    Node(int k) { key = k; }
};

int main() {
    Node head = new Node(1);
    head.next = new Node(2);
    std::cout << head.next.key << std::endl;
    return 0;
}
```

The output is ordinary C++ (`std::unique_ptr<Node>`, `std::make_unique`, `->`). Users of a library written in CeeLing never need CeeLing: they get the generated C++.

## What it promises, exactly

> If a program compiles under CeeLing, it contains no dangling references and no use after move.

plus two run-time checks that print the `.clg` line and abort: out-of-bounds `v[i]` / `s[i]` and a null object handle (both on by default, `--no-bounds-checks` / `--no-null-checks` turn them off).

What it does **not** claim: Rust-equivalent safety, thread safety (only `spawn` of plain values and `Channel<T>` are checked), or anything about the caller's C++, `ext_fn` shims and the libraries it calls (those are trusted boundaries, and `--report-facts` lists every library fact a program relied on). The full contract, the threat table with what is closed and what is open, and the reasoning are in [CLAUDE.md](CLAUDE.md) and the [Language Guide](docs/language-guide/README.md). [TODO.md](TODO.md) is the itemised status.

Positioning: safer than hand-written C++ while keeping the C++ ecosystem (std, Eigen, your own libraries) and requiring no annotations; not a replacement for Rust where the strongest guarantee is needed.

## Build

Requirements: CMake, a C++17 compiler (MinGW/GCC/Clang/MSVC), Git. The parser is generated with the tree-sitter CLI (`npm install -g tree-sitter-cli`) and `tree-sitter-ceeling/src/parser.c` is checked in, so regenerating is only needed after editing `tree-sitter-ceeling/grammar.js`.

```bash
git clone --recursive <this repository>      # vendor/tree-sitter is a submodule
./dev.sh setup && ./dev.sh build             # Windows: dev.bat init-submodules, dev.bat configure, dev.bat build
./dev.sh test                                # CTest
```

Use it:

```bash
build/cling file.clg --cpp out.cpp           # transpile (add -g for #line mapping back to the .clg, see guide chapter 11)
g++ -std=c++17 out.cpp -o program
```

`cling --facts mylib.clfacts` teaches the compiler about your own C++ library, `--report-facts` prints what a program trusted, `--strict` refuses facts nobody reviewed ([facts format](docs/internals/facts-format.md), [cling-scan](tools/cling-scan/README.md) writes a draft pack from a header).

## Layout

| path | what |
|---|---|
| `src/` | the compiler: AST builder, escape-hatch / dangling / move checkers, smart-pointer analyzer, C++ emitter |
| `tree-sitter-ceeling/` | the grammar and the generated parser |
| `facts/` | library facts shipped inside the compiler (`std::vector`, `string`, `map`, `optional`, `span`, Eigen...) |
| `tests/safety/` | the safety contract as fixtures: each is accepted (and run, with its output asserted) or must be rejected |
| `docs/language-guide/` | the guide; every example in it is compiled and checked by CTest |
| `docs/internals/` | implementation notes and history |
| `tools/` | fact scanners (`cling-scan`, `std-scan-experiment`), the fact gauge and probes |
| `examples/`, `playground/`, `regression/` | small demos, a debugging playground, transpile regression runs |

Companion projects written in CeeLing live in their own repositories (a zlib port, a cJSON port, a yaml-cpp port, an app shell).

## License

MIT, see [LICENSE](LICENSE).
