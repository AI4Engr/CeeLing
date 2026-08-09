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
      $.return_statement,
      $.main_function,
      $.class_declaration,
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

    return_statement: $ => seq(
      'return',
      $.expression,
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
    function_declaration: $ => seq(
      $.type,
      $.identifier,
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
    class_declaration: $ => seq(
      'class',
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
    method_declaration: $ => seq(
      optional($.method_type_parameter),
      optional('virtual'),
      $.type,
      $.identifier,
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
      $.member_access,
      $.subscript_expression,
      $.primary_expression
    ),

    binary_expression: $ => choice(
      // Multiplicative `* /`: highest of the arithmetic group, left-associative,
      // matching C++ precedence (tighter than `+ -`).
      prec.left(6, seq($.expression, choice('*', '/'), $.expression)),
      // Arithmetic `+ -`: left-associative, so `a + b + c` chains. Operands are
      // full expressions (not just primaries), enabling nesting like `(a + b) - c`.
      prec.left(5, seq($.expression, choice('+', '-'), $.expression)),
      // Comparison/equality: below arithmetic (so `a + b == c` groups as `(a+b) == c`).
      prec.left(4, seq($.expression, choice('==', '!=', '<', '>', '<=', '>='), $.expression)),
      // Logical AND: below comparison (so `a == b && c == d` groups as `(a==b)&&(c==d)`),
      // matching C++ precedence (&& binds tighter than ||).
      prec.left(3, seq($.expression, '&&', $.expression)),
      // Logical OR: loosest of the boolean/comparison group.
      prec.left(2, seq($.expression, '||', $.expression)),
      // Stream output `a << b << c`: n-ary left-associative chaining, precedence 1
      // (below arithmetic, matching C++ so `std::cout << a + b` groups as `<< (a+b)`).
      // `<<` is tokenized explicitly so it never mis-lexes as two `<` comparisons.
      prec.left(1, seq($.expression, token('<<'), $.expression))
    ),

    // Unary prefix operators: `!x`, `-x`. Higher precedence than any binary operator.
    unary_expression: $ => prec(7, seq(
      choice('!', '-'),
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

    primary_expression: $ => choice(
      $.scoped_identifier,
      $.identifier,
      $.number,
      $.string_literal,
      seq('(', $.expression, ')')
    ),

    // Qualified name like `std::cout`, `std::endl`, `std::vector`. General `::` scope
    // resolution — usable in both expression and type position.
    scoped_identifier: $ => prec.left(seq(
      $.identifier,
      repeat1(seq('::', $.identifier))
    )),

    // A double-quoted string literal, e.g. `std::cout << " ";`.
    string_literal: $ => token(seq('"', /[^"]*/, '"')),

    // Member access is written with `.` in CeeLing source (Java-like). `->` is also
    // accepted for compatibility. The generator lowers `.` on smart-pointer objects
    // to `->` in the emitted C++. Left-associative and left-recursive so chains of
    // arbitrary depth (`a.b.c.d`) parse, not just a single `.` off a primary. Also
    // accepts a subscript as its left operand, so `v[i].field` parses.
    member_access: $ => prec.left(seq(
      choice($.member_access, $.subscript_expression, $.primary_expression),
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

    function_call: $ => seq(
      choice($.identifier, $.member_access, $.subscript_expression),
      '(',
      optional($.argument_list),
      ')'
    ),

    // `n.as<int>()` -- a call carrying an explicit type argument, resolving a
    // specialization of a same-named primary generic method (method_type_parameter/
    // method_specialization, above). This is the ONE genuinely risky grammar
    // addition in this slice: `<` after ANY expression is otherwise consumed only
    // by binary_expression's comparison rule (prec.left(4, ... '<' ...)), and BOTH
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