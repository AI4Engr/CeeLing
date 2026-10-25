#pragma once
#include <memory>
#include <set>
#include <string>
#include <vector>
#include <variant>

// AST Node definitions for CLing language

namespace cling {

// Source position of a node, 1-based line/column. line == 0 means "unknown"
// (e.g. synthesized nodes). Populated by the AST builder from the Tree-sitter CST;
// used by the safety analysis to point diagnostics at the originating .clg line.
struct SourceLoc {
    int line = 0;
    int column = 0;
};

class ASTNode {
public:
    virtual ~ASTNode() = default;
    virtual std::string toString() const = 0;

    SourceLoc loc;  // source position; line == 0 if unset
};

using ASTNodePtr = std::unique_ptr<ASTNode>;

class Expression : public ASTNode {
public:
    virtual ~Expression() = default;
};

using ExpressionPtr = std::unique_ptr<Expression>;

class Statement : public ASTNode {
public:
    virtual ~Statement() = default;

    // True for a top-level declaration (class/function/enum) pulled in ONLY to make
    // an `#include`d sibling .clg file's types/functions visible to this file's own
    // semantic analysis (EscapeHatchChecker/SmartPointerAnalyzer/DanglingChecker) --
    // it is NOT part of this file's own source and must never be re-emitted by
    // AstEmitter::emit/emitHeader, or the including file's .cpp/.clh would duplicate
    // a definition that already exists in the included file's own .cpp/.clh (an ODR
    // violation at link time). See main.cpp's collectExternalDeclarations.
    bool isExternalDeclaration = false;

    // The source file this top-level statement was parsed from (set by main.cpp for the primary file and for
    // every #include'd sibling). Used to give synthesized names -- the classes closures desugar into -- a stem
    // that is stable no matter which file is being compiled, so two files that both contain closures never
    // generate the same class name (they used to: each counted from 0, and the merged declaration of the
    // second silently collided with the first's definition at link time).
    std::string originFile;
};

using StatementPtr = std::unique_ptr<Statement>;

// Expression implementations
class Identifier : public Expression {
public:
    std::string name;

    explicit Identifier(const std::string& n) : name(n) {}
    std::string toString() const override { return name; }
};

class Number : public Expression {
public:
    std::string original_text;

    explicit Number(const std::string& text) : original_text(text) {}
    std::string toString() const override { return original_text; }
};

// `this` -- a reserved keyword (grammar.js's `this_expression`), not an
// ordinary Identifier whose name happens to be "this". A distinct node type
// so every backend pass can dynamic_cast against it directly instead of
// string-comparing an Identifier's name -- see docs/language-guide/18-
// implementation-notes.md's Step 37 for the full migration this replaced.
class ThisExpression : public Expression {
public:
    std::string toString() const override { return "this"; }
};

// A qualified name like `std::cout`, `std::endl`, `std::vector`. Stored as the full
// joined text; the generator emits it verbatim.
class ScopedIdentifier : public Expression {
public:
    std::string qualifiedName;

    explicit ScopedIdentifier(const std::string& name) : qualifiedName(name) {}
    std::string toString() const override { return qualifiedName; }
};

// A double-quoted string literal, e.g. `" "` in `std::cout << " ";`. Holds the
// original text including the quotes; emitted verbatim.
class StringLiteral : public Expression {
public:
    std::string original_text;

    explicit StringLiteral(const std::string& text) : original_text(text) {}
    std::string toString() const override { return original_text; }
};

// A single-quoted char literal, e.g. `':'`. Holds the original text including
// the quotes (mirrors StringLiteral); emitted verbatim -- CeeLing's `'x'` and
// C++'s `'x'` char-literal syntax are identical, so no translation is needed.
class CharLiteral : public Expression {
public:
    std::string original_text;

    explicit CharLiteral(const std::string& text) : original_text(text) {}
    std::string toString() const override { return original_text; }
};

class BinaryExpression : public Expression {
public:
    ExpressionPtr left;
    std::string operator_str;
    ExpressionPtr right;

    BinaryExpression(ExpressionPtr l, const std::string& op, ExpressionPtr r)
        : left(std::move(l)), operator_str(op), right(std::move(r)) {}

    std::string toString() const override {
        return "(" + left->toString() + " " + operator_str + " " + right->toString() + ")";
    }
};

// A unary prefix expression, e.g. `!left` or `-x`.
class UnaryExpression : public Expression {
public:
    std::string operator_str;
    ExpressionPtr operand;

    UnaryExpression(const std::string& op, ExpressionPtr expr)
        : operator_str(op), operand(std::move(expr)) {}

    std::string toString() const override {
        return operator_str + operand->toString();
    }
};

class FunctionCall : public Expression {
public:
    std::string function_name;
    std::vector<ExpressionPtr> arguments;
    // For method calls `recv.method(args)`, `callee` holds the `recv.method`
    // MemberAccess so the receiver's access operator can be lowered correctly.
    // Null for a plain call `f(args)` (use function_name).
    ExpressionPtr callee;
    // Call-site type argument (`n.as<int>()` -> "int"), empty for an ordinary call.
    // A call carrying this resolves to a METHOD SPECIALIZATION, not the bare method
    // name -- every analysis module looks it up under the synthetic key
    // specializationKey(methodName, templateArg) instead of the bare name whenever
    // this is non-empty (see class_hierarchy.h).
    std::string templateArg;

    FunctionCall(const std::string& name, std::vector<ExpressionPtr> args)
        : function_name(name), arguments(std::move(args)) {}

    std::string toString() const override {
        std::string result = function_name;
        if (!templateArg.empty()) result += "<" + templateArg + ">";
        result += "(";
        for (size_t i = 0; i < arguments.size(); ++i) {
            if (i > 0) result += ", ";
            result += arguments[i]->toString();
        }
        result += ")";
        return result;
    }
};

// Statement implementations
class VariableDeclaration : public Statement {
public:
    std::string type;
    std::string identifier;
    ExpressionPtr expression;
    bool isObserver = false;  // true if declared with `ref` (non-owning observer)

    VariableDeclaration(const std::string& t, const std::string& id, ExpressionPtr expr,
                        bool observer = false)
        : type(t), identifier(id), expression(std::move(expr)), isObserver(observer) {}

    std::string toString() const override {
        return std::string(isObserver ? "ref " : "") + type + " " + identifier +
               " = " + expression->toString() + ";";
    }
};

// A container declared without an initializer, e.g. `std::vector<int> v;` --
// default-constructs to empty. The one exception to CeeLing's definite-
// initialization rule (see CLAUDE.md hatch #18): a container's default (empty)
// state is always well-defined and safe to read, unlike a bare scalar or class
// declaration. `type` is always a generic-type string (e.g. "std::vector<int>");
// the grammar restricts this production to $.generic_type, never a plain type.
class ContainerDeclaration : public Statement {
public:
    std::string type;
    std::string identifier;

    ContainerDeclaration(const std::string& t, const std::string& id)
        : type(t), identifier(id) {}

    std::string toString() const override {
        return type + " " + identifier + ";";
    }
};

class AssignmentStatement : public Statement {
public:
    // For a bare-name target (`a = ...`), `variable` holds the name and `target` is
    // null. For a member target (`a.b = ...`), `target` holds the MemberAccess and
    // `variable` holds the root object name (`a`) for the analysis passes.
    std::string variable;
    ExpressionPtr expression;
    ExpressionPtr target;  // non-null iff the LHS is a member access

    AssignmentStatement(const std::string& var, ExpressionPtr expr)
        : variable(var), expression(std::move(expr)) {}

    std::string toString() const override {
        std::string lhs = target ? target->toString() : variable;
        return lhs + " := " + expression->toString() + ";";
    }
};

// A bare expression used as a statement, e.g. `sink(a);`. Its main role for the
// safety analysis is to make move events at call sites visible to the checker.
class ExpressionStatement : public Statement {
public:
    ExpressionPtr expression;

    explicit ExpressionStatement(ExpressionPtr expr)
        : expression(std::move(expr)) {}

    std::string toString() const override {
        return expression->toString() + ";";
    }
};

class CommentStatement : public Statement {
public:
    std::string content;

    explicit CommentStatement(const std::string& comment)
        : content(comment) {}

    std::string toString() const override {
        return "//" + content;
    }
};

class ReturnStatement : public Statement {
public:
    // Null for a bare `return;` (void early-exit) -- see grammar.js's
    // `optional($.expression)` on return_statement.
    ExpressionPtr expression;

    explicit ReturnStatement(ExpressionPtr expr)
        : expression(std::move(expr)) {}

    std::string toString() const override {
        return expression ? "return " + expression->toString() + ";" : "return;";
    }
};

// `throw <expr>;` -- restricted to string-typed operands (DanglingChecker enforces
// this, not the grammar; see its ThrowStatement branch). Structurally identical to
// ReturnStatement (a single carried expression); kept as a distinct AST node rather
// than reusing ReturnStatement because the two mean different things to both the
// dangling checker (a throw doesn't hand a value back to the caller the way return
// does) and the emitter (`throw expr;` vs `return expr;`).
class ThrowStatement : public Statement {
public:
    ExpressionPtr expression;

    explicit ThrowStatement(ExpressionPtr expr)
        : expression(std::move(expr)) {}

    std::string toString() const override {
        return "throw " + expression->toString() + ";";
    }
};

class FunctionDeclaration : public Statement {
public:
    std::string returnType;
    std::string name;
    // (type, name) pairs, like MethodDeclaration. C++-style: `int f(int a, Node b)`.
    std::vector<std::pair<std::string, std::string>> parameters;
    std::vector<StatementPtr> body;
    // Enclosing namespace name, or empty if this function was declared at the
    // top level -- same desugaring ClassDeclaration/EnumDeclaration already
    // get (see their own namespaceName fields for why): `namespace X { int f()
    // {...} }` is DESUGARED by the AST builder into a top-level
    // FunctionDeclaration with namespaceName="X". Free functions were not
    // included in the original namespace design (only classes/enums were);
    // added when yaml-cling's `YAML::Load(...)` needed a namespaced free
    // function to actually resolve.
    std::string namespaceName;

    FunctionDeclaration(const std::string& retType,
                        const std::string& func_name,
                        std::vector<std::pair<std::string, std::string>> params,
                        std::vector<StatementPtr> statements)
        : returnType(retType), name(func_name),
          parameters(std::move(params)), body(std::move(statements)) {}

    std::string toString() const override {
        std::string result = returnType + " " + name + "(";
        for (size_t i = 0; i < parameters.size(); ++i) {
            if (i > 0) result += ", ";
            result += parameters[i].first + " " + parameters[i].second;
        }
        result += ") {\n";
        for (const auto& stmt : body) {
            result += "    " + stmt->toString() + "\n";
        }
        result += "}";
        return result;
    }
};

// `ext_fn ReturnType name(params) [borrows N];` -- a pure signature
// declaration for a function whose real implementation is hand-written C++
// living outside CeeLing's own analysis (see grammar.js's ext_fn_declaration
// doc comment for the full design rationale). Deliberately has NO body field
// at all -- that structural absence, not just a naming difference, is what
// distinguishes it from FunctionDeclaration and is why every checker that
// walks FunctionDeclaration bodies (SmartPointerAnalyzer, DanglingChecker's
// deriveSummaryForBody) simply has nothing to do for this node: there is no
// body to infer ownership/borrow behavior from, only the hand-declared
// returnBorrowsParam fact below.
class ExternFunctionDeclaration : public Statement {
public:
    std::string returnType;
    std::string name;
    std::vector<std::pair<std::string, std::string>> parameters;
    // Index into `parameters` that the return value aliases (borrows from),
    // or -1 if the return is always fresh/owned -- mirrors
    // DanglingChecker::FunctionSummary::returnBorrowsParam exactly, since an
    // ext_fn's hand-written summary IS a FunctionSummary, just supplied by
    // the user instead of derived from a body.
    int returnBorrowsParam = -1;
    // Indices into `parameters` declared `ref` (pass-by-reference instead of
    // by value). ext_fn-only: `ref` stays hard-rejected in every other
    // parameter list (rejectObserverParams). A container ref lowers to
    // `const T&` (read-only borrow); a primitive ref lowers to `T&` (mutable
    // out-param) -- see the type-decides-meaning table in TODO.md's ext_fn entry.
    std::set<size_t> refParams;

    ExternFunctionDeclaration(const std::string& retType,
                              const std::string& func_name,
                              std::vector<std::pair<std::string, std::string>> params,
                              int borrowsParam)
        : returnType(retType), name(func_name),
          parameters(std::move(params)), returnBorrowsParam(borrowsParam) {}

    std::string toString() const override {
        std::string result = "ext_fn " + returnType + " " + name + "(";
        for (size_t i = 0; i < parameters.size(); ++i) {
            if (i > 0) result += ", ";
            if (refParams.count(i)) result += "ref ";
            result += parameters[i].first + " " + parameters[i].second;
        }
        result += ")";
        if (returnBorrowsParam >= 0) {
            result += " borrows " + std::to_string(returnBorrowsParam);
        }
        result += ";";
        return result;
    }
};

class MainFunction : public Statement {
public:
    std::string returnType;
    std::vector<StatementPtr> body;

    MainFunction(const std::string& type, std::vector<StatementPtr> statements)
        : returnType(type), body(std::move(statements)) {}

    std::string toString() const override {
        std::string result = returnType + " main() {\n";
        for (const auto& stmt : body) {
            result += "    " + stmt->toString() + "\n";
        }
        result += "}";
        return result;
    }
};

class NewExpression : public Expression {
public:
    std::string className;
    std::vector<ExpressionPtr> arguments;

    NewExpression(const std::string& cls, std::vector<ExpressionPtr> args)
        : className(cls), arguments(std::move(args)) {}

    std::string toString() const override {
        std::string result = "new " + className + "(";
        for (size_t i = 0; i < arguments.size(); ++i) {
            if (i > 0) result += ", ";
            result += arguments[i]->toString();
        }
        result += ")";
        return result;
    }
};

// A `closure IFace [captures] (params) { body }` expression. TRANSIENT: this
// node never survives past ASTBuilder -- ASTBuilder::desugarClosures (run at
// the end of buildProgram, before validateInheritance) replaces every
// occurrence with a NewExpression naming a freshly synthesized ClassDeclaration
// (derived from `interfaceName`, one field+ctor-param per capture, one
// overriding method holding `body`) inserted just before the top-level
// statement that used it. No checker or the emitter ever sees this type --
// it exists purely to carry the closure's literal syntax from the CST to the
// desugaring pass. This is the first Expression subclass to hold a statement
// body (mirrors MethodDeclaration's body, not any other Expression), which is
// exactly why it cannot be treated like an ordinary expression by anything
// downstream and must be fully desugared away before EscapeHatchChecker (or
// any other checker) ever walks the program.
class ClosureExpression : public Expression {
public:
    std::string interfaceName;
    std::vector<std::string> captures;
    std::vector<std::pair<std::string, std::string>> parameters;
    std::vector<StatementPtr> body;

    ClosureExpression(const std::string& iface, std::vector<std::string> caps,
                      std::vector<std::pair<std::string, std::string>> params,
                      std::vector<StatementPtr> stmts)
        : interfaceName(iface), captures(std::move(caps)),
          parameters(std::move(params)), body(std::move(stmts)) {}

    std::string toString() const override {
        std::string result = "closure " + interfaceName + " [";
        for (size_t i = 0; i < captures.size(); ++i) {
            if (i > 0) result += ", ";
            result += captures[i];
        }
        result += "] (";
        for (size_t i = 0; i < parameters.size(); ++i) {
            if (i > 0) result += ", ";
            result += parameters[i].first + " " + parameters[i].second;
        }
        result += ") {\n";
        for (const auto& stmt : body) {
            result += "    " + stmt->toString() + "\n";
        }
        result += "}";
        return result;
    }
};

class MemberAccess : public Expression {
public:
    ExpressionPtr object;
    std::string member;

    MemberAccess(ExpressionPtr obj, const std::string& memberName)
        : object(std::move(obj)), member(memberName) {}

    std::string toString() const override {
        return object->toString() + "->" + member;
    }
};

// Subscript access `v[i]` (Phase 2.6 slice 2: std::vector<T> element access).
// Mirrors MemberAccess, but the index is a real sub-expression (not a bare name --
// it can be an identifier, a literal, or an arithmetic expression), so unlike
// MemberAccess::member it must be an ExpressionPtr, not a std::string.
class SubscriptExpression : public Expression {
public:
    ExpressionPtr object;
    ExpressionPtr index;

    SubscriptExpression(ExpressionPtr obj, ExpressionPtr idx)
        : object(std::move(obj)), index(std::move(idx)) {}

    std::string toString() const override {
        return object->toString() + "[" + index->toString() + "]";
    }
};

class IfStatement : public Statement {
public:
    ExpressionPtr condition;
    std::vector<StatementPtr> body;
    // `else` branch. For `else if`, this holds a single nested IfStatement; for a
    // plain `else { ... }`, it holds that block's statements. Empty if no else.
    std::vector<StatementPtr> elseBody;

    IfStatement(ExpressionPtr cond, std::vector<StatementPtr> statements)
        : condition(std::move(cond)), body(std::move(statements)) {}

    std::string toString() const override {
        std::string result = "if (" + condition->toString() + ") {\n";
        for (const auto& stmt : body) {
            result += "    " + stmt->toString() + "\n";
        }
        result += "}";
        if (!elseBody.empty()) {
            result += " else {\n";
            for (const auto& stmt : elseBody) {
                result += "    " + stmt->toString() + "\n";
            }
            result += "}";
        }
        return result;
    }
};

// `break;` / `continue;` -- target the innermost enclosing loop (no labels, no switch).
class BreakStatement : public Statement {
public:
    std::string toString() const override { return "break;"; }
};

class ContinueStatement : public Statement {
public:
    std::string toString() const override { return "continue;"; }
};

class WhileStatement : public Statement {
public:
    ExpressionPtr condition;
    std::vector<StatementPtr> body;

    WhileStatement(ExpressionPtr cond, std::vector<StatementPtr> statements)
        : condition(std::move(cond)), body(std::move(statements)) {}

    std::string toString() const override {
        std::string result = "while (" + condition->toString() + ") {\n";
        for (const auto& stmt : body) {
            result += "    " + stmt->toString() + "\n";
        }
        result += "}";
        return result;
    }
};

// Range-based `for` only: `for (Type name : container) { ... }`. Matches native
// C++ range-for 1:1, so the emitter needs no translation logic. `loopVarType`/
// `loopVarName` are the per-iteration binding; `container` is the collection
// expression being iterated.
class ForStatement : public Statement {
public:
    std::string loopVarType;
    std::string loopVarName;
    ExpressionPtr container;
    std::vector<StatementPtr> body;

    ForStatement(const std::string& varType, const std::string& varName,
                ExpressionPtr containerExpr, std::vector<StatementPtr> statements)
        : loopVarType(varType), loopVarName(varName),
          container(std::move(containerExpr)), body(std::move(statements)) {}

    std::string toString() const override {
        std::string result = "for (" + loopVarType + " " + loopVarName + " : " +
                             container->toString() + ") {\n";
        for (const auto& stmt : body) {
            result += "    " + stmt->toString() + "\n";
        }
        result += "}";
        return result;
    }
};

// `try { ... } catch (Type name) { ... }` -- single catch clause only. DanglingChecker
// rejects any catchType other than `string` this slice (see its TryStatement branch),
// since `throw` itself only ever produces a string; the AST node stays general
// (catchType is a plain string, not hardcoded to "string") the same way
// ContainerDeclaration's type field stays general even though this slice only
// validates a narrow subset of what it could hold.
class TryStatement : public Statement {
public:
    std::vector<StatementPtr> body;
    std::string catchType;
    std::string catchIdentifier;
    std::vector<StatementPtr> catchBody;

    TryStatement(std::vector<StatementPtr> tryBody, const std::string& cType,
                const std::string& cIdentifier, std::vector<StatementPtr> cBody)
        : body(std::move(tryBody)), catchType(cType), catchIdentifier(cIdentifier),
          catchBody(std::move(cBody)) {}

    std::string toString() const override {
        std::string result = "try {\n";
        for (const auto& stmt : body) {
            result += "    " + stmt->toString() + "\n";
        }
        result += "} catch (" + catchType + " " + catchIdentifier + ") {\n";
        for (const auto& stmt : catchBody) {
            result += "    " + stmt->toString() + "\n";
        }
        result += "}";
        return result;
    }
};

class IncludeStatement : public Statement {
public:
    std::string includeFile;

    explicit IncludeStatement(const std::string& file)
        : includeFile(file) {}

    std::string toString() const override {
        return "#include " + includeFile;
    }
};

class ClassDeclaration : public Statement {
public:
    std::string name;
    // Single base class (`class D : public B`), or empty if this class has no base.
    // Multiple inheritance is deliberately not representable here -- the grammar
    // admits exactly one base, so "which base?" is never ambiguous for the
    // member-flattening pass every analysis module runs at collection time.
    std::string baseClass;
    // Enclosing namespace name, or empty if this class was declared at the top
    // level. `namespace X { class C {...} }` is DESUGARED by the AST builder into
    // a top-level ClassDeclaration with namespaceName="X" -- every analysis module
    // keeps seeing an ordinary top-level ClassDeclaration; only AstEmitter reads
    // this field, to re-wrap the class in `namespace X { ... }` on output.
    std::string namespaceName;
    std::vector<StatementPtr> members;
    // True iff declared with `struct` instead of `class` -- an explicit,
    // user-opted-in plain VALUE type: stored bare/unwrapped everywhere
    // (locals, fields, parameters, vector<T> elements), never smart-pointer-
    // wrapped. See SmartPointerAnalyzer::classifyValueTypeClasses, which folds
    // this into the SAME valueTypeClasses_ set the pre-existing
    // operator*/operator++/operator!= (Iterator-shaped) structural inference
    // already populates -- every existing isValueTypeClass-gated call site
    // picks up a `struct` class for free. Deliberately NOT inferred
    // structurally (see class_declaration's own grammar.js comment for why).
    bool isValueType = false;
    // True for the class closure_desugar synthesizes for a `spawn(closure ...)` job. Its object-typed
    // captures are MOVED into the new thread (stored as unique owners), unlike an ordinary closure's
    // shared captures -- see DanglingChecker::checkSpawnCall for the rules that make that safe.
    bool isSpawnJob = false;

    ClassDeclaration(const std::string& className, std::vector<StatementPtr> classMembers)
        : name(className), members(std::move(classMembers)) {}

    std::string toString() const override {
        std::string result = "class " + name;
        if (!baseClass.empty()) result += " : public " + baseClass;
        result += " {\npublic:\n";
        for (const auto& member : members) {
            result += "    " + member->toString() + "\n";
        }
        result += "};";
        return result;
    }
};

// An unscoped enum: `enum Name { A, B, C };`. Deliberately unscoped -- enumerators
// implicitly convert to `int`, matching how CeeLing users store an enum tag in a
// plain `int` field (Java-like: no `static_cast` escape hatch exists to convert
// an `enum class` value, and CeeLing forbids casts entirely -- see CLAUDE.md hatch
// #13). Not registered as a class type: DanglingChecker::isObjectType only matches
// classNames_/vector/map, so an enum-typed value is automatically a plain value
// (like int/bool/string), participating in zero move/observer tracking.
class EnumDeclaration : public Statement {
public:
    std::string name;
    std::vector<std::string> enumerators;
    // Enclosing namespace name, or empty -- same desugaring as ClassDeclaration's
    // namespaceName, and for the same reason (enumerators must "leak into" the
    // namespace so `NodeType::Scalar` resolves, matching real C++ unscoped-enum
    // scoping rules).
    std::string namespaceName;

    EnumDeclaration(const std::string& enumName, std::vector<std::string> values)
        : name(enumName), enumerators(std::move(values)) {}

    std::string toString() const override {
        std::string result = "enum " + name + " { ";
        for (size_t i = 0; i < enumerators.size(); ++i) {
            if (i > 0) result += ", ";
            result += enumerators[i];
        }
        result += " };";
        return result;
    }
};

class ConstructorDeclaration : public Statement {
public:
    std::string name;
    std::vector<std::pair<std::string, std::string>> parameters;
    std::vector<StatementPtr> body;

    ConstructorDeclaration(const std::string& ctorName,
                          std::vector<std::pair<std::string, std::string>> params,
                          std::vector<StatementPtr> statements)
        : name(ctorName), parameters(std::move(params)), body(std::move(statements)) {}

    std::string toString() const override {
        std::string result = name + "(";
        for (size_t i = 0; i < parameters.size(); ++i) {
            if (i > 0) result += ", ";
            result += parameters[i].first + " " + parameters[i].second;
        }
        result += ") {\n";
        for (const auto& stmt : body) {
            result += "    " + stmt->toString() + "\n";
        }
        result += "}";
        return result;
    }
};

class MethodDeclaration : public Statement {
public:
    std::string returnType;
    std::string name;
    std::vector<std::pair<std::string, std::string>> parameters;
    std::vector<StatementPtr> body;
    bool isConst = false;  // true if declared with a trailing `const` qualifier
    // Polymorphic dispatch qualifiers, carried straight through to the generated
    // C++ (CeeLing reuses the C++ compiler's vtable rather than building its own).
    bool isVirtual = false;    // `virtual` before the return type
    bool isOverride = false;   // `override` after the parameter list
    // Return-type-generic method support (narrowly scoped -- see CLAUDE.md/TODO.md
    // and Chapter 18's implementation notes for exactly what this does and does not
    // cover). A method is at most ONE of: ordinary (both empty), a PRIMARY generic
    // (`typeParameter` set, e.g. "T" for `<T> T as()`), or a SPECIALIZATION
    // (`specializationArg` set, e.g. "int" for `int as<int>()`). Never both set.
    std::string typeParameter;
    std::string specializationArg;
    // True iff this MethodDeclaration was parsed from an
    // external_method_specialization (yaml-cpp's convert<T> customization-
    // point shape: `ReturnType ClassName::methodName<Type>(...) {...}` at
    // TOP LEVEL, specializing a primary generic method already declared
    // INSIDE ClassName's own class body, possibly in a different .clg file
    // reached via #include). ASTBuilder splices this MethodDeclaration
    // directly into its target ClassDeclaration's `members` vector right
    // after parsing (see spliceExternalMethodSpecializations), so every
    // downstream pass that already iterates `members` looking for
    // MethodDeclarations sees it automatically, with NO changes needed --
    // this flag exists only so AstEmitter knows to emit its definition at
    // NAMESPACE scope (the real C++ rule for member-template
    // specializations) instead of inside the class body, since that's the
    // one place source position still matters after the splice.
    bool isExternalSpecialization = false;

    MethodDeclaration(const std::string& retType,
                     const std::string& methodName,
                     std::vector<std::pair<std::string, std::string>> params,
                     std::vector<StatementPtr> statements)
        : returnType(retType), name(methodName), parameters(std::move(params)), body(std::move(statements)) {}

    std::string toString() const override {
        std::string result;
        if (isVirtual) result += "virtual ";
        result += returnType + " " + name + "(";
        for (size_t i = 0; i < parameters.size(); ++i) {
            if (i > 0) result += ", ";
            result += parameters[i].first + " " + parameters[i].second;
        }
        result += ")";
        if (isConst) result += " const";
        if (isOverride) result += " override";
        result += " {\n";
        for (const auto& stmt : body) {
            result += "    " + stmt->toString() + "\n";
        }
        result += "}";
        return result;
    }
};

// Emission-only placeholder for an external_method_specialization
// (`ReturnType ClassName::method<Type>(...) {...}`, yaml-cpp's convert<T>
// shape). The real MethodDeclaration is spliced into its target
// ClassDeclaration's `members` (see ASTBuilder::buildProgram) so every
// checker that walks `members` sees it for free -- but that splice ties the
// method's AST *ownership* to the target class, which is wrong for
// *emission* position: the specialization's body/signature can reference a
// type declared AFTER the target class (or, in the #include case, the
// target class is merged in first while the type the specialization needs
// is declared later, in the consuming file's own statements). CeeLing has
// no forward declarations, so the only source position guaranteed to be
// safe is the specialization's own original top-level position -- ordinary
// source-order rules already guarantee anything it references was declared
// before it there. This marker is a non-owning placeholder (raw pointer,
// the MethodDeclaration is owned by target->members) inserted into
// Program::statements at that original position purely so AstEmitter's
// top-level walk emits declaration+definition there instead of next to the
// target class. Every other pass (checkers) does not recognize this type
// and simply skips it, exactly as they already skip IncludeStatement etc.
class ExternalSpecializationMarker : public Statement {
public:
    std::string targetClassName;
    MethodDeclaration* method;  // non-owning; owned by targetClass->members

    ExternalSpecializationMarker(std::string targetClass, MethodDeclaration* m)
        : targetClassName(std::move(targetClass)), method(m) {}

    std::string toString() const override {
        return "external_specialization_marker(" + targetClassName + "::" + method->name + ")";
    }
};

class Program : public ASTNode {
public:
    std::vector<StatementPtr> statements;

    // external_method_specialization entries (targetClassName, MethodDeclaration)
    // whose target class was NOT found among this Program's OWN statements at
    // build time -- only possible when the target arrives later via
    // mergeExternalDeclarations (see main.cpp), which runs strictly AFTER
    // ASTBuilder::buildProgram returns. ASTBuilder's own splice pass resolves
    // everything it can immediately (the common, same-file case); anything
    // left here must be re-attempted by the caller (main.cpp) after merging,
    // via ASTBuilder::spliceExternalSpecializations, before giving up with
    // the unknown-target error.
    std::vector<std::pair<std::string, StatementPtr>> unresolvedExternalSpecializations;

    void addStatement(StatementPtr stmt) {
        statements.push_back(std::move(stmt));
    }

    std::string toString() const override {
        std::string result;
        for (const auto& stmt : statements) {
            result += stmt->toString() + "\n";
        }
        return result;
    }
};

using ProgramPtr = std::unique_ptr<Program>;

} // namespace cling