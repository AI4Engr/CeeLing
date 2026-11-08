# Contributing

## Build and test

```bash
git clone --recursive <repository>
cmake -S . -B build                   # add -G Ninja for a faster build
cmake --build build --parallel
ctest --test-dir build                # the whole suite; a few tests skip themselves when g++, WSL, Eigen or jsoncpp are missing
```

Useful single runs:

```bash
bash tests/safety/run_safety_tests.sh --strict build/cling                       # the safety fixtures (cling.exe on Windows)
python -I docs/language-guide/examples/check.py check --cling build/cling        # every example in the guide
python -I docs/language-guide/examples/check.py write --cling build/cling        # regenerate the quoted outputs after a message changes
```

Two guide examples quote g++ 16 diagnostics (`06_const_ref_temporary`, `12_demand_use`), so `check.py` reports them on older compilers; examples that need a sibling project (cJSON-cling, gdb) are skipped when it is absent (`GUIDE_PROJECT_CJSON_CLING` points at it).

The generated parser `tree-sitter-ceeling/src/parser.c` is checked in. After editing `tree-sitter-ceeling/grammar.js`, run `tree-sitter generate` in that directory (`npm install -g tree-sitter-cli`) and commit the result.

## The rule for changes

The compiler's promise is a list of things that must be rejected and things that must be accepted. A change to what the compiler accepts needs a fixture:

* `tests/safety/NAME_rejected.clg` with `//@ expect: reject` and `//@ error: <text of the message>`, or
* `tests/safety/NAME_accepted.clg` with `//@ expect: accept` and, when the claim is about behaviour, `//@ run: <exact output>`.

Add a `//@ reason:` line saying what the fixture proves. A fixture that exists only to pass is not a test. The same applies to library facts: a fact that relaxes a check needs a program that would have dangled.

The language guide is part of the test suite. If a change alters a message or an example, regenerate it with `check.py write` and read the diff. [docs/language-guide/CONTRIBUTING.md](docs/language-guide/CONTRIBUTING.md) lists what the guide may claim (no unscoped "memory-safe", no numbers without a source).

## Source layout

| path | what |
|---|---|
| `src/syntax/` | the AST (`ast.h`) and the tree-sitter based AST builder |
| `src/passes/` | closure and struct-JSON desugaring, the escape-hatch, dangling / move and channel checks, smart-pointer inference |
| `src/facts/` | what the compiler knows about library calls: the fact table loader and `library_summary` |
| `src/emit/` | the C++ emitter and the runtime helper code it adds to generated files |
| `src/main.cpp` | the command line and the order of the passes |
| `tree-sitter-ceeling/` | the grammar and the generated parser |
| `facts/` | the library facts shipped inside the compiler (`std`, Eigen, `span`); format in [docs/internals/facts-format.md](docs/internals/facts-format.md) |
| `tests/` | the safety fixtures and the fact, scan, separate-compilation and debugging tests |
| `tools/` | `cling-scan` (fact scanners), the fact gauge and probes |
| `docs/language-guide/` | the guide; `docs/design/` design records |

The pipeline is: parse (`syntax`), desugar closures and struct decoders (`passes`), check, infer pointer kinds (`passes`), emit (`emit`).
