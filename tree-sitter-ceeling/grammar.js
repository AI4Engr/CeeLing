module.exports = grammar({
  name: 'cling',

  extras: $ => [
    /\s/,
    // Line comments are skippable anywhere (like whitespace), so a trailing `// ...`
    // after a member declaration, statement, etc. never breaks parsing. Comments are
    // dropped from the generated C++, so they carry no AST node.
    $.comment
  ],

  // `generic_type identifier` is a common prefix of both `container_declaration`
  // (no initializer: `std::vector<int> v;`) and `variable_declaration` (with
  // initializer: `std::vector<int> v = ...;`) -- the parser cannot know which until
  // it sees whether `;` or `=` follows the identifier, one token past where LALR
  // state-merging would otherwise report an unresolved conflict. GLR resolves this
  // correctly at parse time using that lookahead; this just tells the generator the
  // ambiguity is expected and real (not a grammar bug to eliminate via precedence).
  conflicts: $ => [
    [$.container_declaration, $.type],
    // `identifier '<'` and `member_access '<'` are both ambiguous between an
    // ordinary comparison (`x < y`, `x.as < y`) and the start of a call-site type
    // argument (`f<int>(...)`, `x.as<int>(...)`) -- both are legal at the exact
    // same $.expression position. GLR explores both and keeps whichever completes:
    // template_call only succeeds when it sees the full `< type > (` shape; an
    // ordinary comparison operand never does, so this resolves deterministically
    // at parse time with no real ambiguity in practice. Two entries because a bare
    // callee (`f<int>()`) and a method-call callee (`x.as<int>()`) hit the
    // ambiguity through different left-hand productions (primary_expression's
    // identifier vs. member_access itself).
    [$.primary_expression, $.template_call],
    [$.expression, $.template_call]
  ],

  rules: {
    source_file: $ => repeat($.statement),

    statement: $ => choice(
      $.assignment_statement,
      $.variable_declaration,
      $.container_declaration,
      $.function_declaration,
      $.ext_fn_declaration,
      $.return_statement,
      $.main_function,
      $.class_declaration,
      $.external_method_specialization,
      $.enum_declaration,
      $.namespace_declaration,
      $.include_statement,
      $.if_statement,
      $.while_statement,
      $.for_statement,
      $.throw_statement,
      $.try_statement,
      $.expression_statement
    ),

    // `ext_fn ReturnType name(params) [borrows N];` -- a pure SIGNATURE
    // declaration for a function whose real implementation is hand-written
    // C++ living outside CeeLing's own analysis (linked in via an ordinary
    // `#include`, exactly like imgui-cling's gfx_shim.h pattern -- see
    // library_summary.cpp's isKnownQualifiedFreeFunction doc comment). This
    // is CeeLing's escape hatch for "a little hand-written C++ at the
    // bottom" (CLAUDE.md/TODO.md's "External primitive-only function
    // declarations" gap) -- the source-level-keyword design the project
    // owner chose over a project-local declaration file (`.ceeling_extern_fns`)
    // specifically so the trust boundary is visible at the call site, not
    // hidden in a sibling file that can silently drift out of sync.
    //
    // No body, ends in `;` -- reuses `parameter_list` verbatim (so
    // `rejectObserverParams` keeps rejecting `ref` params here exactly like
    // an ordinary function_declaration, no new grammar-level rejection
    // needed). Leads with the `ext_fn` keyword for the SAME non-ambiguity
    // reason `closure_expression` leads with `closure`: `function_declaration`
    // has no leading keyword (starts directly with `$.type`), so a keyword
    // prefix here is what keeps the two productions unambiguous at the same
    // $.statement position with no `conflicts:` entry needed.
    //
    // `borrows N` is the ONLY new syntax beyond a bare signature: declares
    // that the return value aliases (borrows from) parameter index N,
    // matching DanglingChecker::FunctionSummary's own returnBorrowsParam
    // field. Absent means fresh/owned, the overwhelmingly common case (a
    // primitive/vector-returning helper like fastMemcmp8 needs no
    // annotation at all). A trailing clause rather than a per-parameter
    // decoration: only the RETURN can ever need a borrow annotation, since
    // CeeLing bans `ref` parameters outright, so every parameter is always
    // moved into the callee -- there is no per-parameter borrow state to
    // decorate.
    ext_fn_declaration: $ => seq(
      'ext_fn',
      $.type,
      $.identifier,
      '(',
      optional($.parameter_list),
      ')',
      optional(seq('borrows', $.number)),
      ';'
    ),

    // Declares a SPECIALIZATION of an EXISTING primary generic method (a
    // `<T> T methodName()`/`<T> T methodName(T x)`-shaped method already
    // declared inside `ClassName`'s own class body, possibly in a
    // DIFFERENT .clg file reached via `#include "foo.clh"`) from OUTSIDE
    // that class -- yaml-cpp's `convert<T>` customization-point shape
    // (a library consumer specializing `Node::as<MyType>()` for their own
    // type, without editing yaml_node.clg itself). Mirrors ordinary C++
    // out-of-line member syntax (`ReturnType Class::method(...) {...}`),
    // the one shape rust_backend otherwise has NO equivalent of at all
    // (no out-of-line method definitions exist anywhere else in this
    // grammar -- every ordinary method body lives inside its class_body).
    // `method_specialization` (below) still supplies the `<Type>` type
    // argument, exactly as it does for an IN-class specialization; the
    // only new surface here is the `ClassName::` qualifier making the
    // TARGET class explicit at the declaration site, since there is no
    // enclosing class_body to infer it from.
    external_method_specialization: $ => seq(
      $.type,
      choice($.identifier, $.scoped_identifier),  // target class name, optionally Namespace::ClassName
      '::',
      $.identifier,       // method name being specialized
      $.method_specialization,
      '(',
      optional($.parameter_list),
      ')',
      '{',
      repeat($.statement),
      '}'
    ),

    // An unscoped C++ enum: `enum Name { A, B, C };`. Deliberately unscoped (not
    // `enum class`) -- yaml-cling-shaped code stores an enum value in a plain `int`
    // field and passes an enumerator where an `int` is expected
    // (`new Node(NodeType::Undefined)`), which needs the implicit enumerator->int
    // conversion `enum class` forbids.
    enum_declaration: $ => seq(
      'enum',
      $.identifier,
      '{',
      $.identifier,
      repeat(seq(',', $.identifier)),
      optional(','),
      '}',
      optional(';')
    ),

    // `namespace Name { ... }` -- single level only (no nested namespaces; see
    // ast_builder.cpp's buildNamespaceDeclaration, which desugars this into
    // top-level declarations rather than a real nested scope, mirroring how single
    // inheritance stayed one base instead of a full base-chain to keep the analysis
    // modules unchanged).
    namespace_declaration: $ => seq(
      'namespace',
      $.identifier,
      '{',
      repeat($.statement),
      '}'
    ),

    // A bare expression used as a statement, e.g. `sink(a);`.
    expression_statement: $ => seq(
      $.expression,
      ';'
    ),

    // Assignment target is a bare name (`a = ...`), a member (`a.b = ...`), or a
    // subscript (`m[key] = ...` -- std::map insert-or-update, Phase 2.6 map slice).
    assignment_statement: $ => seq(
      choice($.identifier, $.member_access, $.subscript_expression),
      '=',
      $.expression,
      ';'
    ),

    variable_declaration: $ => seq(
      optional($.observer_qualifier),
      $.type,
      $.identifier,
      '=',
      $.expression,
      ';'
    ),

    // A container declared without an initializer, e.g. `std::vector<int> v;` --
    // default-constructs to empty. This is the ONE exception to CeeLing's definite-
    // initialization rule (Phase 1, CLAUDE.md hatch #18): a container's default
    // state (empty) is always well-defined and safe to read from, unlike a bare
    // `int n;` (uninitialized scalar) or a bare `Node n;` (no legal empty-object
    // state), so there is no safety reason to force an explicit initializer here.
    // Restricted to $.generic_type (not $.type generally) so this exception can
    // never apply to a plain class/primitive declaration.
    container_declaration: $ => seq(
      $.generic_type,
      $.identifier,
      ';'
    ),

    // `ref` marks a non-owning observer: `ref Node b = a;`. Observers are local-only
    // (they must not appear as function parameters) and are watched by the dangling
    // checker for use-after-owner-death.
    observer_qualifier: $ => 'ref',

    type: $ => choice(
      'void',
      'int',
      'float',
      'double',
      'string',
      'bool',
      'char',  // a pure VALUE type, like int/bool -- no char*/char[] production
               // exists anywhere in this grammar, so a raw C-string/char-array
               // shape is inexpressible by construction (the same escape-by-
               // construction philosophy CLAUDE.md uses for raw pointers
               // generally), not something a checker rejects after the fact.
      'byte',  // a pure VALUE type, unsigned 8-bit -- the unsigned counterpart
               // to `char` (which is SIGNED in generated C++ on this project's
               // toolchain), added so a `vector<byte>` byte-buffer's
               // index/arithmetic never silently goes negative the way
               // `vector<char>` can. Lowered to C++ `unsigned char` in
               // cppTypeFor (ast_emitter.cpp) -- the one primitive whose
               // CeeLing spelling is NOT identical to its C++ spelling.
      'int64', // a pure VALUE type, signed 64-bit -- named `int64`, not `long`,
               // specifically to avoid C++'s own `long` (32 or 64 bits
               // depending on platform/ABI -- e.g. 32 on Windows LLP64, 64 on
               // Linux LP64) implying a width guarantee this keyword does NOT
               // give if it borrowed that name. Lowered to C++ `long long`
               // (guaranteed >= 64 bits by the standard) in cppTypeFor, the
               // second primitive (after `byte`) whose CeeLing spelling
               // differs from its C++ spelling. Motivated by zlib-cling's
               // inflate/deflate bit-buffer refill, which zlib-rs does 8
               // bytes at a time into a 64-bit accumulator -- CeeLing's
               // `int` (32-bit signed) can only safely hold 31 usable bits
               // (bit 31 would be the sign bit), forcing a 4-byte refill
               // granularity instead of zlib-rs's 8.
      $.generic_type,        // std::vector<T> (Phase 2.6 containers)
      $.scoped_identifier,  // qualified names like std::vector (Phase 2.6 containers)
      $.identifier  // Add support for custom types (class names)
    ),

    // A one- or two-type-argument generic, e.g. `std::vector<Node>` or
    // `std::map<int, string>`. Recursive through $.type, so
    // `std::vector<std::vector<int>>` parses without extra work (nested generics are
    // unvalidated by the safety analysis for now, but not rejected at the grammar
    // level). The optional second argument is only meaningful for `std::map<K,V>`
    // (Phase 2.6 map slice) -- the grammar does not distinguish map from vector, that
    // happens downstream in library_summary.cpp by inspecting the captured type text.
    //
    // `scoped_identifier '<'` is ambiguous with a comparison expression
    // (`someScoped::Name < x`, e.g. as an operand -- rare, but grammatically legal
    // today). generic_type only ever appears in $.type position, never in
    // $.expression, so giving it higher precedence than the comparison alternative
    // in binary_expression correctly favors "this is a type" whenever a type is
    // expected and loses to "this is a comparison" everywhere else (there's no such
    // position/precedence conflict for genuinely ambiguous top-level expression
    // statements, since $.type is never itself a valid $.statement).
    generic_type: $ => prec(10, seq(
      $.scoped_identifier,
      '<',
      $.type,
      optional(seq(',', $.type)),
      '>'
    )),

    comment: $ => token(seq('//', /.*/)),

    include_statement: $ => seq(
      '#include',
      choice(
        seq('<', /[^>]+/, '>'),
        seq('"', /[^"]+/, '"')
      )
    ),

    // `optional($.expression)` -- a bare `return;` (void early-exit) is real,
    // legal CeeLing source (see tests/linked_list.clg's own deleteNode()).
    // Before this, the expression was REQUIRED, and a bare `return;` only
    // "worked" by accident: tree-sitter's GLR error recovery silently
    // inserted a zero-width MISSING node to satisfy the required slot,
    // choosing whichever primary_expression alternative its heuristic rated
    // cheapest -- an empty-name `identifier` before `this_expression`
    // existed (harmless: emitted `return ;`, valid C++ for void), but
    // `this_expression` once it was added (confirmed via `tree-sitter parse`
    // showing `(MISSING this_expression ...)`), which the `return this;` ->
    // `shared_from_this()` lowering then wrongly fired on. Relying on
    // undefined, heuristic-dependent error-recovery behavior for something
    // that should be an explicit, well-defined grammar production was
    // already the real bug; this makes bare `return;` a first-class case
    // instead of a lucky accident of recovery-choice ordering.
    return_statement: $ => seq(
      'return',
      optional($.expression),
      ';'
    ),

    // `throw <expr>;` -- restricted to string-typed operands (enforced downstream by
    // DanglingChecker, not the grammar; see dangling_checker.cpp's ThrowStatement
    // branch). CeeLing has no inheritance/exception-class hierarchy yet, so a plain
    // message is the only supported payload this slice.
    throw_statement: $ => seq(
      'throw',
      $.expression,
      ';'
    ),

    // `try { ... } catch (Type name) { ... }` -- single catch clause only (no
    // multiple catch blocks, no catch-all `...`). `Type` is grammatically general
    // (matches parameter_list's shape) but DanglingChecker rejects anything other
    // than `string` this slice, since `throw` itself only ever produces a string.
    try_statement: $ => seq(
      'try',
      '{',
      repeat($.statement),
      '}',
      'catch',
      '(',
      $.type,
      $.identifier,
      ')',
      '{',
      repeat($.statement),
      '}'
    ),

    // C++-style function declaration: `returnType name(params) { ... }`.
    // (There is no `fn` keyword; functions read like ordinary C++.)
    // The name is an ordinary identifier, OR (free-function-only) 'operator<<'
    // -- yaml-cpp's own `std::ostream& operator<<(std::ostream&, const Node&)`
    // is a FREE function, not a class method, so this is deliberately separate
    // from operator_method_name (below), the fixed 5-operator set scoped to
    // method_declaration. Narrowly just this one operator -- nothing else
    // needs a free-function operator name yet.
    function_declaration: $ => seq(
      $.type,
      choice($.identifier, 'operator<<'),
      '(',
      optional($.parameter_list),
      ')',
      '{',
      repeat($.statement),
      '}'
    ),

    // `main` is just a function whose name is "main"; kept as its own rule (higher
    // precedence) so it is recognized specifically even though it also matches the
    // general function_declaration shape.
    main_function: $ => prec(1, seq(
      $.type,
      'main',
      '(',
      ')',
      '{',
      repeat($.statement),
      '}'
    )),

    // Single inheritance only: `class Derived : public Base { ... }`. `public` is
    // required and explicit -- CeeLing has no private/protected inheritance, and
    // spelling it out keeps the generated C++ literal instead of relying on a silent
    // default. Multiple inheritance (including the pure-interface form) is a separate,
    // later increment; the grammar deliberately admits exactly one base so that
    // "which base?" is never ambiguous for the member-flattening pass downstream.
    //
    // `struct` is an EXPLICIT opt-in alternative to `class`, marking the
    // declared type as a plain VALUE type -- stored bare/unwrapped everywhere
    // (locals, fields, parameters, and critically, vector<T> elements), never
    // smart-pointer-wrapped. Reuses 100% of class_declaration's own grammar
    // (member/method/constructor productions); only the leading keyword
    // differs, read back in ASTBuilder::buildClassDeclaration to set
    // ClassDeclaration::isValueType. Deliberately NOT inferred structurally
    // (e.g. "all fields are primitives") -- a class with only primitive
    // fields could still be intentionally shared/mutable-reference (e.g. a
    // Counter incremented from multiple call sites), so CLAUDE.md's
    // "ambiguous -> conservative reject, never guess" rule means this needs
    // an explicit keyword, not inference.
    class_declaration: $ => seq(
      choice('class', 'struct'),
      $.identifier,
      optional(seq(':', 'public', $.identifier)),
      '{',
      'public',
      ':',
      // Members, methods and constructors may appear in any order (a constructor is
      // no longer required to be last), matching ordinary C++ class bodies.
      repeat(choice(
        $.member_variable_declaration,
        $.method_declaration,
        $.constructor_declaration
      )),
      '}',
      optional(';')
    ),

    member_variable_declaration: $ => seq(
      $.type,
      $.identifier,
      ';'
    ),

    // `const` after the parameter list marks a read-only method (`void f() const {...}`),
    // matching ordinary C++. CeeLing does not yet enforce const-correctness; the
    // qualifier is accepted syntactically and carried through so real C++ using it
    // (e.g. copied-in library code) parses cleanly.
    //
    // `virtual` (before the return type) and `override` (after the parameter list,
    // alongside `const`) carry polymorphic dispatch through to the generated C++ --
    // CeeLing does not implement its own vtable, it reuses the C++ compiler's. Both
    // are placed exactly where ordinary C++ puts them, so the syntax reads the same.
    //
    // A leading `<T>` (`method_type_parameter`) declares a PRIMARY generic method,
    // narrowly scoped to return-type genericity only (`<T> T as() {...}`) -- the
    // shape yaml-cpp's `node.as<T>()` needs, nothing more general (no generic
    // parameter types, no multiple type parameters). A trailing `<Type>` right after
    // the method name (`method_specialization`) declares a SPECIALIZATION of a
    // same-named primary (`int as<int>() {...}`). Both positions are inside a
    // DECLARATION, never in expression position, so neither competes with
    // binary_expression's comparison rule the way the call-site type argument
    // (template_call, below) does -- no conflict risk here.
    //
    // The method NAME may be an ordinary identifier or one of the supported
    // operator names (operator_method_name, below) -- narrowly scoped to exactly
    // what yaml-cpp's Node and a range-for-compatible iterator need:
    // `operator[]`/`operator=` (subscript/assignment) plus `operator*`/
    // `operator++`/`operator!=` (the C++ iterator protocol -- dereference, prefix
    // increment only (no postfix), inequality only (no `operator==`, unneeded by
    // range-for's desugaring)). General operator overloading (operator+,
    // operator<<, ...) beyond this fixed set is a separate, unstarted TODO.md item,
    // not this.
    method_declaration: $ => seq(
      optional($.method_type_parameter),
      optional('virtual'),
      $.type,
      choice($.identifier, $.operator_method_name),
      optional($.method_specialization),
      '(',
      optional($.parameter_list),
      ')',
      optional('const'),
      optional('override'),
      '{',
      repeat($.statement),
      '}'
    ),

    // A supported operator as a method name. Kept as its own token (not built
    // from separate `operator`/`[`/`]`/`=`/etc. terminals) so it can never be
    // mistaken for `operator` used as an ordinary identifier elsewhere -- there is
    // no other place in the grammar `operator` is meaningful, so this is
    // unambiguous.
    operator_method_name: $ => choice(
      'operator[]', 'operator=', 'operator*', 'operator++', 'operator!=', 'operator=='
    ),

    // `<T>` before a method's return type: declares ONE type parameter, usable only
    // as that method's own return type (`<T> T as()`). Enforced in ast_builder.cpp,
    // not here -- the grammar doesn't know the return type must equal T.
    method_type_parameter: $ => seq('<', $.identifier, '>'),

    // `<int>` right after a method name: this declaration SPECIALIZES a same-named
    // primary generic method for one concrete type. Single type argument only
    // (matches the narrowed scope -- no `f<A,B>()`).
    method_specialization: $ => seq('<', $.type, '>'),

    constructor_declaration: $ => seq(
      $.identifier,
      '(',
      optional($.parameter_list),
      ')',
      '{',
      repeat($.statement),
      '}'
    ),

    parameter_list: $ => seq(
      seq(optional($.observer_qualifier), $.type, $.identifier),
      repeat(seq(',', optional($.observer_qualifier), $.type, $.identifier))
    ),

    if_statement: $ => prec.right(seq(
      'if',
      '(',
      $.expression,
      ')',
      '{',
      repeat($.statement),
      '}',
      optional(seq(
        'else',
        choice(
          $.if_statement,                          // else if ...
          seq('{', repeat($.statement), '}')       // else { ... }
        )
      ))
    )),

    while_statement: $ => seq(
      'while',
      '(',
      $.expression,
      ')',
      '{',
      repeat($.statement),
      '}'
    ),

    // Range-based for only: `for (Type name : container) { ... }`, matching native
    // C++ range-for syntax 1:1 (no translation needed at emission). C-style
    // `for(init;cond;incr)` is a separate, not-yet-supported grammar addition.
    for_statement: $ => seq(
      'for',
      '(',
      $.type,
      $.identifier,
      ':',
      $.expression,
      ')',
      '{',
      repeat($.statement),
      '}'
    ),

    expression: $ => choice(
      $.binary_expression,
      $.unary_expression,
      $.template_call,
      $.function_call,
      $.new_expression,
      $.closure_expression,
      $.member_access,
      $.subscript_expression,
      $.primary_expression
    ),

    binary_expression: $ => choice(
      // Multiplicative `* / %`: highest of the arithmetic group, left-associative,
      // matching C++ precedence (tighter than `+ -`). `%` added for zlib-cling's
      // adler32 (`% 65521`) and zlib header check (`% 31`) -- mechanically
      // identical to `*`/`/`, same tier, no new AST/checker work (ast_builder,
      // EscapeHatchChecker, DanglingChecker are all operator-text-agnostic here,
      // same as every prior binary-operator addition).
      prec.left(9, seq($.expression, choice('*', '/', '%'), $.expression)),
      // Arithmetic `+ -`: left-associative, so `a + b + c` chains. Operands are
      // full expressions (not just primaries), enabling nesting like `(a + b) - c`.
      prec.left(8, seq($.expression, choice('+', '-'), $.expression)),
      // Comparison/equality: below arithmetic (so `a + b == c` groups as `(a+b) == c`).
      prec.left(7, seq($.expression, choice('==', '!=', '<', '>', '<=', '>='), $.expression)),
      // Shift `<< >>` AND stream output `a << b << c` -- ONE shared tier, by
      // necessity. `<<` has two meanings in CeeLing (integer shift; ostream
      // insertion) and they cannot live at two different precedence tiers:
      // tree-sitter's GLR then hits a genuinely unresolvable conflict on
      // `a & b << c` (the two `<<` alternatives straddle `&`, so neither
      // precedence can win) -- confirmed by `tree-sitter generate` refusing
      // the two-tier version outright. So both meanings share this tier, and
      // the distinction is made downstream, not by the parser:
      //   * WHICH MEANING is emitted -- AstEmitter checks whether the chain's
      //     leftmost leaf is a stream (std::cout/cerr/clog or an `ostream`-typed
      //     variable). Stream chains emit flat and paren-free
      //     (`std::cout << a << b`); everything else emits ordinary grouped
      //     output (`(a << b)`), which is what makes `(1 << 4) + 1` mean 17.
      //   * WHICH OPERATOR RESOLVES -- left entirely to C++'s own overload
      //     resolution on the operand types: `int << int` is a shift,
      //     `ostream << T` is insertion. CeeLing needs no operand-type analysis.
      //
      // Tier placement (6) is C++'s own: below arithmetic, so `a << b + c`
      // groups as `a << (b+c)` (and `std::cout << a + b` as `<< (a+b)`, the
      // behavior the old precedence-1 stream tier existed to get), and above
      // comparison, so `a >> b == c` groups as `(a >> b) == c`. It sits ABOVE
      // bitwise `&`/`|` below, unlike C++ -- real shift code parenthesizes its
      // masks anyway (`1 << (x & 7)`, see zlib-rs's inflate/bitreader.rs), so
      // this costs nothing in practice and leaves the `&`/`|` tiers unmoved.
      // Both tokens are tokenized explicitly so they never mis-lex as two
      // `<`/`>` comparisons -- that mis-lex is exactly what made `a >> c`
      // silently emit `((a > this) > c)` before this tier existed.
      prec.left(6, seq($.expression, choice(token('<<'), token('>>')), $.expression)),
      // Bitwise AND: above bitwise OR -- matching real C++ precedence relative
      // to `|` (`a & b | c` groups as `(a & b) | c`). Scope deliberately
      // narrow: only `&`/`|` here -- no `^` (not needed by any current
      // consumer). `~` (bitwise NOT) is a unary_expression alternative, see
      // below. Shift `<<`/`>>` lives one tier ABOVE, see above.
      prec.left(5, seq($.expression, '&', $.expression)),
      // Bitwise OR: below bitwise AND (matching C++: `a & b | c` groups as
      // `(a & b) | c`), above logical &&/||.
      prec.left(4, seq($.expression, '|', $.expression)),
      // Logical AND: below bitwise OR (so `a | b && c` groups as `(a|b) && c`),
      // matching C++ precedence (&& binds tighter than ||).
      prec.left(3, seq($.expression, '&&', $.expression)),
      // Logical OR: loosest of the boolean/comparison group.
      //
      // NOTE stream output `<<` used to live below here at its own precedence-1
      // tier. It now shares the shift tier above -- see that tier's comment for
      // why two tiers for one token is impossible. One user-visible consequence:
      // a stream operand that is itself a loose boolean expression now needs
      // explicit parens (`std::cout << (a || b)`), since `<<` binds tighter
      // than `||` as it does in real C++.
      prec.left(2, seq($.expression, '||', $.expression))
    ),

    // Unary prefix operators: `!x`, `-x`, `*x` (dereference, e.g. an iterator's
    // `operator*`), `++x` (prefix increment ONLY -- no postfix `x++`, unneeded by
    // range-for's `++it` desugaring and would need a disambiguating dummy-`int`
    // parameter shape nothing here requires), `~x` (bitwise NOT -- added for
    // zlib-cling's stored-block NLEN check, `~nlenField & 0xFFFF`; unlike `*`
    // it has no binary_expression counterpart at all, so it is unambiguous by
    // construction -- no disambiguation concern the way `*` needed one).
    // Higher precedence than any binary operator -- kept one tier above
    // binary_expression's own highest tier (currently `* / %` at 9; bumped
    // from 6 when bitwise `&`/`|` were inserted below comparison, then 7->9
    // and now 9->10 when bitwise shift was inserted and the arithmetic/
    // comparison group shifted up to make room -- this must move with that
    // group to preserve the invariant, not get left behind at the old
    // number). `*` here is unambiguous against binary_expression's
    // multiplication rule (`a * b`) -- a leading `*` can only be prefix since
    // binary `*` requires an already-parsed left operand first.
    unary_expression: $ => prec(10, seq(
      choice('!', '-', '*', '++', '~'),
      $.expression
    )),

    // `new Node(...)` or, when the class lives in a namespace, `new YAML::Node(...)`.
    new_expression: $ => seq(
      'new',
      choice($.identifier, $.scoped_identifier),
      '(',
      optional($.argument_list),
      ')'
    ),

    // `closure IFace [captureA, captureB] (int x) { ...body... }` -- a by-value-
    // capture-only closure implementing the single-virtual-method interface
    // `IFace` (validated/desugared in ASTBuilder, not here). Leads with the
    // `closure` keyword for the SAME reason `new_expression` above needs no
    // `prec` wrapper: a leading keyword makes the whole production unambiguous
    // at the exact position `$.expression` already appears. A leading `[`
    // instead (mimicking C++ `[x](...){...}`) would collide with
    // subscript_expression's own infix `[` (below) -- the same real ambiguity
    // template_call's `<` hit against binary_expression's comparison operator,
    // which needed two `conflicts:` entries to resolve via GLR. The keyword
    // avoids that class of problem entirely; no `conflicts:` entry is needed
    // for this rule.
    //
    // Capture list is BARE IDENTIFIERS ONLY -- no `&`, no `=`. This is a
    // deliberate language-level decision, not a missing feature: by-reference
    // capture is a new dangling-reference source (CLAUDE.md threat #7,
    // "lambda captures `[&]` local, then escapes") that this compiler's
    // scope-aware, per-body analysis cannot verify once a closure escapes its
    // creating function (see the design notes this fixture set's `closure_*`
    // fixtures document). Making the capture list syntactically incapable of
    // spelling a reference capture closes that door the same way the grammar
    // has no `ref`-qualified FIELD production at all (ast_builder.cpp's
    // ref_field_rejected.clg-adjacent comments) -- sealed by construction, not
    // by a check that could be forgotten.
    //
    // Body shape (`'{', repeat($.statement), '}'`) is copied verbatim from
    // function_declaration above -- this is the first EXPRESSION production in
    // the grammar to contain statements (every other expression rule bottoms
    // out in primary_expression, never $.statement), making expression and
    // statement mutually recursive through this one rule.
    closure_expression: $ => seq(
      'closure',
      $.identifier,
      '[', optional($.capture_list), ']',
      '(',
      optional($.parameter_list),
      ')',
      '{',
      repeat($.statement),
      '}'
    ),

    capture_list: $ => seq(
      $.identifier,
      repeat(seq(',', $.identifier))
    ),

    primary_expression: $ => choice(
      $.scoped_identifier,
      $.identifier,
      $.this_expression,
      $.number,
      $.string_literal,
      $.char_literal,
      seq('(', $.expression, ')')
    ),

    // `this` -- a reserved keyword, not an ordinary identifier (unlike the
    // earlier approach, where `this` was just an Identifier whose NAME
    // happened to be special-cased by string comparison throughout the
    // generator). Tree-sitter's lexer already prefers an explicit string-
    // literal token like this one over the general `identifier` regex
    // whenever both could match -- the same mechanism that already makes
    // `new`/`class`/every other keyword unusable as a declared name with no
    // exclusion regex needed on `identifier` itself. Consequence: `this` can
    // no longer be used anywhere an `identifier` is expected (a variable,
    // parameter, field, or class named `this`; a closure capture-list entry
    // `[this]`, since capture_list's own grammar uses `$.identifier`
    // directly) -- these now fail to PARSE rather than needing a semantic
    // check to catch them.
    this_expression: $ => 'this',

    // Qualified name like `std::cout`, `std::endl`, `std::vector`. General `::` scope
    // resolution — usable in both expression and type position.
    scoped_identifier: $ => prec.left(seq(
      $.identifier,
      repeat1(seq('::', $.identifier))
    )),

    // A double-quoted string literal, e.g. `std::cout << " ";`.
    string_literal: $ => token(seq('"', /[^"]*/, '"')),

    // A single-quoted char literal, e.g. `':'`, `' '`, `'\n'`. Either one plain
    // character, or a backslash escape (\n \r \t \\ \' \0) -- confirmed needed
    // by a real consumer (a hand-rolled scanner testing `peek() == '\n'`/
    // `'\r'` for line-ending detection) rather than built speculatively.
    char_literal: $ => token(choice(
      seq("'", /[^'\\]/, "'"),
      seq("'", '\\', /[nrt\\'0]/, "'")
    )),

    // Member access is written with `.` in CeeLing source (Java-like). `->` is also
    // accepted for compatibility. The generator lowers `.` on smart-pointer objects
    // to `->` in the emitted C++. Left-associative and left-recursive so chains of
    // arbitrary depth (`a.b.c.d`) parse, not just a single `.` off a primary. Also
    // accepts a subscript as its left operand, so `v[i].field` parses, and a
    // function_call as its left operand, so `f().field` / `a.method().field`
    // parse -- confirmed as a REAL, previously-silent gap (not just "worth
    // adding"): without this, `h.get().val` parsed the `.val` half as a bare
    // ERROR node sibling (verified via `tree-sitter parse` directly), and the
    // AST builder's error-recovery silently dropped it with no diagnostic --
    // `int x = h.get().val;` transpiled clean and only failed at the final
    // g++ step, with a confusing type-mismatch error pointing nowhere near
    // the real cause. This is exactly the class of bug Phase 1's
    // rejectUnbuildableChild discipline exists to catch, but the ERROR node
    // here was a top-level sibling, not nested inside a builder call that
    // would have surfaced it.
    member_access: $ => prec.left(seq(
      choice($.member_access, $.subscript_expression, $.function_call, $.primary_expression),
      choice('.', '->'),
      $.identifier
    )),

    // Subscript access `v[i]`. Binds as tightly as `.`/`->` -- by the SAME technique
    // member_access/function_call already use (restricting the left operand to
    // member_access/subscript_expression/primary_expression, not general
    // $.expression), not by a numeric precedence climb. Composes with member_access
    // in both directions (`v[i].field` above; `a.v[i]` here) and with function_call
    // (`v[i].method()`), so chains of arbitrary shape parse.
    subscript_expression: $ => prec.left(seq(
      choice($.subscript_expression, $.member_access, $.primary_expression),
      '[',
      $.expression,
      ']'
    )),

    // A plain call's callee may be a qualified free function (`std::stoi(x)`) as
    // well as a bare identifier -- library allow-list calls only, never a
    // user-defined qualified name (CeeLing has no qualified declarations of its
    // own to call). No new ambiguity: a scoped_identifier callee is only ever
    // followed by `(` here, never `<` the way generic_type's scoped_identifier is.
    function_call: $ => seq(
      choice($.identifier, $.scoped_identifier, $.member_access, $.subscript_expression),
      '(',
      optional($.argument_list),
      ')'
    ),

    // `n.as<int>()` -- a call carrying an explicit type argument, resolving a
    // specialization of a same-named primary generic method (method_type_parameter/
    // method_specialization, above). This is the ONE genuinely risky grammar
    // addition in this slice: `<` after ANY expression is otherwise consumed only
    // by binary_expression's comparison rule (prec.left(6, ... '<' ...)), and BOTH
    // interpretations are legal at the exact same $.expression position -- unlike
    // generic_type's `scoped_identifier '<'` (which only ever appears in $.type
    // position and so could be resolved with a plain numeric prec()), this is a
    // REAL ambiguity needing GLR to explore both and keep whichever completes. The
    // `conflicts:` entry below tells the generator this is expected. In practice an
    // ordinary comparison (`a.length() < 5`, `a < b`) never has a `type`-shaped
    // right operand immediately followed by `>` then `(`, so it can only ever
    // resolve as binary_expression; template_call only completes for the exact
    // shape a real call-site type argument has.
    template_call: $ => prec.left(seq(
      choice($.identifier, $.member_access, $.subscript_expression),
      '<',
      $.type,
      '>',
      '(',
      optional($.argument_list),
      ')'
    )),

    argument_list: $ => seq(
      $.expression,
      repeat(seq(',', $.expression))
    ),

    identifier: $ => /[a-zA-Z_][a-zA-Z0-9_]*/,
    number: $ => /\d+(\.\d+)?/
  }
});