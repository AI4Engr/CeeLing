#pragma once

#include "../cst_to_ast/ast.h"
#include "smart_pointer_analyzer.h"
#include <functional>
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

    // Bounds checks on `v[i]` / `s[i]` are ON by default (the safety contract's runtime half);
    // a project that has measured the cost and accepts the risk can turn them off
    // (`--no-bounds-checks`). Output without checks must never be described as bounds-safe.
    void setBoundsChecks(bool on) { boundsChecks_ = on; }

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
    // Every EXTERNAL class name (declared by a merged `#include "*.clh"`, not
    // by `program`'s own non-external statements) that appears in a field,
    // parameter, or return-type position across `program`'s own declarations
    // -- used by emitHeader to forward-declare (`class X;`) instead of
    // requiring the corresponding header to have already been fully
    // `#include`d. Only classes qualify (never value-type structs, which
    // need their full definition even for a declaration-only header, since a
    // struct field/return is embedded by value, not smart-pointer-wrapped).
    std::set<std::string> collectForwardDeclarableClasses(const Program& program) const;
    // "CEELING_DECL_" + uppercased class name (non-alnum -> '_') -- a PER-CLASS
    // include-guard-style symbol, independent of any FILE-level guard
    // (emitHeader's own `#ifndef <FILE>_H` wraps a whole header, potentially
    // several classes at once; this wraps exactly one class's own
    // declaration). Used by both emitClass (.cpp mode) and emitClassHeader
    // (.h mode) to wrap JUST the "class X { ... };" declaration block (never
    // the out-of-line method bodies, which must always compile regardless)
    // in `#ifndef GUARD / #define GUARD ... #endif` -- so whichever of the
    // two reaches a given class's declaration FIRST within one translation
    // unit "wins", and the other is correctly skipped instead of producing a
    // duplicate definition. See Chapter 18 Step 37's own follow-up for the
    // real bug this fixes (a genuinely circular `.clh` include pair, e.g.
    // context.clg <-> button.clh, could otherwise see the SAME class
    // declared twice -- once via context.cpp's own local declaration, once
    // via a transitively-included copy of context.h).
    static std::string perClassGuardName(const std::string& className);
    bool isClassType(const std::string& type) const;
    // A value-type class (see SmartPointerAnalyzer::isValueTypeClass) -- never
    // wrapped in a smart pointer, accessed via `.` not `->`, constructed as a
    // plain `ClassName(args)` call, never moved (copied instead). Checked
    // BEFORE the ordinary isClassType/smartPointerType wrapping path at every
    // site that currently assumes "class-typed means pointer-backed".
    bool isValueType(const std::string& type) const;
    // True if `cls` already declares its own zero-arg constructor -- guards the
    // base-class synthesized `T() = default;` injection (emitClass/
    // emitClassHeader) against duplicating one the user wrote themselves.
    bool hasUserDefinedDefaultConstructor(const ClassDeclaration& cls) const;
    // Names of `cls`'s fields that OWN (unique/shared, not weak) another object of `cls`'s own
    // type -- a `next`/`child` link. Empty for a class used as a base or a value type, which keep
    // their implicit destructor. Such a class gets a generated iterative destructor
    // (selfOwningDestructorBody): the implicit one frees a 1,000,000-node chain by recursing once
    // per node and overflows the stack.
    std::vector<std::string> selfOwningFields(const ClassDeclaration& cls) const;
    std::string selfOwningDestructorBody(const ClassDeclaration& cls,
                                         const std::vector<std::string>& fields) const;
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
    // Every operator= overload's SINGLE parameter type, per class (bare AND
    // namespace-qualified keys, matching every other collection here) --
    // built directly from each ClassDeclaration's raw member list in
    // collectClassNames, independent of the collapsed classMethodParams_
    // map. Needed because a class declaring BOTH `operator=(string)` and
    // `operator=(Node)` (yaml-cling's real Node does exactly this, RB18) has
    // two arity-1 operator= overloads that COLLIDE under
    // classMethodParams_'s overloadKey (arity-only disambiguation, confirmed
    // via probe -- the second declaration silently overwrote the first,
    // so classMethodParams_[cls].count("operator=") went from true to FALSE
    // once a second overload existed, wrongly falling through to a plain
    // shared_ptr::operator= for BOTH overloads' call sites, one of which --
    // the string RHS -- doesn't even compile against shared_ptr).
    std::map<std::string, std::set<std::string>> operatorAssignParamTypes_;
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

    // Does call argument `argIndex` need an EXPLICIT COPY instead of the
    // plain reference emitMovedArg would otherwise emit? Returns the
    // argument's own CeeLing container type string (e.g. "std::vector<int>")
    // if so (truthy via non-empty, doubling as the type to construct the
    // copy from), "" otherwise. True exactly when: the target parameter at
    // `argIndex` is classified read-only-container (so it lowers to
    // `const&`, meaning the callee sees the caller's live vector, not a
    // snapshot); the argument is NOT a caller local/by-value-parameter
    // (those can never be reached by the callee mid-call, since CeeLing has
    // no references to locals outside `ext_fn`); and the callee is not
    // proven heap-pure (see SmartPointerAnalyzer::isHeapPureFreeFunction/
    // isHeapPureMethod) -- i.e. it might mutate/replace a container or
    // object the field's OWN storage depends on during the call. `receiverClass`/
    // `methodName` are the SAME values already resolved at the call site
    // (empty receiverClass means a plain free-function call).
    std::string argNeedsExplicitCopyForReadOnlyContainerParam(const FunctionCall& call, size_t argIndex,
                                                                const std::string& receiverClass,
                                                                const std::string& methodName) const;

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
    // `isConstructor` enables member-initializer-list emission: a leading run of
    // this constructor's body statements that are plain assignments to THIS
    // class's own fields (not inherited ones -- see emitFunction's own comment)
    // is hoisted into a real C++ ` : field1(rhs1), ...` list instead of staying
    // as body assignments. This is what makes a value-type field with no
    // default constructor legal as another class's field (see
    // selectAssignmentRhs's doc comment for the bug this fixes). False for
    // every non-constructor call site; only emitClass's constructor loop passes
    // true.
    void emitFunction(const std::string& returnType, const std::string& name,
                      const std::vector<std::pair<std::string, std::string>>& params,
                      const std::vector<StatementPtr>& body, std::ostringstream& out,
                      const std::string& ownerClass = "", int baseIndent = 0,
                      bool isConst = false, bool isVirtual = false,
                      bool isOverride = false, const std::string& methodName = "",
                      bool isConstructor = false);
    void emitStatement(const Statement* stmt, std::ostringstream& out, int indent);

    // Shared RHS-selection logic for `target = expr;` (used by BOTH an ordinary
    // body assignment in emitStatement AND a hoisted member-initializer-list
    // entry in emitFunction) -- extracted so the two paths can never silently
    // drift apart (a future bugfix to one would otherwise need to be
    // remembered for the other). `ptrVar` is the name used to look up the
    // target's own pointer/value-type kind (the field/variable being
    // assigned); `memberTarget`/`subscriptTarget` mirror emitStatement's own
    // AssignmentStatement branch (a hoisted initializer-list entry always
    // passes false/false, since hoisting is scoped to plain own-field
    // targets). Returns the C++ text for the right-hand side only (no
    // `lhs = `/`;` or `field(...)` wrapper -- callers add whichever their
    // context needs).
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
    // Is `expr` a STREAM-INSERTION `<<` chain (as opposed to an integer shift)?
    // Both are the same `<<` BinaryExpression since CeeLing's grammar gives the
    // token one shared precedence tier (two tiers is an unresolvable GLR conflict
    // -- see grammar.js's shift tier comment), so the distinction is made here, by
    // walking LEFT down nested `<<` nodes to the chain's leftmost leaf and asking
    // whether that leaf is a stream: `std::cout`/`std::cerr`/`std::clog`, or an
    // `ostream`-typed local/param (see ostreamVars_).
    //
    // This drives GROUPING, and getting it wrong is silently wrong arithmetic, not
    // just ugly output: stream chains must emit flat and paren-free
    // (`std::cout << a << b`), while a shift must emit ordinary parenthesized
    // output, or the user's own parens are dropped and C++ regroups -- which is
    // exactly how `(1 << 4) + 1` used to emit `(1 << 4 + 1)` and evaluate to 32
    // instead of 17. It does NOT decide which C++ operator runs; that is left to
    // C++'s own overload resolution on the operand types.
    bool isStreamChain(const Expression* expr) const;
    // Is `expr` a call (`recv.method(...)`, receiver resolvable via exprObjectClass)
    // to a method with a `@ptr shared` return hint? A local declared from such a
    // call's result must ITSELF be shared_ptr (its C++ initializer is already a
    // shared_ptr; declaring the local unique_ptr does not compile) -- see the
    // VariableDeclaration branch in emitStatement, the one call site that needs
    // this alongside the ordinary smartPointerType(decl->type, ...) lookup.
    bool isSharedReturningCall(const Expression* expr) const;
    // True for a call to a method specialization whose return type is its
    // own concrete specialized type (e.g. `doc.as<Point>()` when `Point
    // Node::as<Point>()` is an external specialization) -- such a call's
    // C++ result is a bare value, not smart-pointer wrapped. See
    // currentSpecializationValueType_'s comment for the full rationale.
    bool isBareValueReturningSpecializationCall(const Expression* expr) const;

    // Expression emission.
    std::string emitExpression(const Expression* expr);
    // Address a `ref` observer binds to: `(x).get()` for a smart pointer, or the
    // bare name when `x` is itself an observer (already a raw `T*`).
    std::string observerAddress(const Expression* expr);
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

    // Set only while emitting an external specialization whose return type
    // equals its own concrete specialized type (e.g. `Point
    // Node::as<Point>()`) -- see isValueType's comment for the full
    // rationale. Empty ("") the rest of the time, so isValueType's ordinary
    // behavior for every other type/function is unaffected.
    std::string currentSpecializationValueType_;

    const SmartPointerAnalyzer& analyzer_;
    std::set<std::string> classNames_;
    // Populated alongside classNames_ in collectClassNames -- lets
    // emitExternalSpecializationMarker resolve an ExternalSpecializationMarker's
    // targetClassName back to the real ClassDeclaration (needed for its
    // namespaceName) without threading the whole Program through emitTopLevel.
    std::map<std::string, const ClassDeclaration*> classByName_;
    // Mirrors DanglingChecker::outParamIndex_ (see its own comment for the
    // full rationale) -- (className, methodName) -> index of the parameter
    // whose PRIMARY declares it as the type-parameter T (`<T> bool
    // decodeInto(T out)`). Lowered like a `ref` observer (VarKind::Reference:
    // raw pointer, `.get()` at the call site, `->` access in the body) rather
    // than moved/smart-pointer-wrapped -- both the primary's own `T out` and
    // every concrete specialization's `Point out` share this SAME index and
    // SAME non-owning lowering, since a specialization's parameter list must
    // structurally match its primary. NOTE: unrelated to CLAUDE.md's roadmap
    // `borrow<T>` item -- see outParamIndex_'s own comment in dangling_checker.h.
    std::map<std::string, size_t> outParamIndex_;
    static std::string outParamKey(const std::string& ownerClass, const std::string& methodName) {
        return ownerClass + "::" + methodName;
    }
    // Runtime helpers (see library_summary.h's isKnownRuntimeHelper) actually
    // called anywhere in this program -- computed once up front by
    // collectUsedRuntimeHelpers so emit()/emitHeader() know which shim
    // definitions (if any) to inject, without restructuring emission into a
    // buffer-then-decide-preamble shape.
    std::set<std::string> usedRuntimeHelpers_;
    void collectUsedRuntimeHelpers(const Program& program);
    // Set by collectUsedRuntimeHelpers alongside usedRuntimeHelpers_ (same
    // single pass, no separate scan): does the program call `.copyWithin(...)`
    // on ANY vector-typed receiver anywhere? Its generated codegen
    // (emitExpression's FunctionCall branch) lowers to std::memmove, so
    // `#include <cstring>` is only emitted when this is true -- matching
    // usedRuntimeHelpers_'s own "only include what's actually used" discipline.
    bool usesCopyWithin_ = false;
    // Does this file declare any std::weak_ptr field? If so, a read of that field goes through
    // lock(), and a member access THROUGH it is wrapped in cling::nn (null check), so the
    // helper (nullCheckHelperDefinition) and <cstdio>/<cstdlib> must be emitted.
    bool usesNullCheck_ = false;
    // Wrap `v[i]` / `s[i]` in cling::at (runtime bounds check). usesBoundsCheck_ is set by the
    // pre-scan when the program contains any subscript, so the helper is emitted only when needed.
    bool boundsChecks_ = true;
    bool usesBoundsCheck_ = false;
    // Two-phase class emission (see emit()): when set, emitClass writes only the class DEFINITION to
    // the output and stashes the out-of-line member bodies here, keyed by class name, to be written
    // after every class definition exists.
    bool deferClassBodies_ = false;
    std::map<std::string, std::string> deferredClassBodies_;
    // Set while emitting the left-hand side of an assignment, so the field being STORED to is not
    // rewritten into a lock() read.
    bool emittingAssignTarget_ = false;
    // True if `expr` reads a field that is lowered to std::weak_ptr: a bare field name of the
    // current class, or `obj.field` where `field` is declared weak in obj's class.
    bool isWeakFieldRead(const Expression* expr) const;
    // Emits an ExternalSpecializationMarker's declaration+definition at its
    // own top-level source position -- see the marker's comment in ast.h for
    // why this position (not "after the target class") is the only one
    // that's always safe.
    void emitExternalSpecializationMarker(const ExternalSpecializationMarker& marker,
                                           const ClassDeclaration& targetClass,
                                           std::ostringstream& out);
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
    // ext_fn-only: indices of the CURRENT signature's `ref` parameters (see
    // ExternFunctionDeclaration::refParams in ast.h for the full design
    // rationale). Consulted by buildFunctionSignature's primitive/vector
    // branch to lower index i to `const T&` (container) or `T&` (primitive)
    // instead of by-value. Populated and cleared at each of the three
    // ExternFunctionDeclaration emit sites, mirroring how varKinds_ etc. are
    // reset per-signature; empty for every non-ext_fn signature, so ordinary
    // functions/methods are completely unaffected.
    std::set<size_t> currentExternRefParams_;
    // Names of `ostream`-typed locals/params (see isOstreamType). Needed because
    // `<<` is overloaded between stream insertion and integer shift: a chain whose
    // leftmost leaf is one of these (or std::cout/cerr/clog) is a stream chain and
    // emits flat and paren-free, while every other `<<` emits ordinary grouped
    // output. Tracked as a name set rather than via varClass_/containerVarType_
    // because `ostream` is neither a class type nor a container -- the parameter
    // branch that recognizes it (isOstreamType, ast_emitter.cpp) previously
    // recorded only VarKind::Value and dropped the type name entirely.
    std::set<std::string> ostreamVars_;
    // Per-class field name -> declared type (for `this->` and nested `->` resolution).
    std::map<std::string, std::map<std::string, std::string>> classFields_;
    std::string currentClass_;  // class whose method is being emitted ("" = free fn)
    // The bare name of the function/method currently being emitted, and the
    // set of ITS OWN parameter names -- needed to gate the same-class
    // operator= handle-rebind routing (see the AssignmentStatement handling)
    // to PARAMETERS only, never an ordinary local. Set in emitFunction,
    // restored on return (mirrors currentClass_'s own save/restore).
    std::string currentFunctionName_;
    std::set<std::string> currentParamNames_;
    // True while emitting a method whose return was `@ptr shared`-hinted --
    // ReturnStatement emits a COPY (aliasing) instead of emitMovedArg's move in
    // this case. False (the default) preserves today's unique_ptr move-return
    // behavior unchanged for every method without a hint.
    bool currentMethodReturnsShared_ = false;
    // True while emitting a method/function whose return type is a value-type
    // class (see isValueType) -- ReturnStatement emits a plain, unwrapped return
    // (copy, never move; `new T(...)` becomes a bare constructor call, never a
    // make_unique/make_shared factory).
    bool currentMethodReturnsValueType_ = false;
};

} // namespace cling
