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
//   * `ref T x = y;`         -> `T* x = (y).get();`   (observer = raw pointer, so it
//     can be rebound -- `x = z;` -> `x = (z).get();` -- which a native `T&` cannot be.
//     This is compiler-internal codegen for a construct the dangling checker has
//     already proven safe, not a reopening of the source-level raw-pointer ban; see
//     Chapter 17's note on the distinction.
//   * `std::cout << e;`      -> emitted verbatim (native stream output, no `print`)
//   * member access `a.b`    -> `a->b` for smart-pointer objects AND `ref` observers
//     (source stays `.` either way; exprObjectClass drives the lowering)
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

    // Emit a declaration-only header for the program: class bodies with member/method
    // SIGNATURES (no bodies -- `;` instead), enums in full (already pure declarations),
    // and free-function signatures. No `main`. Lets a second .clg file `#include` this
    // header and reference the declared classes/functions, while the definitions stay
    // in the `.cpp` this same Program emits via emit() -- ordinary C++ separate
    // compilation (declaration in the header, one defining translation unit), not a
    // text-merge of the two source files. `guardName` becomes the `#ifndef`/`#define`
    // include-guard token (e.g. "YAML_NODE_H" for yaml_node.h).
    std::string emitHeader(const Program& program, const std::string& guardName);

private:
    void collectClassNames(const Program& program);
    bool isClassType(const std::string& type) const;
    // "std::vector<Node>" -> "std::vector<std::shared_ptr<Node>>" or
    // "std::vector<std::unique_ptr<Node>>", depending on `varName`'s scanned
    // element kind (SmartPointerAnalyzer::containerElementIsShared -- decided once
    // across every push_back call site targeting this variable across the whole
    // program, see Chapter 18). "std::vector<int>" -> unchanged (no point wrapping
    // a primitive). Returns `ceelingType` unchanged if it isn't a recognized
    // container shape. `varName` may be empty for shapes where no specific
    // variable identity applies (falls back to unique_ptr, the default).
    std::string cppTypeFor(const std::string& ceelingType, const std::string& varName = "") const;

    // Declared parameter types for every free function / method, so a call argument
    // that is a bare `new T(...)` (no target variable of its own to key off) can look
    // up what smart-pointer type the RECEIVING parameter needs instead of defaulting
    // blindly (see emitMovedArg / the FunctionCall argument loop).
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> functionParams_;
    std::map<std::string, std::map<std::string, std::vector<std::pair<std::string, std::string>>>> classMethodParams_;
    // Declared RETURN type for every free function / method, keyed identically to
    // functionParams_/classMethodParams_ (including overload/specialization keys)
    // -- needed so exprObjectClass can resolve `someCall().field`'s `.`-vs-`->`
    // choice: the object being accessed is a CALL RESULT, not a variable, so
    // there's no varClass_/classFields_ entry to look up; the call's own
    // declared return type tells us whether it's class-typed at all.
    std::map<std::string, std::string> functionReturnType_;
    std::map<std::string, std::map<std::string, std::string>> classMethodReturnType_;
    // For a resolvable call (`recv.method(...)` or a plain `f(...)`), the (class,
    // name) of parameter `argIndex` -- needed together, since the analyzer's pointer
    // decisions are keyed by (className, variableName), not class alone. Returns
    // ("", "") if unknown/not an object parameter.
    std::pair<std::string, std::string> calleeParam(const FunctionCall& call, size_t argIndex) const;

    void emitTopLevel(const Statement* stmt, std::ostringstream& out);
    // Header-mode counterparts of emitTopLevel/emitClass: same traversal and type
    // lowering, but member/method/function bodies are replaced with `;`.
    void emitTopLevelHeader(const Statement* stmt, std::ostringstream& out);
    void emitClassHeader(const ClassDeclaration& cls, std::ostringstream& out);
    // Emits just the signature line(s) used by both emitFunction (body form) and
    // emitClassHeader (`;`-terminated declaration form) -- returns the signature text
    // without trailing body/semicolon/newline so callers append what they need. Has
    // the SAME side effects on varKinds_/varClass_/containerVarType_ that emitFunction
    // has today (so a later emitStatement pass, if any, sees consistent state); a
    // declaration-only header never calls emitStatement, so those side effects are
    // harmless there.
    std::string buildFunctionSignature(const std::string& returnType, const std::string& name,
                                       const std::vector<std::pair<std::string, std::string>>& params,
                                       const std::string& ownerClass, bool isConst,
                                       bool isVirtual, bool isOverride, const std::string& methodName);
    void emitClass(const ClassDeclaration& cls, std::ostringstream& out);
    // `ownerClass` is the enclosing class for methods/constructors (empty for free
    // functions); inside it, bare field names are lowered to `this->field`.
    // `name` is what actually gets EMITTED (may be a qualified/specialized name,
    // e.g. `Node::as<int>`, for an out-of-class specialization); `methodName` is
    // the method's own BARE name, used ONLY to look up a `@ptr shared` return
    // hint (analyzer_.getMethodReturnPointerType) -- empty for constructors/free
    // functions/main, where no such hint is meaningful.
    void emitFunction(const std::string& returnType, const std::string& name,
                      const std::vector<std::pair<std::string, std::string>>& params,
                      const std::vector<StatementPtr>& body, std::ostringstream& out,
                      const std::string& ownerClass = "", int baseIndent = 0,
                      bool isConst = false, bool isVirtual = false,
                      bool isOverride = false, const std::string& methodName = "");
    void emitStatement(const Statement* stmt, std::ostringstream& out, int indent);

    // Is `name` a field of the class we are currently emitting a method for?
    bool isCurrentField(const std::string& name) const;
    // Declared type of field `name` in class `cls`, or empty if unknown.
    std::string fieldType(const std::string& cls, const std::string& name) const;
    // If `expr` evaluates to a smart-pointer-to-object (so member access uses `->`),
    // return the object's class name; otherwise "" (references/values use `.`).
    std::string exprObjectClass(const Expression* expr) const;
    // If `expr` is a bare identifier naming a container-typed local/param, return
    // its declared type string (e.g. "std::vector<Node>"); else "".
    std::string containerVarType(const Expression* expr) const;
    // Is `expr` a call (`recv.method(...)`, receiver resolvable via exprObjectClass)
    // to a method with a `@ptr shared` return hint? A local declared from such a
    // call's result must ITSELF be shared_ptr (its C++ initializer is already a
    // shared_ptr; declaring the local unique_ptr does not compile) -- see the
    // VariableDeclaration branch in emitStatement, the one call site that needs
    // this alongside the ordinary smartPointerType(decl->type, ...) lookup.
    bool isSharedReturningCall(const Expression* expr) const;

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
    // `className` is the type being CONSTRUCTED; `declaredType` (when it differs --
    // i.e. `Animal a = new Dog();`) is the receiving variable's own declared type,
    // which is what decides the smart-pointer KIND. Empty `declaredType` means "same
    // as className", the only possibility before inheritance existed.
    std::string factoryFor(const std::string& className, const std::string& varName,
                           const std::vector<ExpressionPtr>& args,
                           const std::string& declaredType = "");

    static std::string indentStr(int indent) { return std::string(indent * 4, ' '); }

    // How a variable is represented in the generated C++, so member access and moves
    // are lowered correctly.
    enum class VarKind {
        SmartPtr,   // std::unique_ptr/shared_ptr<T> -> access via `->` (generated)
        Reference,  // ref observer, lowered to `T*`  -> access via `->` (generated);
                    // source-level access stays `.` either way (see exprObjectClass)
        Value       // int/float/... or non-pointer    -> access via `.`
    };

    void noteVar(const std::string& name, VarKind kind) { varKinds_[name] = kind; }
    VarKind kindOf(const std::string& name) const;

    const SmartPointerAnalyzer& analyzer_;
    std::set<std::string> classNames_;
    // Classes that some other class inherits from. Any such class gets an
    // auto-generated `virtual ~Base() = default;` -- destroying a derived object
    // through a base-typed owner is undefined behavior without one, and CeeLing has
    // no destructor syntax, so the compiler emitting it is the ONLY way it can
    // exist. That makes this hole closed by construction rather than by a rule the
    // user has to remember.
    std::set<std::string> baseClasses_;
    std::map<std::string, VarKind> varKinds_;  // variable representation in current unit
    std::map<std::string, std::string> varClass_;  // smart-ptr local/param -> class name
    // ref-observer local/param -> class name (raw-pointer lowering; see VarKind::Reference).
    std::map<std::string, std::string> observerClass_;
    // container local/param name -> declared CeeLing type string (e.g.
    // "std::vector<Node>"), so a call-argument receiver can be checked against the
    // library allow-list's share-vs-move rules (see library_summary.h).
    std::map<std::string, std::string> containerVarType_;
    // Per-class field name -> declared type (for `this->` and nested `->` resolution).
    std::map<std::string, std::map<std::string, std::string>> classFields_;
    std::string currentClass_;  // class whose method is being emitted ("" = free fn)
    // True while emitting a method whose return was `@ptr shared`-hinted --
    // ReturnStatement emits a COPY (aliasing) instead of emitMovedArg's move in
    // this case. False (the default) preserves today's unique_ptr move-return
    // behavior unchanged for every method without a hint.
    bool currentMethodReturnsShared_ = false;
};

} // namespace cling
