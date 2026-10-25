#include "closure_desugar.h"

#include "library_summary.h"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace cling {

namespace {

// One entry per lexical scope currently open (function/method body, plus one
// per nested if/while/for/try block) -- a captured name resolves by searching
// innermost-to-outermost, same shape as real block scoping. `type` is the
// bare declared-type string (as written in source); `isObserver` mirrors
// VariableDeclaration::isObserver so a captured `ref` name can be rejected
// (see desugarClosures' own doc comment: this is the most important check
// here).
struct ScopeEntry {
    std::string type;
    bool isObserver = false;
};

// A dedicated scope-type stack for THIS pass only -- confirmed via exploration
// that ASTBuilder has no symbol table at all (it is a syntax-directed
// translator) and SmartPointerAnalyzer's own symbolTypesByScope (a) runs
// AFTER EscapeHatchChecker, which would reject a live ClosureExpression
// before the analyzer ever sees it, and (b) skips every non-class type
// (`if (classNames.find(ptype) == classNames.end()) continue;`), which is
// exactly what a closure captures most (int/float/string). Neither existing
// table can serve this pass, so this is new, minimal, purpose-built state:
// captured names are the ONLY thing that ever needs to be looked up here,
// never a general "what type is this expression" query.
class ScopeStack {
public:
    void push() { scopes_.emplace_back(); }
    void pop() { scopes_.pop_back(); }

    void declare(const std::string& name, const std::string& type, bool isObserver) {
        scopes_.back()[name] = ScopeEntry{type, isObserver};
    }

    // Returns nullptr if `name` is not in scope anywhere on the stack.
    const ScopeEntry* find(const std::string& name) const {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto found = it->find(name);
            if (found != it->end()) return &found->second;
        }
        return nullptr;
    }

private:
    std::vector<std::map<std::string, ScopeEntry>> scopes_;
};

[[noreturn]] void reject(const std::string& code, const Expression* at, const std::string& msg,
                         const std::string& help) {
    std::ostringstream oss;
    oss << "error[semantic/" << code << "]: " << msg;
    if (at != nullptr && at->loc.line > 0) {
        oss << " (line " << at->loc.line << ", column " << at->loc.column << ")";
    }
    oss << ". " << help;
    throw std::runtime_error(oss.str());
}

// The interface class named by a closure must be a declared class with
// EXACTLY ONE virtual method -- that method is unambiguously the one the
// closure overrides, so the closure syntax itself never has to name it.
const MethodDeclaration* findSoleVirtualMethod(const ClassDeclaration& iface,
                                                const ClosureExpression& closure) {
    const MethodDeclaration* found = nullptr;
    int virtualCount = 0;
    for (const auto& member : iface.members) {
        if (auto method = dynamic_cast<const MethodDeclaration*>(member.get())) {
            if (method->isVirtual) {
                ++virtualCount;
                found = method;
            }
        }
    }
    if (virtualCount != 1) {
        reject("closure-interface-shape", &closure,
               "interface `" + iface.name + "` has " + std::to_string(virtualCount) +
                   " virtual method(s), not exactly one",
               "a closure's interface must declare EXACTLY ONE virtual method, so "
               "there is a single unambiguous method for the closure body to override");
    }
    return found;
}

// The closure's own parameter list must match the interface method's in
// arity and declared types -- a mismatch would make C++ treat the closure's
// `call` as an OVERLOAD instead of an OVERRIDE (AstEmitter emits `override`
// unconditionally, so C++ itself would reject a non-overriding override, but
// CeeLing's own message here is far clearer about which of the two
// signatures needs to change).
void checkSignatureMatches(const ClosureExpression& closure, const MethodDeclaration& iface,
                           const std::string& ifaceName) {
    if (closure.parameters.size() != iface.parameters.size()) {
        reject("closure-signature-mismatch", &closure,
               "closure has " + std::to_string(closure.parameters.size()) +
                   " parameter(s), interface `" + ifaceName + "::" + iface.name +
                   "` declares " + std::to_string(iface.parameters.size()),
               "a closure's parameter list must match its interface method's exactly");
    }
    for (size_t i = 0; i < closure.parameters.size(); ++i) {
        if (closure.parameters[i].first != iface.parameters[i].first) {
            reject("closure-signature-mismatch", &closure,
                   "closure parameter " + std::to_string(i + 1) + " has type `" +
                       closure.parameters[i].first + "`, interface `" + ifaceName +
                       "::" + iface.name + "` declares `" + iface.parameters[i].first + "`",
                   "a closure's parameter types must match its interface method's exactly");
        }
    }
}

int g_closureCounter = 0;

// Builds the synthesized class for one closure and returns it, along with the
// NewExpression that replaces the closure at its use site. NESTING: a closure
// literal may appear inside `closure.body` (another closure's own body). This
// is handled by walking `closure.body` with `walkBody` (forward-declared
// below, defined further down) using a scope seeded ONLY from the closure's
// OWN parameters -- never the outer scope this closure itself was found in,
// and never this closure's OWN capture list. `walkBody`'s existing recursion
// finds any nested ClosureExpression and desugars it via this same function,
// so depth is unbounded (3+ levels work identically, each one only ever
// seeing its immediate parent's parameters).
//
// This deliberately does NOT let an inner closure capture an outer closure's
// CAPTURE (as opposed to its parameter): an outer capture is a FIELD of the
// outer's synthesized class, reachable only through the outer's own `this` --
// and `this` cannot be captured (see the `closure-captures-this` rejection
// below: it is a borrow of the enclosing object, not an owned value). Since
// the inner closure's scope only ever contains the outer's PARAMETERS, an
// attempt to capture the outer's own capture falls through to the ordinary
// `closure-unknown-capture` rejection below with no new code -- the name
// simply isn't in the (deliberately narrow) inner scope.
struct DesugarResult {
    StatementPtr closureClass;
    ExpressionPtr replacement;
};

void walkBody(std::vector<StatementPtr>& body, ScopeStack& scope,
             const std::map<std::string, const ClassDeclaration*>& classesByName,
             std::vector<StatementPtr>& pendingClasses);

// Nesting depth of `spawn` bodies currently being walked: a `spawn` inside a `spawn` body is rejected
// (v1 -- no unbounded thread multiplication, and the inner body's captures would be a second hop the
// sendable-value check below was not written for).
int g_spawnDepth = 0;

// A value that may cross into another thread: copied on capture, so nothing the new thread can reach is
// reachable from the creating thread. Primitives (incl. string) and vectors of primitives. Everything else
// -- every class-typed object, whatever pointer kind it was inferred -- is refused: an object's graph can
// be shared with a live owner in the creating thread, and a closure capturing a shared_ptr would carry a
// live alias of the SAME object across (the hole this rule exists to close).
//
// A `struct` is a value type too -- copied, stored bare (never behind a smart pointer), and since a struct
// handle does not exist, its dynamic type is always its static type (no hidden subclass with extra shared
// fields, which is exactly what rules out moving a heap OBJECT: a base-typed handle can hold a subclass
// declared in another file, invisible to this translation unit). So a struct crosses by copy iff everything
// it is made of does: every field is itself a sendable value and so is its base struct. A class-typed field
// (stored as an owning handle) makes the struct unsendable.
bool isSendableValueType(const std::string& type,
                         const std::map<std::string, const ClassDeclaration*>& classesByName,
                         std::set<std::string>& visiting) {
    if (isPrimitiveType(type)) return true;
    if (isVectorType(type)) return isSendableValueType(vectorElementType(type), classesByName, visiting);
    auto it = classesByName.find(type);
    if (it == classesByName.end() || !it->second->isValueType) return false;
    if (!visiting.insert(type).second) return true;   // already being checked higher up: no new information
    const ClassDeclaration& cls = *it->second;
    bool ok = cls.baseClass.empty() || isSendableValueType(cls.baseClass, classesByName, visiting);
    for (const auto& member : cls.members) {
        if (!ok) break;
        if (auto field = dynamic_cast<const VariableDeclaration*>(member.get())) {
            ok = !field->isObserver && isSendableValueType(field->type, classesByName, visiting);
        } else if (auto container = dynamic_cast<const ContainerDeclaration*>(member.get())) {
            ok = isSendableValueType(container->type, classesByName, visiting);
        }
    }
    visiting.erase(type);
    return ok;
}

// Every name a `spawn` body reads or writes, and every name it declares itself. Used to refuse a body that
// reaches an enclosing local that was not captured: in C++ it would fail to compile anyway, but CeeLing's
// own message is clearer about the rule, and the rule should not depend on the C++ compile gate.
void collectNamesInExpr(const Expression* e, std::set<std::string>& used) {
    if (e == nullptr) return;
    if (auto id = dynamic_cast<const Identifier*>(e)) {
        used.insert(id->name);
    } else if (auto call = dynamic_cast<const FunctionCall*>(e)) {
        collectNamesInExpr(call->callee.get(), used);
        for (const auto& a : call->arguments) collectNamesInExpr(a.get(), used);
    } else if (auto nw = dynamic_cast<const NewExpression*>(e)) {
        for (const auto& a : nw->arguments) collectNamesInExpr(a.get(), used);
    } else if (auto bin = dynamic_cast<const BinaryExpression*>(e)) {
        collectNamesInExpr(bin->left.get(), used);
        collectNamesInExpr(bin->right.get(), used);
    } else if (auto un = dynamic_cast<const UnaryExpression*>(e)) {
        collectNamesInExpr(un->operand.get(), used);
    } else if (auto mem = dynamic_cast<const MemberAccess*>(e)) {
        collectNamesInExpr(mem->object.get(), used);
    } else if (auto sub = dynamic_cast<const SubscriptExpression*>(e)) {
        collectNamesInExpr(sub->object.get(), used);
        collectNamesInExpr(sub->index.get(), used);
    } else if (auto inner = dynamic_cast<const ClosureExpression*>(e)) {
        // a closure literal inside the body: its capture list reads names of THIS body; its own body is
        // checked on its own when it is desugared
        for (const auto& c : inner->captures) used.insert(c);
    }
}

void collectNamesInBody(const std::vector<StatementPtr>& body, std::set<std::string>& used,
                        std::set<std::string>& declared) {
    for (const auto& stmt : body) {
        if (auto d = dynamic_cast<const VariableDeclaration*>(stmt.get())) {
            declared.insert(d->identifier);
            collectNamesInExpr(d->expression.get(), used);
        } else if (auto c = dynamic_cast<const ContainerDeclaration*>(stmt.get())) {
            declared.insert(c->identifier);
        } else if (auto a = dynamic_cast<const AssignmentStatement*>(stmt.get())) {
            used.insert(a->variable);
            collectNamesInExpr(a->expression.get(), used);
            collectNamesInExpr(a->target.get(), used);
        } else if (auto es = dynamic_cast<const ExpressionStatement*>(stmt.get())) {
            collectNamesInExpr(es->expression.get(), used);
        } else if (auto r = dynamic_cast<const ReturnStatement*>(stmt.get())) {
            collectNamesInExpr(r->expression.get(), used);
        } else if (auto t = dynamic_cast<const ThrowStatement*>(stmt.get())) {
            collectNamesInExpr(t->expression.get(), used);
        } else if (auto i = dynamic_cast<const IfStatement*>(stmt.get())) {
            collectNamesInExpr(i->condition.get(), used);
            collectNamesInBody(i->body, used, declared);
            collectNamesInBody(i->elseBody, used, declared);
        } else if (auto w = dynamic_cast<const WhileStatement*>(stmt.get())) {
            collectNamesInExpr(w->condition.get(), used);
            collectNamesInBody(w->body, used, declared);
        } else if (auto f = dynamic_cast<const ForStatement*>(stmt.get())) {
            declared.insert(f->loopVarName);
            collectNamesInExpr(f->container.get(), used);
            collectNamesInBody(f->body, used, declared);
        } else if (auto tr = dynamic_cast<const TryStatement*>(stmt.get())) {
            declared.insert(tr->catchIdentifier);
            collectNamesInBody(tr->body, used, declared);
            collectNamesInBody(tr->catchBody, used, declared);
        }
    }
}

DesugarResult desugarOne(ClosureExpression& closure, const ScopeStack& scope,
                         const std::map<std::string, const ClassDeclaration*>& classesByName,
                         std::vector<StatementPtr>& pendingClasses, bool isSpawn = false) {
    auto ifaceIt = classesByName.find(closure.interfaceName);
    if (ifaceIt == classesByName.end()) {
        reject("closure-unknown-interface", &closure,
               "closure names interface `" + closure.interfaceName +
                   "`, which is not a class declared in this program",
               "a closure's interface must be an ordinary CeeLing class with exactly one "
               "virtual method -- CeeLing cannot desugar a closure against an interface it "
               "did not compile itself");
    }
    const ClassDeclaration& iface = *ifaceIt->second;
    const MethodDeclaration* ifaceMethod = findSoleVirtualMethod(iface, closure);
    checkSignatureMatches(closure, *ifaceMethod, iface.name);
    if (isSpawn && (ifaceMethod->returnType != "void" || !ifaceMethod->parameters.empty())) {
        reject("spawn-interface-shape", &closure,
               "`spawn` needs a closure whose interface method is `void run()` -- `" + iface.name + "::" +
                   ifaceMethod->name + "` is not",
               "a spawned job has no caller to return to and no arguments to receive; hand data in through the "
               "capture list and results out through an ext_fn such as app_return");
    }

    // Resolve every capture's type from the enclosing scope BEFORE building
    // anything -- reject on the first unresolvable/forbidden name rather than
    // partially constructing a broken class.
    std::vector<std::pair<std::string, std::string>> resolvedCaptures;  // (type, name)
    for (const auto& name : closure.captures) {
        // `[this]` cannot reach here anymore: `this` is now a reserved keyword
        // (grammar.js's `this_expression`), and `capture_list` still parses its
        // entries as plain `$.identifier` tokens -- `this` no longer lexes as
        // one, so `[this]` is a PARSE ERROR before this pass ever runs, not a
        // semantic rejection. See tests/safety/closure_captures_this_rejected.clg
        // for the fixture proving this (still correctly rejected, just earlier).
        const ScopeEntry* entry = scope.find(name);
        if (entry == nullptr) {
            reject("closure-unknown-capture", &closure,
                   "closure captures `" + name + "`, which is not in scope here",
                   "every name in a closure's capture list must be a local variable or "
                   "parameter already declared before the closure expression -- if this "
                   "closure is NESTED inside another closure's body, note that it can only "
                   "capture its immediately enclosing closure's own PARAMETERS, never that "
                   "closure's own CAPTURES (which are fields reachable only through the "
                   "enclosing closure's `this`, and closures cannot capture `this`)");
        }
        if (entry->isObserver) {
            reject("closure-captures-observer", &closure,
                   "closure captures `" + name + "`, a `ref` observer",
                   "a `ref` observer is a non-owning reference into its owner's stack "
                   "frame; capturing it by value would copy that reference into a FIELD "
                   "that can outlive the observer's own scope, reintroducing exactly the "
                   "dangling-reference hole this language exists to prevent -- capture the "
                   "observer's OWNER instead, or restructure so the closure does not need it");
        }
        std::set<std::string> visiting;
        if (isSpawn && !isSendableValueType(entry->type, classesByName, visiting)) {
            reject("spawn-captures-object", &closure,
                   "`spawn` captures `" + name + "` of type `" + entry->type + "`, which is not a plain value",
                   "only plain values may cross into a new thread: primitives, string, std::vector of those, and "
                   "`struct`s made only of those. They are copied, so no memory is shared with the creating thread. "
                   "A class object (whatever pointer kind was inferred for it) may have another live owner here and "
                   "may be a subclass declared elsewhere, so it is refused -- make the data a `struct`, or "
                   "serialize it to a string first (e.g. printUnformatted(item)) and rebuild it inside the thread");
        }
        resolvedCaptures.emplace_back(entry->type, name);
    }


    if (isSpawn) {
        std::set<std::string> used, declared;
        collectNamesInBody(closure.body, used, declared);
        for (const auto& u : used) {
            if (declared.count(u) > 0) continue;
            if (std::find(closure.captures.begin(), closure.captures.end(), u) != closure.captures.end()) continue;
            bool isParam = false;
            for (const auto& p : closure.parameters) if (p.second == u) isParam = true;
            if (isParam || scope.find(u) == nullptr) continue;
            reject("spawn-uncaptured-name", &closure,
                   "the `spawn` body uses `" + u + "`, which belongs to the enclosing scope but is not in the capture list",
                   "a spawned job sees only what it captures (copies of plain values); add `" + u +
                       "` to the capture list if it is a plain value, or pass what you need another way");
        }
    }

    const std::string closureClassName = "__closure_" + std::to_string(g_closureCounter++);

    // Fields: one VariableDeclaration per capture, matching how an ordinary
    // hand-written field is represented (buildMemberVariableDeclaration
    // produces exactly this shape: type + identifier + null expression).
    std::vector<StatementPtr> members;
    for (const auto& [type, name] : resolvedCaptures) {
        members.push_back(std::make_unique<VariableDeclaration>(type, name, nullptr));
    }

    // Constructor: one parameter per capture, body assigns each into the
    // SAME-NAMED field. The parameter itself is named "p_<capture>" (not
    // "<capture>") -- confirmed via probe that CeeLing's AstEmitter has no
    // parameter/field shadowing concept at all (unlike real C++, where a
    // constructor parameter shadows a same-named field inside its own body):
    // an AssignmentStatement's RHS Identifier is resolved via the SAME
    // isCurrentField check as the LHS, so `total = total;` emitted BOTH sides
    // as `this->total`, producing a nonsensical self-initializing
    // `total(this->total)`. Every existing hand-written CeeLing constructor
    // in this codebase avoids the collision the same way (`age = a;`, never
    // `age = age;`) -- this is not a special case, it's the established
    // convention, just applied to a synthesized name instead of a
    // hand-chosen one. AstEmitter's member-initializer-list hoisting
    // (emitFunction's isConstructor path) still turns this leading run of
    // own-field assignments into a real `: count(p_count)` initializer list
    // automatically -- the parameter rename doesn't affect that at all.
    std::vector<std::pair<std::string, std::string>> ctorParams;
    std::vector<StatementPtr> ctorBody;
    for (const auto& [type, name] : resolvedCaptures) {
        const std::string paramName = "p_" + name;
        ctorParams.emplace_back(type, paramName);
        ctorBody.push_back(std::make_unique<AssignmentStatement>(
            name, std::make_unique<Identifier>(paramName)));
    }
    members.push_back(std::make_unique<ConstructorDeclaration>(
        closureClassName, std::move(ctorParams), std::move(ctorBody)));

    // Walk the closure's OWN body before moving it into the override method
    // below -- this is what makes nesting work. The inner scope is seeded
    // ONLY from `closure.parameters` (this closure's own `call`-method
    // parameter list), matching exactly how desugarClosures' top-level driver
    // seeds a FunctionDeclaration's scope from `fn->parameters` -- NOT from
    // `scope` (the scope this closure itself was FOUND in) and NOT from
    // `resolvedCaptures` (this closure's own captures becoming fields is an
    // orthogonal fact an inner closure must not see, per this function's own
    // doc comment above). `walkBody` will find and desugar any
    // ClosureExpression nested inside via `rewriteExpr`'s own dispatch,
    // recursively -- depth is unbounded, terminating only when a body
    // contains no further closure. `pendingClasses` is the SAME accumulator
    // threaded in from the caller, so an inner closure's synthesized class is
    // pushed onto it BEFORE this (outer) closure's own class below, giving
    // the correct inner-before-outer emission order with no extra bookkeeping
    // (CeeLing has no forward declarations, so the outer's `new
    // __closure_N(...)` reference to the inner class must appear after the
    // inner class's own declaration).
    ScopeStack innerScope;
    innerScope.push();
    for (const auto& [ptype, pname] : closure.parameters) {
        innerScope.declare(pname, ptype, false);
    }
    if (isSpawn) ++g_spawnDepth;
    walkBody(closure.body, innerScope, classesByName, pendingClasses);
    if (isSpawn) --g_spawnDepth;

    // The overriding method: same name/return type/parameters as the
    // interface's sole virtual method, body is the closure's own body (now
    // walked above -- any nested closure it contained has already been
    // replaced with a `new __closure_N(...)` NewExpression).
    auto overrideMethod = std::make_unique<MethodDeclaration>(
        ifaceMethod->returnType, ifaceMethod->name, closure.parameters, std::move(closure.body));
    overrideMethod->isOverride = true;
    members.push_back(std::move(overrideMethod));

    auto closureClass = std::make_unique<ClassDeclaration>(closureClassName, std::move(members));
    closureClass->baseClass = closure.interfaceName;
    closureClass->loc = closure.loc;

    std::vector<ExpressionPtr> ctorArgs;
    for (const auto& [type, name] : resolvedCaptures) {
        (void)type;
        ctorArgs.push_back(std::make_unique<Identifier>(name));
    }
    auto replacement = std::make_unique<NewExpression>(closureClassName, std::move(ctorArgs));
    replacement->loc = closure.loc;

    return DesugarResult{std::move(closureClass), std::move(replacement)};
}

// Recursively scans `expr` for a ClosureExpression and, if found, replaces it
// in place (via the owning ExpressionPtr&) with its desugared NewExpression,
// appending the synthesized class to `pendingClasses`. Must be called on
// every ExpressionPtr& that could directly hold or transitively contain a
// closure -- call arguments and binary/unary operands are the realistic
// cases; member/subscript objects are covered for completeness even though a
// closure there is not a real use case.
void rewriteExpr(ExpressionPtr& expr, const ScopeStack& scope,
                 const std::map<std::string, const ClassDeclaration*>& classesByName,
                 std::vector<StatementPtr>& pendingClasses) {
    if (!expr) return;

    if (auto closure = dynamic_cast<ClosureExpression*>(expr.get())) {
        DesugarResult result = desugarOne(*closure, scope, classesByName, pendingClasses);
        pendingClasses.push_back(std::move(result.closureClass));
        expr = std::move(result.replacement);
        return;
    }
    if (auto call = dynamic_cast<FunctionCall*>(expr.get())) {
        if (call->callee == nullptr && call->function_name == "cling::spawn") {
            reject("spawn-reserved", call, "`cling::spawn` is reserved for the compiler",
                   "write `spawn(closure Job [values] () { ... })` instead -- calling the runtime helper directly "
                   "would skip the check on what is captured");
        }
        if (call->callee == nullptr && call->function_name == "spawn") {
            auto closure = call->arguments.size() == 1
                ? dynamic_cast<ClosureExpression*>(call->arguments[0].get()) : nullptr;
            if (closure == nullptr) {
                reject("spawn-form", call, "`spawn` takes exactly one closure literal",
                       "write `spawn(closure Job [values] () { ... })` -- a named object cannot be spawned, "
                       "because only the capture list is checked for what crosses into the new thread");
            }
            if (g_spawnDepth > 0) {
                reject("spawn-nested", call, "`spawn` inside a `spawn` body",
                       "a spawned job may not start more threads (v1) -- start both from the creating thread");
            }
            DesugarResult result = desugarOne(*closure, scope, classesByName, pendingClasses, true);
            pendingClasses.push_back(std::move(result.closureClass));
            std::vector<ExpressionPtr> args;
            args.push_back(std::move(result.replacement));
            auto replaced = std::make_unique<FunctionCall>("cling::spawn", std::move(args));
            replaced->loc = call->loc;
            expr = std::move(replaced);
            return;
        }
        for (auto& arg : call->arguments) {
            rewriteExpr(arg, scope, classesByName, pendingClasses);
        }
        rewriteExpr(call->callee, scope, classesByName, pendingClasses);
        return;
    }
    if (auto neww = dynamic_cast<NewExpression*>(expr.get())) {
        if (neww->className.rfind("__closure_", 0) == 0) {
            reject("closure-reserved-name", neww, "`" + neww->className + "` is a compiler-generated name",
                   "a closure class cannot be constructed by hand -- write a `closure` expression");
        }
        // `new Holder(closure ...)` -- a closure passed as a CONSTRUCTOR
        // argument, not a plain function-call argument. Found as a real gap
        // via probe: without this case, a closure inside `new T(...)`'s
        // argument list was never visited at all, so the raw
        // ClosureExpression survived all the way to AstEmitter and was
        // echoed verbatim as text (emitExpression's "unknown node: best
        // effort" fallback, `return expr->toString();`) -- invalid C++, not
        // even a clean rejection.
        for (auto& arg : neww->arguments) {
            rewriteExpr(arg, scope, classesByName, pendingClasses);
        }
        return;
    }
    if (auto bin = dynamic_cast<BinaryExpression*>(expr.get())) {
        rewriteExpr(bin->left, scope, classesByName, pendingClasses);
        rewriteExpr(bin->right, scope, classesByName, pendingClasses);
        return;
    }
    if (auto un = dynamic_cast<UnaryExpression*>(expr.get())) {
        rewriteExpr(un->operand, scope, classesByName, pendingClasses);
        return;
    }
    if (auto mem = dynamic_cast<MemberAccess*>(expr.get())) {
        rewriteExpr(mem->object, scope, classesByName, pendingClasses);
        return;
    }
    if (auto sub = dynamic_cast<SubscriptExpression*>(expr.get())) {
        rewriteExpr(sub->object, scope, classesByName, pendingClasses);
        rewriteExpr(sub->index, scope, classesByName, pendingClasses);
        return;
    }
    // Identifier/Number/StringLiteral/CharLiteral/ScopedIdentifile/NewExpression's
    // own arguments (handled when NewExpression itself is built, never holds a
    // closure in practice) -- nothing to recurse into.
}

// Walks one body's statements, maintaining `scope` (pushed/popped by this
// function's own callers for the body's own block), rewriting every
// expression position, and recursing into nested if/while/for/try blocks
// (each gets its OWN pushed scope, matching real block-scoping -- a variable
// declared inside an `if` is not visible to a closure outside it). Declares
// each VariableDeclaration/ContainerDeclaration into `scope` as it's
// encountered, so a capture can only see names declared BEFORE the closure
// in the same top-to-bottom source order real scoping implies.
void walkBody(std::vector<StatementPtr>& body, ScopeStack& scope,
             const std::map<std::string, const ClassDeclaration*>& classesByName,
             std::vector<StatementPtr>& pendingClasses) {
    for (auto& stmt : body) {
        if (auto decl = dynamic_cast<VariableDeclaration*>(stmt.get())) {
            rewriteExpr(decl->expression, scope, classesByName, pendingClasses);
            scope.declare(decl->identifier, decl->type, decl->isObserver);
        } else if (auto cdecl = dynamic_cast<ContainerDeclaration*>(stmt.get())) {
            scope.declare(cdecl->identifier, cdecl->type, false);
        } else if (auto assign = dynamic_cast<AssignmentStatement*>(stmt.get())) {
            rewriteExpr(assign->expression, scope, classesByName, pendingClasses);
            rewriteExpr(assign->target, scope, classesByName, pendingClasses);
        } else if (auto exprStmt = dynamic_cast<ExpressionStatement*>(stmt.get())) {
            rewriteExpr(exprStmt->expression, scope, classesByName, pendingClasses);
        } else if (auto ret = dynamic_cast<ReturnStatement*>(stmt.get())) {
            rewriteExpr(ret->expression, scope, classesByName, pendingClasses);
        } else if (auto thr = dynamic_cast<ThrowStatement*>(stmt.get())) {
            rewriteExpr(thr->expression, scope, classesByName, pendingClasses);
        } else if (auto ifs = dynamic_cast<IfStatement*>(stmt.get())) {
            rewriteExpr(ifs->condition, scope, classesByName, pendingClasses);
            scope.push();
            walkBody(ifs->body, scope, classesByName, pendingClasses);
            scope.pop();
            scope.push();
            walkBody(ifs->elseBody, scope, classesByName, pendingClasses);
            scope.pop();
        } else if (auto whiles = dynamic_cast<WhileStatement*>(stmt.get())) {
            rewriteExpr(whiles->condition, scope, classesByName, pendingClasses);
            scope.push();
            walkBody(whiles->body, scope, classesByName, pendingClasses);
            scope.pop();
        } else if (auto fors = dynamic_cast<ForStatement*>(stmt.get())) {
            rewriteExpr(fors->container, scope, classesByName, pendingClasses);
            scope.push();
            scope.declare(fors->loopVarName, fors->loopVarType, false);
            walkBody(fors->body, scope, classesByName, pendingClasses);
            scope.pop();
        } else if (auto trys = dynamic_cast<TryStatement*>(stmt.get())) {
            scope.push();
            walkBody(trys->body, scope, classesByName, pendingClasses);
            scope.pop();
            scope.push();
            scope.declare(trys->catchIdentifier, trys->catchType, false);
            walkBody(trys->catchBody, scope, classesByName, pendingClasses);
            scope.pop();
        }
        // CommentStatement/IncludeStatement/nested declarations: nothing to walk.
    }
}

} // namespace

void desugarClosures(Program& program) {
    std::map<std::string, const ClassDeclaration*> classesByName;
    for (const auto& stmt : program.statements) {
        if (auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
            classesByName[cls->name] = cls;
        }
    }

    std::vector<StatementPtr> newStatements;
    newStatements.reserve(program.statements.size());

    for (auto& topLevel : program.statements) {
        std::vector<StatementPtr> pendingClasses;
        ScopeStack scope;
        scope.push();

        if (auto cls = dynamic_cast<ClassDeclaration*>(topLevel.get())) {
            // Fields are in scope (by bare name, matching implicit `this->field`
            // access already used elsewhere) for every method/constructor body.
            for (const auto& member : cls->members) {
                if (auto field = dynamic_cast<const VariableDeclaration*>(member.get())) {
                    scope.declare(field->identifier, field->type, false);
                }
            }
            for (auto& member : cls->members) {
                scope.push();
                if (auto ctor = dynamic_cast<ConstructorDeclaration*>(member.get())) {
                    for (const auto& [ptype, pname] : ctor->parameters) {
                        scope.declare(pname, ptype, false);
                    }
                    walkBody(ctor->body, scope, classesByName, pendingClasses);
                } else if (auto method = dynamic_cast<MethodDeclaration*>(member.get())) {
                    for (const auto& [ptype, pname] : method->parameters) {
                        scope.declare(pname, ptype, false);
                    }
                    walkBody(method->body, scope, classesByName, pendingClasses);
                }
                scope.pop();
            }
        } else if (auto fn = dynamic_cast<FunctionDeclaration*>(topLevel.get())) {
            for (const auto& [ptype, pname] : fn->parameters) {
                scope.declare(pname, ptype, false);
            }
            walkBody(fn->body, scope, classesByName, pendingClasses);
        } else if (auto main = dynamic_cast<MainFunction*>(topLevel.get())) {
            walkBody(main->body, scope, classesByName, pendingClasses);
        }

        for (auto& synthesized : pendingClasses) {
            newStatements.push_back(std::move(synthesized));
        }
        newStatements.push_back(std::move(topLevel));
    }

    program.statements = std::move(newStatements);
}

} // namespace cling
