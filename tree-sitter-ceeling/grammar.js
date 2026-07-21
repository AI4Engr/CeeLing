module.exports = grammar({
  name: 'cling',

  extras: $ => [
    /\s/,
    // Line comments are skippable anywhere (like whitespace), so a trailing `// ...`
    // after a member declaration, statement, etc. never breaks parsing. Comments are
    // dropped from the generated C++, so they carry no AST node.
    $.comment
  ],

  rules: {
    source_file: $ => repeat($.statement),

    statement: $ => choice(
      $.assignment_statement,
      $.variable_declaration,
      $.function_declaration,
      $.return_statement,
      $.main_function,
      $.class_declaration,
      $.include_statement,
      $.if_statement,
      $.expression_statement
    ),

    // A bare expression used as a statement, e.g. `sink(a);`.
    expression_statement: $ => seq(
      $.expression,
      ';'
    ),

    // Assignment target is a bare name (`a = ...`) or a member (`a.b = ...`).
    assignment_statement: $ => seq(
      choice($.identifier, $.member_access),
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
      $.scoped_identifier,  // qualified names like std::vector (Phase 2.6 containers)
      $.identifier  // Add support for custom types (class names)
    ),

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

    class_declaration: $ => seq(
      'class',
      $.identifier,
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

    method_declaration: $ => seq(
      $.type,
      $.identifier,
      '(',
      optional($.parameter_list),
      ')',
      '{',
      repeat($.statement),
      '}'
    ),

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

    expression: $ => choice(
      $.binary_expression,
      $.unary_expression,
      $.function_call,
      $.new_expression,
      $.member_access,
      $.primary_expression
    ),

    binary_expression: $ => choice(
      // Arithmetic `+ -`: highest, left-associative, so `a + b + c` chains. Operands are
      // full expressions (not just primaries), enabling nesting like `(a + b) - c`.
      prec.left(3, seq($.expression, choice('+', '-'), $.expression)),
      // Comparison/equality: below arithmetic (so `a + b == c` groups as `(a+b) == c`).
      prec.left(2, seq($.expression, choice('==', '!=', '<', '>', '<=', '>='), $.expression)),
      // Stream output `a << b << c`: n-ary left-associative chaining, precedence 1
      // (below arithmetic, matching C++ so `std::cout << a + b` groups as `<< (a+b)`).
      // `<<` is tokenized explicitly so it never mis-lexes as two `<` comparisons.
      prec.left(1, seq($.expression, token('<<'), $.expression))
    ),

    // Unary prefix operators: `!x`, `-x`. Higher precedence than any binary operator.
    unary_expression: $ => prec(4, seq(
      choice('!', '-'),
      $.expression
    )),

    new_expression: $ => seq(
      'new',
      $.identifier,
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
    // to `->` in the emitted C++.
    member_access: $ => seq(
      $.primary_expression,
      choice('.', '->'),
      $.identifier
    ),

    function_call: $ => seq(
      choice($.identifier, $.member_access),
      '(',
      optional($.argument_list),
      ')'
    ),

    argument_list: $ => seq(
      $.expression,
      repeat(seq(',', $.expression))
    ),

    identifier: $ => /[a-zA-Z_][a-zA-Z0-9_]*/,
    number: $ => /\d+(\.\d+)?/
  }
});