#pragma once

#include "../cst_to_ast/ast.h"
#include "smart_pointer_analyzer.h"
#include <map>
#include <set>
#include <sstream>
#include <string>

namespace cling {

// AST-driven C++ generator (the new architecture replacing the regex text pipeline).
//
// It walks the AST and emits C++ directly, so language constructs are lowered
// structurally rather than by fragile text substitution:
//   * object variables      -> the smart-pointer type chosen by SmartPointerAnalyzer
//   * `new T(...)`           -> std::make_unique / std::make_shared<T>(...)
//   * `ref T x = y;`         -> `T& x = *y;`   (observer = native reference)
//   * `std::cout << e;`      -> emitted verbatim (native stream output, no `print`)
//   * member access `a.b`    -> `a->b` for smart-pointer objects
//
// EXPERIMENTAL / opt-in via the `--emit-ast` CLI flag; the regex-based CppGenerator
// remains the default. Introduced incrementally.
//
// Known gaps (verified against fixtures; each is a follow-up increment):
//   * custom `#include` lines beyond std headers are passed through, but arbitrary
//     `a + b + c` (3+ operand chains) are a pre-existing grammar limit, not emitter.
// Done: functions/classes/main, if/else, comparisons, methods with `this->` field
//   access, nested member access and method calls (`a.b->c(...)`), member-target
//   assignment (`a.b = x;`), ref->reference, native `std::cout << ...` stream output
//   (`::` scope resolution + `<<` chaining), new->make_*, smart-pointer types,
//   auto-`std::move` of object arguments (safe because the dangling checker forbids
//   use-after-move), and return-value move for object-returning functions (return type
//   lowered to a smart pointer; the owning local is std::move'd out). Programs without
//   the gaps above emit COMPILABLE C++ (verified with g++).
class AstEmitter {
public:
    explicit AstEmitter(const SmartPointerAnalyzer& analyzer) : analyzer_(analyzer) {}

    // Emit a full translation unit for the program.
    std::string emit(const Program& program);

private:
    void collectClassNames(const Program& program);
    bool isClassType(const std::string& type) const;

    // Declared parameter types for every free function / method, so a call argument
    // that is a bare `new T(...)` (no target variable of its own to key off) can look
    // up what smart-pointer type the RECEIVING parameter needs instead of defaulting
    // blindly (see emitMovedArg / the FunctionCall argument loop).
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> functionParams_;
    std::map<std::string, std::map<std::string, std::vector<std::pair<std::string, std::string>>>> classMethodParams_;
    // For a resolvable call (`recv.method(...)` or a plain `f(...)`), the (class,
    // name) of parameter `argIndex` -- needed together, since the analyzer's pointer
    // decisions are keyed by (className, variableName), not class alone. Returns
    // ("", "") if unknown/not an object parameter.
    std::pair<std::string, std::string> calleeParam(const FunctionCall& call, size_t argIndex) const;

    void emitTopLevel(const Statement* stmt, std::ostringstream& out);
    void emitClass(const ClassDeclaration& cls, std::ostringstream& out);
    // `ownerClass` is the enclosing class for methods/constructors (empty for free
    // functions); inside it, bare field names are lowered to `this->field`.
    void emitFunction(const std::string& returnType, const std::string& name,
                      const std::vector<std::pair<std::string, std::string>>& params,
                      const std::vector<StatementPtr>& body, std::ostringstream& out,
                      const std::string& ownerClass = "", int baseIndent = 0,
                      bool isConst = false);
    void emitStatement(const Statement* stmt, std::ostringstream& out, int indent);

    // Is `name` a field of the class we are currently emitting a method for?
    bool isCurrentField(const std::string& name) const;
    // Declared type of field `name` in class `cls`, or empty if unknown.
    std::string fieldType(const std::string& cls, const std::string& name) const;
    // If `expr` evaluates to a smart-pointer-to-object (so member access uses `->`),
    // return the object's class name; otherwise "" (references/values use `.`).
    std::string exprObjectClass(const Expression* expr) const;

    // Expression emission.
    std::string emitExpression(const Expression* expr);
    // An argument in a call position. An object-typed local/parameter is std::move'd:
    // the dangling checker has already proven it is not used after this point, so the
    // move is always safe (and required, since object params take ownership). A bare
    // `new T(...)` argument has no variable of its own to key its pointer type off,
    // so `targetParam` (the receiving parameter's (class, name), from calleeParam)
    // tells it which factory (make_unique/make_shared) the callee actually needs.
    std::string emitMovedArg(const Expression* arg,
                             const std::pair<std::string, std::string>& targetParam = {});
    std::string smartPointerType(const std::string& className, const std::string& varName) const;
    std::string factoryFor(const std::string& className, const std::string& varName,
                           const std::vector<ExpressionPtr>& args);

    static std::string indentStr(int indent) { return std::string(indent * 4, ' '); }

    // How a variable is represented in the generated C++, so member access and moves
    // are lowered correctly.
    enum class VarKind {
        SmartPtr,   // std::unique_ptr/shared_ptr<T> -> access via `->`, movable
        Reference,  // ref observer, lowered to `T&`   -> access via `.`
        Value       // int/float/... or non-pointer    -> access via `.`
    };

    void noteVar(const std::string& name, VarKind kind) { varKinds_[name] = kind; }
    VarKind kindOf(const std::string& name) const;

    const SmartPointerAnalyzer& analyzer_;
    std::set<std::string> classNames_;
    std::map<std::string, VarKind> varKinds_;  // variable representation in current unit
    std::map<std::string, std::string> varClass_;  // smart-ptr local/param -> class name
    // Per-class field name -> declared type (for `this->` and nested `->` resolution).
    std::map<std::string, std::map<std::string, std::string>> classFields_;
    std::string currentClass_;  // class whose method is being emitted ("" = free fn)
};

} // namespace cling
