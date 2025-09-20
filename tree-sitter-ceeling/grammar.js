module.exports = grammar({
  name: 'cling',

  extras: $ => [
    /\s/
  ],

  rules: {
    source_file: $ => repeat($.statement),

    statement: $ => choice(
      $.assignment_statement,
      $.variable_declaration,
      $.print_statement,
      $.comment_statement,
      $.function_declaration,
      $.return_statement,
      $.main_function,
      $.class_declaration,
      $.include_statement,
      $.if_statement
    ),

    assignment_statement: $ => seq(
      $.identifier,
      '=',
      $.expression,
      ';'
    ),

    variable_declaration: $ => seq(
      $.type,
      $.identifier,
      '=',
      $.expression,
      ';'
    ),

    type: $ => choice(
      'int',
      'float',
      'double',
      'string',
      'bool',
      $.identifier  // Add support for custom types (class names)
    ),

    print_statement: $ => seq(
      'print',
      '(',
      $.expression,
      ')',
      ';'
    ),

    comment_statement: $ => token(seq('//', /.*/)),

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

    function_declaration: $ => seq(
      'fn',
      $.identifier,
      '(',
      optional($.parameter_list),
      ')',
      '{',
      repeat($.statement),
      '}'
    ),

    main_function: $ => seq(
      $.type,
      'main',
      '(',
      ')',
      '{',
      repeat($.statement),
      '}'
    ),

    class_declaration: $ => seq(
      'class',
      $.identifier,
      '{',
      'public',
      ':',
      repeat(choice($.member_variable_declaration, $.method_declaration)),
      $.constructor_declaration,
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
      seq($.type, $.identifier),
      repeat(seq(',', $.type, $.identifier))
    ),

    if_statement: $ => seq(
      'if',
      '(',
      $.expression,
      ')',
      '{',
      repeat($.statement),
      '}'
    ),

    expression: $ => choice(
      $.binary_expression,
      $.function_call,
      $.new_expression,
      $.member_access,
      $.primary_expression
    ),

    binary_expression: $ => prec.left(1, seq($.primary_expression, choice('+', '-'), $.primary_expression)),

    new_expression: $ => seq(
      'new',
      $.identifier,
      '(',
      optional($.argument_list),
      ')'
    ),

    primary_expression: $ => choice(
      $.identifier,
      $.number,
      seq('(', $.expression, ')')
    ),

    member_access: $ => seq(
      $.primary_expression,
      '->',
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