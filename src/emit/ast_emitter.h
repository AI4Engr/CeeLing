#pragma once

#include "syntax/ast.h"
#include "passes/smart_pointer_analyzer.h"
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>

namespace cling {

// Walks the AST and emits C++ directly. Main lowerings:
//   * object variables   -> the smart-pointer type chosen by SmartPointerAnalyzer
//   * `new T(...)`       -> std::make_unique / std::make_shared<T>(...)
//   * `ref T x = y;`     -> `T* x = (y).get();` (an observer is a raw pointer so it can be rebound; this is
//     compiler-generated code for a construct the dangling checker already accepted)
//   * `a.b`              -> `a->b` for smart-pointer objects and `ref` observers
//   * object arguments   -> std::move (safe: the checker rejects use after move)
//   * `std::cout << e;`  -> emitted verbatim
class AstEmitter {
public:
    explicit AstEmitter(const SmartPointerAnalyzer& analyzer) : analyzer_(analyzer) {}

    // Bounds checks on `v[i]` / `s[i]` are on by default (`--no-bounds-checks` turns them off).
    void setBoundsChecks(bool on) { boundsChecks_ = on; }
    // Runtime null checks on every dereference of an object handle (`--no-null-checks` turns them off).
    void setNullChecks(bool on) { nullChecks_ = on; }
    // Allocate shared objects from a per-thread free list instead of malloc (`--pool-shared`).
    void setPoolShared(bool on) { poolShared_ = on; }
    // Debug mode (`-g`): every statement is preceded by `#line N "path"`, and the generated check helpers are
    // marked artificial so a debugger does not step into them.
    void setDebugLines(bool on, const std::string& sourcePath) { debugLines_ = on; debugPath_ = sourcePath; }

    // Emit a full translation unit for the program.
    std::string emit(const Program& program);

    // Emit a declaration-only header: class bodies with member and method signatures, enums, and free-function
    // signatures; no `main`. The definitions stay in the .cpp that emit() produces. `guardName` is the include-guard token.
    std::string emitHeader(const Program& program, const std::string& guardName);

private:
    void collectClassNames(const Program& program);
    // External classes (declared by a merged `#include "*.clh"`) named in a field, parameter or return type.
    // emitHeader forward-declares them. Value-type structs are excluded: they are embedded by value and need
    // their full definition.
    std::set<std::string> collectForwardDeclarableClasses(const Program& program) const;
    // "CEELING_DECL_" + uppercased class name: a per-class guard wrapped around just the class declaration
    // (not the out-of-line bodies). Whichever of emitClass / emitClassHeader reaches a class first in a
    // translation unit wins, so a circular include pair cannot declare the class twice.
    static std::string perClassGuardName(const std::string& className);
    bool isClassType(const std::string& type) const;
    // A value-type class (see SmartPointerAnalyzer::isValueTypeClass): never wrapped in a smart pointer,
    // accessed with `.`, constructed as a plain call, copied rather than moved. Checked before the ordinary
    // class-type path.
    bool isValueType(const std::string& type) const;
    // True if `cls` declares its own zero-arg constructor (so the generated `T() = default;` is not duplicated).
    bool hasUserDefinedDefaultConstructor(const ClassDeclaration& cls) const;
    // Fields of `cls` that own another object of its own type (a `next`/`child` link). Empty for a base
    // class or value type. Such a class gets a generated iterative destructor, because the implicit one
    // recurses once per node and overflows the stack on a long chain.
    std::vector<std::string> selfOwningFields(const ClassDeclaration& cls) const;
    std::string selfOwningDestructorBody(const ClassDeclaration& cls,
                                         const std::vector<std::string>& fields) const;
    // Maps a CeeLing type to its C++ type. For a container of objects, the element wrapper (unique_ptr or
    // shared_ptr) comes from the analyzer's kind for `varName` (may be empty: unique_ptr). A container of
    // primitives and an unrecognized shape are returned unchanged.
    std::string cppTypeFor(const std::string& ceelingType, const std::string& varName = "") const;

    // Declared parameter types of every free function and method. A call argument that is a bare
    // `new T(...)` uses them to learn which smart-pointer type the receiving parameter needs.
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> functionParams_;
    std::map<std::string, std::map<std::string, std::vector<std::pair<std::string, std::string>>>> classMethodParams_;
    // Every operator= overload's single parameter type, per class. Kept apart from classMethodParams_
    // because that map is keyed by arity only, so two one-argument operator= overloads overwrite each other.
    std::map<std::string, std::set<std::string>> operatorAssignParamTypes_;
    // Declared return type of every free function and method, keyed like functionParams_. Lets
    // exprObjectClass choose `.` or `->` for `someCall().field`.
    std::map<std::string, std::string> functionReturnType_;
    std::map<std::string, std::map<std::string, std::string>> classMethodReturnType_;
    // For a resolvable call, the (class, name) of parameter `argIndex`; the analyzer's decisions are keyed
    // by both. ("", "") if unknown or not an object parameter.
    std::pair<std::string, std::string> calleeParam(const FunctionCall& call, size_t argIndex) const;
    // The declared type of parameter `argIndex` of a callee defined in this program (any type, not only a class), or "" if unknown.
    std::string calleeParamType(const FunctionCall& call, size_t argIndex) const;

    // A view-valued expression (a `string_view` local or parameter, or `substr` of one) where a `string` is
    // expected is emitted as `std::string(...)`, since C++ has no implicit conversion.
    bool isViewExpression(const Expression* e) const;
    std::string emitAsString(const Expression* e);

    bool stringArgCopyType(const FunctionCall& call, size_t argIndex, const std::string& callee) const;
    // Does call argument `argIndex` need an explicit copy? Returns the argument's container type (to build
    // the copy from), or "" if not. True when the target parameter is a read-only container (lowered to
    // const&), the argument is not a caller local or by-value parameter, and the callee is not heap-pure.
    // receiverClass is empty for a free-function call.
    std::string argNeedsExplicitCopyForReadOnlyContainerParam(const FunctionCall& call, size_t argIndex,
                                                                const std::string& receiverClass,
                                                                const std::string& methodName) const;

    void emitTopLevel(const Statement* stmt, std::ostringstream& out);
    // Header-mode counterparts of emitTopLevel/emitClass: same traversal, bodies replaced with `;`.
    void emitTopLevelHeader(const Statement* stmt, std::ostringstream& out);
    void emitClassHeader(const ClassDeclaration& cls, std::ostringstream& out);
    // The signature text shared by emitFunction and emitClassHeader, without body, semicolon or newline.
    // Has the same side effects on varKinds_/varClass_/containerVarType_ as emitFunction.
    std::string buildFunctionSignature(const std::string& returnType, const std::string& name,
                                       const std::vector<std::pair<std::string, std::string>>& params,
                                       const std::string& ownerClass, bool isConst,
                                       bool isVirtual, bool isOverride, const std::string& methodName);
    void emitClass(const ClassDeclaration& cls, std::ostringstream& out);
    // ownerClass is the enclosing class for methods and constructors ("" for free functions); inside it,
    // bare field names become `this->field`. `name` is the emitted name (may be qualified, e.g.
    // `Node::as<int>`); methodName is the bare name used to look up the return pointer kind. With
    // isConstructor, a leading run of assignments to the class's own fields becomes a member-initializer
    // list (this lets a value-type field without a default constructor be legal).
    void emitFunction(const std::string& returnType, const std::string& name,
                      const std::vector<std::pair<std::string, std::string>>& params,
                      const std::vector<StatementPtr>& body, std::ostringstream& out,
                      const std::string& ownerClass = "", int baseIndent = 0,
                      bool isConst = false, bool isVirtual = false,
                      bool isOverride = false, const std::string& methodName = "",
                      bool isConstructor = false);
    void emitStatement(const Statement* stmt, std::ostringstream& out, int indent);
    void emitStatementImpl(const Statement* stmt, std::ostringstream& out, int indent);

    // The right-hand side text for `target = expr;`, shared by an ordinary assignment and a hoisted
    // member-initializer entry so the two cannot drift apart. ptrVar names the target for the pointer-kind
    // lookup; memberTarget/subscriptTarget mirror the AssignmentStatement branch.
    std::string selectAssignmentRhs(const AssignmentStatement* assign, const std::string& ptrVar,
                                    bool memberTarget, bool subscriptTarget);

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
    // Is `expr` a stream-insertion `<<` chain rather than an integer shift? The grammar gives both the same
    // node, so this walks left to the chain's first operand and asks whether it is std::cout/cerr/clog or an
    // ostream-typed local (ostreamVars_). It decides grouping: a stream chain is emitted flat, a shift keeps
    // its parentheses (dropping them once turned `(1 << 4) + 1` into 32 instead of 17).
    bool isStreamChain(const Expression* expr) const;
    // Is `expr` a call to a method whose return kind is shared? A local initialised from it must be a
    // shared_ptr too, or the declaration does not compile.
    bool isSharedReturningCall(const Expression* expr) const;
    // True for a call to an external method specialization whose return type is its own concrete type
    // (`doc.as<Point>()`): the C++ result is a bare value (see currentSpecializationValueType_).
    bool isBareValueReturningSpecializationCall(const Expression* expr) const;

    // Expression emission.
    std::string emitExpression(const Expression* expr);
    // Address a `ref` observer binds to: `(x).get()` for a smart pointer, or the
    // bare name when `x` is itself an observer (already a raw `T*`).
    std::string observerAddress(const Expression* expr);
    std::string emitSpawnJobArg(const Expression* arg);
    // An argument in a call position. An object-typed local or parameter is std::move'd (the checker has
    // proven it is not used afterwards). A bare `new T(...)` argument uses targetParam, the receiving
    // parameter's (class, name), to choose make_unique or make_shared.
    std::string emitMovedArg(const Expression* arg,
                             const std::pair<std::string, std::string>& targetParam = {});
    std::string smartPointerType(const std::string& className, const std::string& varName) const;
    // className is the type being constructed; declaredType (when different, as in
    // `Animal a = new Dog();`) is the variable's declared type and decides the pointer kind.
    std::string factoryFor(const std::string& className, const std::string& varName,
                           const std::vector<ExpressionPtr>& args,
                           const std::string& declaredType = "");

    static std::string indentStr(int indent) { return std::string(indent * 4, ' '); }

    // How a variable is represented in the generated C++.
    enum class VarKind {
        SmartPtr,   // std::unique_ptr/shared_ptr<T> -> access via `->` (generated)
        Reference,  // ref observer, lowered to `T*`  -> access via `->` (generated);
                    // source-level access stays `.` either way (see exprObjectClass)
        Value       // int/float/... or non-pointer    -> access via `.`
    };

    void noteVar(const std::string& name, VarKind kind) { varKinds_[name] = kind; }
    VarKind kindOf(const std::string& name) const;

    // Set only while emitting an external specialization whose return type is its own concrete type
    // (`Point Node::as<Point>()`); empty otherwise.
    std::string currentSpecializationValueType_;

    const SmartPointerAnalyzer& analyzer_;
    std::set<std::string> classNames_;
    // Filled with classNames_; resolves an ExternalSpecializationMarker's target class (for its namespace).
    std::map<std::string, const ClassDeclaration*> classByName_;
    // Mirrors DanglingChecker::outParamIndex_: (className, methodName) -> index of the parameter declared as
    // the type parameter T (`<T> bool decodeInto(T out)`). Lowered like a `ref` observer (raw pointer,
    // `.get()` at the call, `->` in the body); the primary and every specialization share the index.
    std::map<std::string, size_t> outParamIndex_;
    static std::string outParamKey(const std::string& ownerClass, const std::string& methodName) {
        return ownerClass + "::" + methodName;
    }
    // Runtime helpers called anywhere in the program (see library_summary.h's isKnownRuntimeHelper),
    // computed up front so emit()/emitHeader() know which definitions to inject.
    std::set<std::string> usedRuntimeHelpers_;
    void collectUsedRuntimeHelpers(const Program& program);
    // Set by the same scan: `.copyWithin(...)` lowers to std::memmove, so <cstring> is included only when true.
    bool usesCopyWithin_ = false;
    bool usesVectorAlgorithms_ = false;           // v.sort() / reverse() / fill() / indexOf() / contains() somewhere (cling::vsort...)
    // Does the file declare a std::weak_ptr field? Then a read goes through lock(), a member access through it
    // is wrapped in cling::nn, and the helper plus <cstdio>/<cstdlib> must be emitted.
    bool usesNullCheck_ = false;
    // Wrap `v[i]` / `s[i]` in cling::at. usesBoundsCheck_ is set by the pre-scan when the program has any
    // subscript, so the helper is emitted only when needed.
    bool boundsChecks_ = true;
    bool nullChecks_ = true;
    bool poolShared_ = false;
    bool debugLines_ = false;
    std::string debugPath_;
    // `std::make_shared<T>(args)`, or its pooled form when --pool-shared is on.
    std::string sharedConstruct(const std::string& cls, const std::string& args) const;
    // `obj` is about to be dereferenced with `->`: wrap its text in cling::nn(...) when null checks
    // are on. `this` and an already-wrapped weak read need nothing.
    std::string checkedDeref(const Expression* obj, const std::string& text, int line) const;
    bool usesBoundsCheck_ = false;
    // Two-phase class emission (see emit()): when set, emitClass writes only the class definition and stashes
    // the out-of-line member bodies here, keyed by class name, to be written after every definition exists.
    bool deferClassBodies_ = false;
    std::map<std::string, std::string> deferredClassBodies_;
    // Set while emitting the left-hand side of an assignment, so a stored-to weak field is not turned into a lock() read.
    bool emittingAssignTarget_ = false;
    // True if `expr` reads a field lowered to std::weak_ptr: a bare field of the current class, or
    // `obj.field` where `field` is weak in obj's class.
    bool isWeakFieldRead(const Expression* expr) const;
    // Emits an ExternalSpecializationMarker's declaration and definition at its own top-level position
    // (see the marker in ast.h).
    void emitExternalSpecializationMarker(const ExternalSpecializationMarker& marker,
                                           const ClassDeclaration& targetClass,
                                           std::ostringstream& out);
    // Classes that another class inherits from. Each gets a generated `virtual ~Base() = default;`: deleting
    // a derived object through a base owner is undefined without one, and the language has no destructor syntax.
    std::set<std::string> baseClasses_;
    std::map<std::string, VarKind> varKinds_;  // variable representation in current unit
    std::map<std::string, std::string> varClass_;  // smart-ptr local/param -> class name
    // ref-observer local/param -> class name (raw-pointer lowering; see VarKind::Reference).
    std::map<std::string, std::string> observerClass_;
    // container local/param name -> declared CeeLing type (e.g. "std::vector<Node>").
    std::map<std::string, std::string> containerVarType_;
    // ext_fn only: indices of the current signature's `ref` parameters (see
    // ExternFunctionDeclaration::refParams in ast.h), lowered to `const T&` (container) or `T&` (primitive).
    // Reset per signature; empty for everything else.
    std::set<size_t> currentExternRefParams_;
    // ext_fn name -> indices of its `ref` parameters, for the CALL sites: such an argument is a borrow (`const&` / `&`), so the
    // caller's variable is passed as it is, never wrapped in std::move (DanglingChecker does not mark it moved either).
    std::map<std::string, std::set<size_t>> externRefParamsByName_;
    // Names of `ostream`-typed locals and parameters; the first operand of a stream chain (see isStreamChain).
    std::set<std::string> ostreamVars_;
    // Per-class field name -> declared type (for `this->` and nested `->` resolution).
    std::map<std::string, std::map<std::string, std::string>> classFields_;
    std::string currentClass_;  // class whose method is being emitted ("" = free fn)
    // Bare name of the function being emitted and its parameter names; the same-class operator= handle
    // rebind applies to parameters only, never to a local. Saved and restored like currentClass_.
    std::string currentFunctionName_;
    // The analyzer's scope string for the body being emitted (free function name, "main", or
    // "Class::method"); "" while emitting a class's field list.
    std::string currentScope_;
    static std::string scopeKeyFor(const std::string& ownerClass, const std::string& name,
                                   const std::string& methodName);
    std::set<std::string> currentParamNames_;
    // True while emitting a method whose return kind is shared: a return copies instead of moving.
    bool currentMethodReturnsShared_ = false;
    // True while emitting a function that returns a value-type class: a plain copy return, and
    // `new T(...)` becomes a bare constructor call.
    bool currentMethodReturnsValueType_ = false;
    std::string currentReturnType_;                 // declared return type of the function being emitted
    std::set<std::string> viewVars_;                // string_view locals and parameters of the function being emitted
};

} // namespace cling
