#include "passes/closure_desugar.h"

#include "facts/library_summary.h"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace cling {

namespace {

// One entry per open lexical scope (a body, plus one per nested block). A captured name is looked up
// innermost to outermost. `isObserver` mirrors VariableDeclaration::isObserver so a captured `ref` can be rejected.
enum class ScopeKind { Local, Param, Field };

struct ScopeEntry {
    std::string type;
    bool isObserver = false;
    ScopeKind kind = ScopeKind::Local;
};

// A scope-type stack for this pass only: it runs before the analyzers, and only captured names are ever looked up.
class ScopeStack {
public:
    void push() { scopes_.emplace_back(); }
    void pop() { scopes_.pop_back(); }

    void declare(const std::string& name, const std::string& type, bool isObserver,
                 ScopeKind kind = ScopeKind::Local) {
        scopes_.back()[name] = ScopeEntry{type, isObserver, kind};
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

// The interface named by a closure must be a declared class with exactly one virtual method; that is the one the closure overrides.
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

// The closure's parameters must match the interface method's in count and types, so the closure's method overrides it.
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

// Closure classes are named after the source file plus a per-file counter (`__closure_app_0`), so the name
// is the same whether app.clg is compiled alone or merged into an includer, and two files never collide.
std::map<std::string, int> g_closureCounters;
std::string g_currentOrigin;
bool g_currentIsExternal = false;

std::string originStem(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    size_t dot = name.find_last_of('.');
    if (dot != std::string::npos) name = name.substr(0, dot);
    for (char& c : name) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) c = '_';
    }
    return name;
}

// Builds the synthesized class for one closure and the NewExpression that replaces it.
// The closure body is walked with a scope seeded only from the closure's own parameters, so a nested closure
// cannot see the outer closure's captures (they are fields reachable only through `this`, which cannot be
// captured); such a capture falls through to the ordinary closure-unknown-capture rejection.
struct DesugarResult {
    StatementPtr closureClass;
    ExpressionPtr replacement;
};

void walkBody(std::vector<StatementPtr>& body, ScopeStack& scope,
             const std::map<std::string, const ClassDeclaration*>& classesByName,
             std::vector<StatementPtr>& pendingClasses);

// Nesting depth of `spawn` bodies being walked; a `spawn` inside a `spawn` body is rejected.
int g_spawnDepth = 0;

// A value that may cross into another thread: copied on capture, so nothing the new thread reaches is
// reachable from the creating thread. Primitives (incl. string) and vectors of primitives. Every
// class-typed object is refused here (a captured shared_ptr would carry a live alias across).
//
// A `struct` is a value type too: stored bare, and with no handle its dynamic type is its static type, so it
// cannot hide a subclass. It crosses by copy iff every field and its base struct do. A class-typed field
// makes it unsendable.
bool isSendableValueType(const std::string& type,
                         const std::map<std::string, const ClassDeclaration*>& classesByName,
                         std::set<std::string>& visiting) {
    if (isPrimitiveType(type)) return true;
    if (isVectorType(type)) return isSendableValueType(vectorElementType(type), classesByName, visiting);
    // A Channel handle is copied in and stays valid in the creator: the queue is mutex-guarded and carries plain-value copies only.
    if (isChannelType(type)) return isSendableValueType(channelElementType(type), classesByName, visiting);
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

// Every name a `spawn` body reads, writes or declares. Used to refuse a body that uses an enclosing local it did not capture.
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
        // a closure literal in the body: its capture list reads names of this body; its own body is checked when desugared
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
                         std::vector<StatementPtr>& pendingClasses, bool isSpawn = false,
                         bool isHandler = false) {
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
    if (isHandler && (ifaceMethod->name != "fail" || ifaceMethod->returnType != "void" ||
                      ifaceMethod->parameters.size() != 1 || ifaceMethod->parameters[0].first != "string")) {
        reject("spawn-handler-shape", &closure,
               "the failure handler of `spawn` needs an interface whose one virtual method is "
               "`void fail(string message)` -- `" + iface.name + "::" + ifaceMethod->name + "` is not",
               "the handler runs on the worker thread when the job throws, and receives the thrown message");
    }
    if (isSpawn && !isHandler && (ifaceMethod->returnType != "void" || !ifaceMethod->parameters.empty())) {
        reject("spawn-interface-shape", &closure,
               "`spawn` needs a closure whose interface method is `void run()` -- `" + iface.name + "::" +
                   ifaceMethod->name + "` is not",
               "a spawned job has no caller to return to and no arguments to receive; hand data in through the "
               "capture list and results out through an ext_fn such as app_return");
    }

    // Resolve every capture's type before building anything, rejecting on the first bad name.
    std::vector<std::pair<std::string, std::string>> resolvedCaptures;  // (type, name)
    for (const auto& name : closure.captures) {
        // `[this]` never reaches here: `this` is a keyword, so it is a parse error.
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
        // A class object is admitted provisionally: whether it may be moved into the thread depends on the pointer
        // kind the analyzer infers later, so the object rules run in DanglingChecker::checkSpawnCall. Only a plain
        // local of a class declared in this program qualifies.
        bool movableObject = false;
        if (isSpawn && !isHandler) {
            auto clsIt = classesByName.find(entry->type);
            if (clsIt != classesByName.end() && !clsIt->second->isValueType &&
                !clsIt->second->isExternalDeclaration) {
                if (entry->kind != ScopeKind::Local) {
                    reject("spawn-captures-object", &closure,
                           "`spawn` captures `" + name + "`, a " +
                               std::string(entry->kind == ScopeKind::Field ? "field" : "parameter") +
                               " of class type `" + entry->type + "`",
                           "an object may be moved into a new thread only from a local variable created in this "
                           "function with `new " + entry->type + "(...)`: a parameter or field may hold an object "
                           "of a subclass, or be reachable from elsewhere, which this function cannot see. Make "
                           "the data a `struct`, or serialize it to a string");
                }
                movableObject = true;
            }
        }
        if (isSpawn && !movableObject && !isSendableValueType(entry->type, classesByName, visiting)) {
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

    const std::string stem = originStem(g_currentOrigin);
    const std::string closureClassName =
        "__closure_" + (stem.empty() ? std::string() : stem + "_") + std::to_string(g_closureCounters[stem]++);

    // Fields: one VariableDeclaration per capture.
    std::vector<StatementPtr> members;
    for (const auto& [type, name] : resolvedCaptures) {
        members.push_back(std::make_unique<VariableDeclaration>(type, name, nullptr));
    }

    // Constructor: one parameter per capture, assigned to the same-named field. The parameter is named
    // "p_<capture>" because the emitter has no parameter/field shadowing (`total = total;` would emit
    // `this->total` on both sides). The emitter hoists the leading own-field assignments into a member initializer list.
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

    // Walk the closure's own body before moving it into the method; this is what makes nesting work. The scope is
    // seeded only from `closure.parameters`. An inner closure's class is pushed onto `pendingClasses` before this
    // one, so it is declared first.
    ScopeStack innerScope;
    innerScope.push();
    for (const auto& [ptype, pname] : closure.parameters) {
        innerScope.declare(pname, ptype, false, ScopeKind::Param);
    }
    if (isSpawn) ++g_spawnDepth;
    walkBody(closure.body, innerScope, classesByName, pendingClasses);
    if (isSpawn) --g_spawnDepth;

    // The overriding method: the interface method's name, return type and parameters, with the closure body.
    auto overrideMethod = std::make_unique<MethodDeclaration>(
        ifaceMethod->returnType, ifaceMethod->name, closure.parameters, std::move(closure.body));
    overrideMethod->isOverride = true;
    members.push_back(std::move(overrideMethod));

    auto closureClass = std::make_unique<ClassDeclaration>(closureClassName, std::move(members));
    closureClass->baseClass = closure.interfaceName;
    closureClass->isSpawnJob = isSpawn && !isHandler;
    closureClass->isExternalDeclaration = g_currentIsExternal;
    closureClass->originFile = g_currentOrigin;
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

// Replaces any ClosureExpression found in `expr` with its desugared NewExpression, appending the synthesized
// class to `pendingClasses`. Called on every expression position that can hold a closure.
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
        if (call->callee == nullptr && call->function_name == "cling::setJobFailure") {
            reject("spawn-reserved", call, "`cling::setJobFailure` is reserved for the compiler",
                   "write `onJobFailure(closure Failure [values] (string message) { ... })` instead");
        }
        if (call->callee == nullptr && call->function_name == "onJobFailure") {
            auto handler = call->arguments.size() == 1
                ? dynamic_cast<ClosureExpression*>(call->arguments[0].get()) : nullptr;
            if (handler == nullptr) {
                reject("spawn-form", call, "`onJobFailure` takes exactly one closure literal",
                       "write `onJobFailure(closure Failure [values] (string message) { ... })`");
            }
            if (g_spawnDepth > 0) {
                reject("spawn-nested", call, "`onJobFailure` inside a `spawn` body",
                       "register the failure handler from the thread that spawns, before the `spawn` call");
            }
            DesugarResult result = desugarOne(*handler, scope, classesByName, pendingClasses, true, true);
            pendingClasses.push_back(std::move(result.closureClass));
            std::vector<ExpressionPtr> args;
            args.push_back(std::move(result.replacement));
            auto replaced = std::make_unique<FunctionCall>("cling::setJobFailure", std::move(args));
            replaced->loc = call->loc;
            expr = std::move(replaced);
            return;
        }
        if (call->callee == nullptr && call->function_name == "cling::spawn") {
            reject("spawn-reserved", call, "`cling::spawn` is reserved for the compiler",
                   "write `spawn(closure Job [values] () { ... })` instead -- calling the runtime helper directly "
                   "would skip the check on what is captured");
        }
        if (call->callee == nullptr && call->function_name == "spawn") {
            auto closure = !call->arguments.empty() && call->arguments.size() <= 2
                ? dynamic_cast<ClosureExpression*>(call->arguments[0].get()) : nullptr;
            auto handler = call->arguments.size() == 2
                ? dynamic_cast<ClosureExpression*>(call->arguments[1].get()) : nullptr;
            if (closure == nullptr || (call->arguments.size() == 2 && handler == nullptr)) {
                reject("spawn-form", call, "`spawn` takes a closure literal, and optionally a second one as failure handler",
                       "write `spawn(closure Job [values] () { ... })` or "
                       "`spawn(closure Job [values] () { ... }, closure Failure [values] (string message) { ... })` -- "
                       "a named object cannot be spawned, because only the capture lists are checked for what "
                       "crosses into the new thread");
            }
            if (g_spawnDepth > 0) {
                reject("spawn-nested", call, "`spawn` inside a `spawn` body",
                       "a spawned job may not start more threads (v1) -- start both from the creating thread");
            }
            DesugarResult result = desugarOne(*closure, scope, classesByName, pendingClasses, true);
            pendingClasses.push_back(std::move(result.closureClass));
            std::vector<ExpressionPtr> args;
            args.push_back(std::move(result.replacement));
            if (handler != nullptr) {
                DesugarResult onFail = desugarOne(*handler, scope, classesByName, pendingClasses, true, true);
                pendingClasses.push_back(std::move(onFail.closureClass));
                args.push_back(std::move(onFail.replacement));
            }
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
        // a closure passed as a constructor argument (`new Holder(closure ...)`)
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
    // Identifier, literals and the rest hold no closure: nothing to recurse into.
}

// Walks one body's statements, rewriting every expression position and recursing into nested blocks, each with
// its own scope. Declarations enter `scope` as they are met, so a capture sees only names declared before it.
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

bool isPlainValueType(const std::string& type, const Program& program) {
    std::map<std::string, const ClassDeclaration*> classesByName;
    for (const auto& stmt : program.statements) {
        if (auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get())) classesByName[cls->name] = cls;
    }
    std::set<std::string> visiting;
    return isSendableValueType(type, classesByName, visiting);
}

void desugarClosures(Program& program) {
    std::map<std::string, const ClassDeclaration*> classesByName;
    for (const auto& stmt : program.statements) {
        if (auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
            classesByName[cls->name] = cls;
        }
    }

    std::vector<StatementPtr> newStatements;
    newStatements.reserve(program.statements.size());

    g_closureCounters.clear();
    for (auto& topLevel : program.statements) {
        std::vector<StatementPtr> pendingClasses;
        ScopeStack scope;
        scope.push();
        g_currentOrigin = topLevel->originFile;
        g_currentIsExternal = topLevel->isExternalDeclaration;

        if (auto cls = dynamic_cast<ClassDeclaration*>(topLevel.get())) {
            // Fields are in scope by bare name for every method and constructor body.
            for (const auto& member : cls->members) {
                if (auto field = dynamic_cast<const VariableDeclaration*>(member.get())) {
                    scope.declare(field->identifier, field->type, false, ScopeKind::Field);
                }
            }
            for (auto& member : cls->members) {
                scope.push();
                if (auto ctor = dynamic_cast<ConstructorDeclaration*>(member.get())) {
                    for (const auto& [ptype, pname] : ctor->parameters) {
                        scope.declare(pname, ptype, false, ScopeKind::Param);
                    }
                    walkBody(ctor->body, scope, classesByName, pendingClasses);
                } else if (auto method = dynamic_cast<MethodDeclaration*>(member.get())) {
                    for (const auto& [ptype, pname] : method->parameters) {
                        scope.declare(pname, ptype, false, ScopeKind::Param);
                    }
                    walkBody(method->body, scope, classesByName, pendingClasses);
                }
                scope.pop();
            }
        } else if (auto fn = dynamic_cast<FunctionDeclaration*>(topLevel.get())) {
            for (const auto& [ptype, pname] : fn->parameters) {
                scope.declare(pname, ptype, false, ScopeKind::Param);
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
