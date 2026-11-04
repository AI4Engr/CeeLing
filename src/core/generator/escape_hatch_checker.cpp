#include "escape_hatch_checker.h"
#include "class_hierarchy.h"
#include "library_summary.h"

namespace cling {

std::vector<EscapeHatchError> EscapeHatchChecker::check(const Program& program) {
    errors_.clear();
    classNames_.clear();
    classFields_.clear();
    knownFreeFunctions_.clear();
    freeFunctionReturnType_.clear();
    classMethods_.clear();
    classMethodReturnType_.clear();
    classSpecializationArities_.clear();
    collectClasses(program);
    collectCallables(program);
    enumNames_.clear();
    reportedUnknownTypes_.clear();
    for (const auto& stmt : program.statements) {
        if (auto en = dynamic_cast<const EnumDeclaration*>(stmt.get())) {
            enumNames_.insert(en->name);
            if (!en->namespaceName.empty()) enumNames_.insert(en->namespaceName + "::" + en->name);
        }
    }

    for (const auto& stmt : program.statements) {
        // Only declarations (functions, classes, enums, includes) have a place at file scope. An
        // executable statement there has no lowering: the emitter used to drop a declaration like
        // `int counter = 0;` silently while keeping every use of it, so the failure surfaced as a
        // C++ compile error instead of a CeeLing diagnostic.
        if (dynamic_cast<const VariableDeclaration*>(stmt.get()) != nullptr ||
            dynamic_cast<const ContainerDeclaration*>(stmt.get()) != nullptr ||
            dynamic_cast<const AssignmentStatement*>(stmt.get()) != nullptr ||
            dynamic_cast<const ExpressionStatement*>(stmt.get()) != nullptr ||
            dynamic_cast<const IfStatement*>(stmt.get()) != nullptr ||
            dynamic_cast<const WhileStatement*>(stmt.get()) != nullptr ||
            dynamic_cast<const ForStatement*>(stmt.get()) != nullptr ||
            dynamic_cast<const TryStatement*>(stmt.get()) != nullptr ||
            dynamic_cast<const ThrowStatement*>(stmt.get()) != nullptr ||
            dynamic_cast<const ReturnStatement*>(stmt.get()) != nullptr) {
            EscapeHatchError e;
            e.line = stmt->loc.line;
            e.column = stmt->loc.column;
            e.message = "statement at file scope";
            e.help = "File-scope variables are not supported (global mutable state has no owner and no "
                     "lifetime the checker can reason about). Put the value in a function, or pass it "
                     "as a parameter or a class field.";
            errors_.push_back(e);
            continue;
        }
        if (auto fn = dynamic_cast<const FunctionDeclaration*>(stmt.get())) {
            requireKnownType(fn->returnType, "the return type of `" + fn->name + "`", fn->loc.line, fn->loc.column);
            for (const auto& [ptype, pname] : fn->parameters) {
                requireKnownType(ptype, "the type of parameter `" + pname + "`", fn->loc.line, fn->loc.column, /*allowView=*/true);
            }
            checkBody(fn->body, fn->parameters, "");
        } else if (auto mn = dynamic_cast<const MainFunction*>(stmt.get())) {
            checkBody(mn->body, {}, "");
        } else if (auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
            for (const auto& member : cls->members) {
                if (auto field = dynamic_cast<const VariableDeclaration*>(member.get())) {
                    requireKnownType(field->type, "the type of field `" + field->identifier + "`", field->loc.line, field->loc.column);
                } else if (auto method = dynamic_cast<const MethodDeclaration*>(member.get())) {
                    // a primary generic method `<T> T as()` names its own type parameter: `T` is not an unknown type inside it
                    typeParameter_ = method->typeParameter;
                    requireKnownType(method->returnType, "the return type of `" + method->name + "`", method->loc.line, method->loc.column);
                    for (const auto& [ptype, pname] : method->parameters) {
                        requireKnownType(ptype, "the type of parameter `" + pname + "`", method->loc.line, method->loc.column, /*allowView=*/true);
                    }
                    checkBody(method->body, method->parameters, cls->name);
                    typeParameter_.clear();
                } else if (auto ctor = dynamic_cast<const ConstructorDeclaration*>(member.get())) {
                    for (const auto& [ptype, pname] : ctor->parameters) {
                        requireKnownType(ptype, "the type of parameter `" + pname + "`", ctor->loc.line, ctor->loc.column, /*allowView=*/true);
                    }
                    checkBody(ctor->body, ctor->parameters, cls->name, /*isConstructor=*/true);
                }
            }
        }
    }
    return errors_;
}

void EscapeHatchChecker::collectClasses(const Program& program) {
    // Base-before-derived so a derived class inherits its base's already-flattened
    // fields; a same-named field declared by the derived class then overwrites the
    // inherited entry, which is the correct shadowing behavior. Flattening here is
    // what lets every classFields_ lookup below stay an exact match with no
    // base-chain walking (see class_hierarchy.h).
    for (const ClassDeclaration* cls : classesInBaseFirstOrder(program)) {
        classNames_.insert(cls->name);
        auto& fields = classFields_[cls->name];
        if (!cls->baseClass.empty()) {
            if (auto baseIt = classFields_.find(cls->baseClass); baseIt != classFields_.end()) {
                fields = baseIt->second;
            }
            // `.get()` is an ordinary user method if ANY class in the chain declares
            // it -- inheriting a `get()` makes it just as legitimate a call.
            if (classesWithOwnGet_.count(cls->baseClass) > 0) {
                classesWithOwnGet_.insert(cls->name);
            }
        }
        for (const auto& member : cls->members) {
            if (auto field = dynamic_cast<const VariableDeclaration*>(member.get())) {
                fields[field->identifier] = field->type;
            } else if (auto method = dynamic_cast<const MethodDeclaration*>(member.get())) {
                if (method->name == "get") {
                    classesWithOwnGet_.insert(cls->name);
                }
            }
        }

        // Dual-register under the qualified name (`YAML::Node`), mirroring
        // AstEmitter::collectClassNames -- a variable declared `YAML::Node x;` needs
        // classFields_["YAML::Node"] to resolve `x`'s field accesses, the same
        // reason the emitter needs it for codegen.
        if (!cls->namespaceName.empty()) {
            const std::string qualified = cls->namespaceName + "::" + cls->name;
            classNames_.insert(qualified);
            classFields_[qualified] = fields;
            if (classesWithOwnGet_.count(cls->name) > 0) {
                classesWithOwnGet_.insert(qualified);
            }
        }
    }
}

void EscapeHatchChecker::collectCallables(const Program& program) {
    for (const auto& stmt : program.statements) {
        if (auto fn = dynamic_cast<const FunctionDeclaration*>(stmt.get())) {
            knownFreeFunctions_.insert(fn->name);
            freeFunctionReturnType_[fn->name] = fn->returnType;
            // Dual-register under the qualified name (`YAML::Load`) -- a call
            // site inside a DIFFERENT namespace (or at global scope) spells it
            // qualified, same reasoning as ClassDeclaration's identical
            // dual-registration for `YAML::Node`.
            if (!fn->namespaceName.empty()) {
                knownFreeFunctions_.insert(fn->namespaceName + "::" + fn->name);
                freeFunctionReturnType_[fn->namespaceName + "::" + fn->name] = fn->returnType;
            }
        } else if (auto extFn = dynamic_cast<const ExternFunctionDeclaration*>(stmt.get())) {
            // An ext_fn has no body for isKnownCallable to reason about beyond
            // "is this name callable at all" -- same treatment as an ordinary
            // FunctionDeclaration at THIS checker's level, which never looks
            // past the name/arity here (DanglingChecker is what consults the
            // hand-declared borrow summary; SmartPointerAnalyzer needs no
            // ext_fn awareness since v1 is primitive/vector-of-primitive only).
            knownFreeFunctions_.insert(extFn->name);
            freeFunctionReturnType_[extFn->name] = extFn->returnType;
        }
    }
    // Same base-before-derived flattening as collectClasses: without it,
    // `d.baseMethod()` would fail isKnownCallable's exact lookup and be rejected by
    // the default-reject-unknown-calls rule -- a safe failure, but a false one.
    for (const ClassDeclaration* cls : classesInBaseFirstOrder(program)) {
        auto& methods = classMethods_[cls->name];
        auto& virtuals = classVirtualMethods_[cls->name];
        auto& returnTypes = classMethodReturnType_[cls->name];
        if (!cls->baseClass.empty()) {
            if (auto baseIt = classMethods_.find(cls->baseClass); baseIt != classMethods_.end()) {
                methods = baseIt->second;
            }
            if (auto baseIt = classMethodReturnType_.find(cls->baseClass);
                baseIt != classMethodReturnType_.end()) {
                returnTypes = baseIt->second;
            }
            // A method stays virtual all the way down the chain once any ancestor
            // declared it so -- that's exactly what makes calling it from a
            // constructor a trap even in a class that never wrote `virtual` itself.
            if (auto baseIt = classVirtualMethods_.find(cls->baseClass);
                baseIt != classVirtualMethods_.end()) {
                virtuals = baseIt->second;
            }
            // Inherit the base's specialization arities too, re-keyed under
            // THIS class's own name (classSpecializationArities_ is keyed by
            // "Class::specKey", not flattened via a shared className->set map
            // the way classMethods_ is, so a derived class needs its own
            // explicit copy). Snapshotted into a separate vector first --
            // inserting into classSpecializationArities_ while iterating it
            // directly would invalidate the iteration.
            const std::string basePrefix = cls->baseClass + "::";
            std::vector<std::pair<std::string, std::set<size_t>>> inherited;
            for (const auto& [key, arities] : classSpecializationArities_) {
                if (key.compare(0, basePrefix.size(), basePrefix) == 0) {
                    inherited.emplace_back(cls->name + "::" + key.substr(basePrefix.size()), arities);
                }
            }
            for (auto& [key, arities] : inherited) {
                classSpecializationArities_[key] = std::move(arities);
            }
        }
        for (const auto& member : cls->members) {
            if (auto method = dynamic_cast<const MethodDeclaration*>(member.get())) {
                // A specialization (`int as<int>() {...}`) registers under its
                // SYNTHETIC key ("as<int>"), not the bare method name -- a call
                // WITHOUT a type argument must never resolve to a specialization's
                // body (isKnownCallable below only ever looks up the synthetic key
                // when the call itself carries a matching templateArg). The bare
                // name stays reserved for the primary generic (or an ordinary
                // method, the common case where specializationArg is empty).
                const std::string key = method->specializationArg.empty()
                    ? method->name
                    : specializationKey(method->name, method->specializationArg);
                methods.insert(key);
                returnTypes[key] = method->returnType;
                if (method->isVirtual || method->isOverride) {
                    virtuals.insert(key);
                }
                // Record this specialization's own declared arity, ALWAYS (not
                // just when a same-type-argument sibling collides) -- see
                // classSpecializationArities_'s own comment for why plain
                // classMethods_ membership alone can't answer "was THIS call's
                // arity ever actually declared" (yaml-cpp's `as<T>()` vs
                // `as<T>(T fallback)` pair needs two real, differently-shaped
                // C++ template primaries -- confirmed via probe that a call to
                // a nonexistent arity, e.g. `as<int>(99)` when only `as<int>()`
                // exists, produced generated C++ that fails to compile, yet
                // isKnownCallable's plain membership check let it through).
                if (!method->specializationArg.empty()) {
                    classSpecializationArities_[cls->name + "::" + key].insert(method->parameters.size());
                }
            }
        }

        if (!cls->namespaceName.empty()) {
            const std::string qualified = cls->namespaceName + "::" + cls->name;
            classMethods_[qualified] = methods;
            classVirtualMethods_[qualified] = virtuals;
            const std::string ownPrefix = cls->name + "::";
            std::vector<std::pair<std::string, std::set<size_t>>> requalified;
            for (const auto& [key, arities] : classSpecializationArities_) {
                if (key.compare(0, ownPrefix.size(), ownPrefix) == 0) {
                    requalified.emplace_back(qualified + "::" + key.substr(ownPrefix.size()), arities);
                }
            }
            for (auto& [key, arities] : requalified) {
                classSpecializationArities_[key] = std::move(arities);
            }
        }
    }
}

std::string EscapeHatchChecker::resolveDeclaredType(const Expression* expr) const {
    if (auto id = dynamic_cast<const Identifier*>(expr)) {
        if (auto it = localType_.find(id->name); it != localType_.end()) {
            return it->second;
        }
        if (!currentClass_.empty()) {
            if (auto clsIt = classFields_.find(currentClass_); clsIt != classFields_.end()) {
                if (auto fIt = clsIt->second.find(id->name); fIt != clsIt->second.end()) {
                    return fIt->second;
                }
            }
        }
        return {};
    }
    // A chained field access (`data.scalar.length()`, receiver is
    // `data.scalar`, a MemberAccess, not a bare Identifier): resolve the
    // OBJECT half (`data`) to its class via resolveObjectType (already walks
    // arbitrary MemberAccess chains through classFields_), then look up the
    // final member's OWN declared type in that class's field map. Needed for
    // RB18 (Node::operator= alias-rebind): yaml_node.clg's fields now live one
    // level down through a `NodeData data` indirection, so a receiver like
    // `data.scalar` (a string-typed field of a class-typed field) must resolve
    // to "string" for `.length()`/`.substr()` to pass the string-method
    // allow-list below -- before this, only a bare identifier receiver
    // resolved at all, so any method call through a chained field access was
    // wrongly rejected as unknown (default-reject caught a legitimate call).
    if (auto member = dynamic_cast<const MemberAccess*>(expr)) {
        const std::string objClass = resolveObjectType(member->object.get());
        if (objClass.empty()) return {};
        if (auto clsIt = classFields_.find(objClass); clsIt != classFields_.end()) {
            if (auto fIt = clsIt->second.find(member->member); fIt != clsIt->second.end()) {
                return fIt->second;
            }
        }
        return {};
    }
    return {};
}

bool EscapeHatchChecker::isKnownCallable(const FunctionCall& call) const {
    if (call.callee) {
        // `recv.method(args)`: the receiver's class must be known AND declare this
        // method. An unresolvable receiver (e.g. the result of another call -- this
        // checker does lightweight resolution, not full type inference) is treated
        // as unknown, matching resolveObjectType's existing conservative behavior.
        auto member = dynamic_cast<const MemberAccess*>(call.callee.get());
        if (member == nullptr) return false;

        // A known library method on a recognized container type, e.g.
        // `v.push_back(x)` where `v`'s declared type is `std::vector<T>`.
        const std::string declaredType = resolveDeclaredType(member->object.get());
        if (isVectorType(declaredType)) {
            // resize/reserve/assign/copyWithin only on primitive-element vectors --
            // see isKnownVectorMethod's comment in library_summary.h. (`assign(n, x)` on an object vector would copy an owner.)
            if (member->member == "resize" || member->member == "reserve" ||
                member->member == "assign" || member->member == "copyWithin") {
                return isPrimitiveType(vectorElementType(declaredType));
            }
            return isKnownVectorMethod(member->member);
        }
        // A Channel<T> (library_summary.h): send/wait/take/size/close/isClosed with their exact argument counts; nothing else.
        if (isChannelType(declaredType)) {
            return isKnownChannelMethod(member->member, call.arguments.size());
        }
        if (isStringType(declaredType)) {
            return isKnownStringMethod(member->member);
        }
        if (isStringViewType(declaredType)) {
            return isKnownStringViewMethod(member->member);
        }
        if (isMapType(declaredType)) {
            return isKnownMapMethod(member->member);
        }
        // A known-external class (e.g. Eigen::MatrixXd) calling one of its
        // allow-listed borrowing methods (`.block()`, `.row()`, ...) -- see
        // library_summary.h's isKnownBorrowingMethod doc comment. Checked
        // before the CeeLing-own-class path below since a known-external type
        // never has a classMethods_ entry (nothing parses its declaration).
        if (!declaredType.empty() && isKnownExternalClassType(declaredType)) {
            return isKnownBorrowingMethod(declaredType, member->member) || isKnownFreshMethod(declaredType, member->member) ||
                   isKnownMutatorMethod(declaredType, member->member);
        }

        const std::string objClass = resolveObjectType(member->object.get());
        if (objClass.empty()) return false;
        auto it = classMethods_.find(objClass);
        if (it == classMethods_.end()) return false;
        // A call carrying a type argument (`n.as<int>()`) resolves to the
        // SPECIALIZATION's synthetic key, never the bare method name -- a
        // specialization must exist for the exact requested type, matching real
        // C++ (there is no implicit conversion between specializations).
        if (call.templateArg.empty()) {
            return it->second.count(member->member) > 0;
        }
        const std::string key = specializationKey(member->member, call.templateArg);
        if (it->second.count(key) == 0) return false;
        // A type-argument match alone isn't enough -- the call's own argument
        // COUNT must match one of this specialization's actually-declared
        // arities too (yaml-cpp's `as<T>()` vs `as<T>(T fallback)` are two
        // separate C++ template primaries at different arities; a call to a
        // nonexistent arity, e.g. `as<int>(99)` when only `as<int>()` was ever
        // declared, must be rejected here -- confirmed via probe that the
        // generated C++ for that case does not compile, yet a plain type-only
        // membership check let it through).
        auto arityIt = classSpecializationArities_.find(objClass + "::" + key);
        return arityIt != classSpecializationArities_.end() &&
               arityIt->second.count(call.arguments.size()) > 0;
    }
    // `int(x)` etc.: a primitive conversion, allowed only when the argument is not known to be an
    // object, container or string (an unresolvable expression such as `a + b` is a primitive
    // computation or a type error the C++ compile gate reports -- neither is a memory hazard).
    if (call.templateArg.empty() && isPrimitiveConversionName(call.function_name)) {
        if (call.arguments.size() != 1) return false;
        const Expression* arg = call.arguments[0].get();
        const std::string declared = resolveDeclaredType(arg);
        if (!declared.empty() && !isPrimitiveType(declared)) return false;
        if (declared == "string" || declared == "std::string") return false;
        return resolveObjectType(arg).empty();
    }
    // Plain call `f(args)`: either a free function, or -- inside a method body -- a
    // bare same-class method call with an implicit `this` receiver, or a qualified
    // library free function on the allow-list (`std::stoi(x)`).
    if (call.templateArg.empty() && knownFreeFunctions_.count(call.function_name) > 0) return true;
    if (call.templateArg.empty() && isKnownQualifiedFreeFunction(call.function_name)) return true;
    if (call.templateArg.empty() && isKnownRuntimeHelper(call.function_name)) return true;
    if (!currentClass_.empty()) {
        auto it = classMethods_.find(currentClass_);
        if (it != classMethods_.end()) {
            if (call.templateArg.empty()) {
                if (it->second.count(call.function_name) > 0) return true;
            } else {
                // Same type-then-arity check as the receiver-qualified branch
                // above, for a bare same-class call (`as<int>(99)` with an
                // implicit `this`).
                const std::string key = specializationKey(call.function_name, call.templateArg);
                if (it->second.count(key) > 0) {
                    auto arityIt = classSpecializationArities_.find(currentClass_ + "::" + key);
                    if (arityIt != classSpecializationArities_.end() &&
                        arityIt->second.count(call.arguments.size()) > 0) {
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

void EscapeHatchChecker::checkBody(const std::vector<StatementPtr>& body,
                                    const std::vector<std::pair<std::string, std::string>>& params,
                                    const std::string& ownerClass, bool isConstructor) {
    localType_.clear();
    currentClass_ = ownerClass;
    inConstructor_ = isConstructor;
    for (const auto& [type, name] : params) {
        localType_[name] = type;
    }
    for (const auto& stmtPtr : body) {
        checkStatement(stmtPtr.get());
    }
    inConstructor_ = false;
}

std::string EscapeHatchChecker::resolveObjectType(const Expression* expr) const {
    if (expr == nullptr) return {};

    if (auto id = dynamic_cast<const Identifier*>(expr)) {
        if (auto it = localType_.find(id->name); it != localType_.end()) {
            return classNames_.count(it->second) ? it->second : std::string{};
        }
        // Not a local/param: fall back to a field of the enclosing class (bare field
        // access inside a method, e.g. `left.get()` for a field named `left`).
        if (!currentClass_.empty()) {
            if (auto clsIt = classFields_.find(currentClass_); clsIt != classFields_.end()) {
                if (auto fIt = clsIt->second.find(id->name); fIt != clsIt->second.end()) {
                    return classNames_.count(fIt->second) ? fIt->second : std::string{};
                }
            }
        }
        return {};
    }
    if (auto member = dynamic_cast<const MemberAccess*>(expr)) {
        std::string objClass = resolveObjectType(member->object.get());
        if (objClass.empty()) return {};
        if (auto clsIt = classFields_.find(objClass); clsIt != classFields_.end()) {
            if (auto fIt = clsIt->second.find(member->member); fIt != clsIt->second.end()) {
                return classNames_.count(fIt->second) ? fIt->second : std::string{};
            }
        }
        return {};
    }
    if (auto call = dynamic_cast<const FunctionCall*>(expr)) {
        // A CHAINED receiver (`a.b().c()`: resolving `a.b()`'s own class here so
        // `.c()` can be checked against it). Only `recv.method(...)` calls are
        // resolved -- a bare `f()` free-function call has no per-function return
        // CLASS tracked anywhere in this checker (only classMethodReturnType_
        // exists, keyed by owning class), so it stays unresolved, same
        // conservative "unknown shape -> {}" fallback every other unhandled case
        // here already uses.
        //
        // Safe to widen resolution to a call receiver (previously always {}):
        // every CeeLing object is compiler-wrapped in a smart pointer, so a
        // method's return value is always a live reference-counted/owning COPY,
        // never a raw alias into a temporary that could already be destroyed --
        // confirmed via probe (chained call on a temporary Holder: the Holder
        // destructs at end of the full expression, but the returned shared_ptr
        // keeps the pointee alive, exactly like `Node r = pick(x, y);` already
        // does for free functions). This only widens what CAN be resolved to a
        // known class; an unresolvable receiver still falls through to {},
        // preserving the default-reject posture for everything genuinely unknown.
        if (!call->callee) {
            // A plain `f()` call -- the first link of a chain can be a free
            // function too (e.g. `makeHolder().getInner()`).
            auto fnIt = freeFunctionReturnType_.find(call->function_name);
            if (fnIt == freeFunctionReturnType_.end()) return {};
            return classNames_.count(fnIt->second) ? fnIt->second : std::string{};
        }
        auto member = dynamic_cast<const MemberAccess*>(call->callee.get());
        if (member == nullptr) return {};
        std::string recvClass = resolveObjectType(member->object.get());
        if (recvClass.empty()) return {};
        auto clsIt = classMethodReturnType_.find(recvClass);
        if (clsIt == classMethodReturnType_.end()) return {};
        auto methodIt = clsIt->second.find(member->member);
        if (methodIt == clsIt->second.end()) return {};
        const std::string& returnType = methodIt->second;
        return classNames_.count(returnType) ? returnType : std::string{};
    }
    // `new`, literals, etc: not resolved by this lightweight pass.
    return {};
}

void EscapeHatchChecker::checkExpression(const Expression* expr) {
    if (expr == nullptr) return;

    if (auto id = dynamic_cast<const Identifier*>(expr)) {
        // `reinterpret_cast<T>(x)` etc. have no cast syntax in CeeLing's grammar, so
        // they parse as ordinary comparison chains (`reinterpret_cast < T > (x)`) with
        // no ERROR node -- the parser never sees anything wrong. Catch it here: these
        // names are never legal identifiers in CeeLing source (no class, variable, or
        // function is allowed to shadow a C++ keyword), so their appearance is
        // conclusive evidence of an attempted cast.
        if (id->name == "reinterpret_cast" || id->name == "const_cast" ||
            id->name == "static_cast" || id->name == "dynamic_cast") {
            EscapeHatchError e;
            e.line = expr->loc.line;
            e.column = expr->loc.column;
            e.message = "C++-style casts are forbidden in CeeLing (found `" + id->name + "`)";
            e.help = "casting is a type-confusion / const-bypass escape hatch with no "
                     "safe equivalent (CLAUDE.md hatch #13/#16); there is no cast "
                     "syntax in the language.";
            errors_.push_back(e);
        }
        return;
    }
    if (auto call = dynamic_cast<const FunctionCall*>(expr)) {
        if (auto member = dynamic_cast<const MemberAccess*>(call->callee.get());
            member != nullptr && member->member == "get" && call->arguments.empty()) {
            std::string objClass = resolveObjectType(member->object.get());
            if (!objClass.empty() && classesWithOwnGet_.find(objClass) == classesWithOwnGet_.end()) {
                EscapeHatchError e;
                e.line = expr->loc.line;
                e.column = expr->loc.column;
                e.message = "'.get()' is forbidden on a CeeLing object ('" +
                            member->object->toString() + "' is a " + objClass + ")";
                e.help = "'.get()' would extract a raw pointer, which has no legal use "
                         "in CeeLing (CLAUDE.md hatch #14) -- use the object directly, "
                         "or add a named accessor method to the class if you need a "
                         "specific field.";
                errors_.push_back(e);
            }
        } else if (!isKnownCallable(*call)) {
            // Default-reject unknown library calls: CeeLing has no summary (borrow/
            // move contract) for anything it didn't compile itself, so it cannot
            // prove a call is safe -- default-allow would silently treat the return
            // value as "fresh" even if the callee actually hands back an alias of
            // one of its arguments (exactly the shape DanglingChecker's auto-derived
            // summaries catch for known functions). `.get()` calls are handled by
            // the branch above (they're rejected/accepted by a different rule, not
            // this one, so they don't fall through here).
            std::string calleeName = call->function_name;
            if (auto calleeMember = dynamic_cast<const MemberAccess*>(call->callee.get())) {
                calleeName = calleeMember->member;
            }
            EscapeHatchError e;
            e.line = expr->loc.line;
            e.column = expr->loc.column;
            e.message = "call to unknown function/method `" + calleeName + "`";
            e.help = "CeeLing has no borrow/ownership summary for anything it did not "
                     "compile itself; unknown calls are rejected by default rather than "
                     "silently trusted (CLAUDE.md \"default-reject, never default-allow\"). "
                     "Define `" + calleeName + "` in CeeLing source, or call an existing "
                     "CeeLing function/method instead.";
            if (call->callee == nullptr && isPrimitiveConversionName(call->function_name)) {
                e.message = "`" + call->function_name + "(...)` converts one primitive to another; "
                            "the argument here is not a single primitive";
                e.help = "Primitive conversions take exactly one int/float/double/char/byte/int64 "
                         "(or bool) value. A class, container or string cannot be cast to a "
                         "primitive -- that would be a type-confusion hatch (CLAUDE.md #13).";
            }
            // A method of std::vector / string that exists but is not admitted: say why (the rule is in library_summary.cpp).
            if (auto calleeMember = dynamic_cast<const MemberAccess*>(call->callee.get())) {
                const std::string recvType = resolveDeclaredType(calleeMember->object.get());
                std::string why;
                if (isVectorType(recvType)) why = vectorMethodRefusal(calleeMember->member);
                else if (isStringType(recvType)) why = stringMethodRefusal(calleeMember->member);
                else if (isStringViewType(recvType)) why = stringViewMethodRefusal(calleeMember->member);
                else if (isMapType(recvType)) why = mapMethodRefusal(calleeMember->member);
                if (!why.empty()) {
                    e.message = "`" + calleeMember->member + "` is not available on `" + recvType + "`: " + why;
                    e.help = "CeeLing admits a method of the standard library only when the checker can follow what it hands back; "
                             "the rule and the scanned facts are in library_summary.cpp and facts/std-*.clfacts.";
                }
            }
            if (auto calleeMember = dynamic_cast<const MemberAccess*>(call->callee.get());
                calleeMember != nullptr && calleeMember->member == "resize") {
                const std::string recvType = resolveDeclaredType(calleeMember->object.get());
                if (isVectorType(recvType)) {
                    e.message = "`resize` on `" + recvType + "`: element type is not primitive";
                    e.help = "`resize` is only allowed on vectors of primitive elements "
                             "(int, byte, char, ...). On a vector of objects it would fill "
                             "the new slots with null smart pointers, or duplicate an owner "
                             "for resize(n, x). Build the elements with push_back instead.";
                }
            }
            if (auto calleeMember = dynamic_cast<const MemberAccess*>(call->callee.get());
                calleeMember != nullptr && (calleeMember->member == "assign") &&
                isVectorType(resolveDeclaredType(calleeMember->object.get()))) {
                e.message = "`assign` on `" + resolveDeclaredType(calleeMember->object.get()) + "`: element type is not primitive";
                e.help = "`assign` is only allowed on vectors of primitive elements (int, byte, char, ...), matching `resize`: "
                         "assign(n, x) on a vector of objects would duplicate an owner.";
            }
            if (auto calleeMember = dynamic_cast<const MemberAccess*>(call->callee.get());
                calleeMember != nullptr && calleeMember->member == "reserve") {
                const std::string recvType = resolveDeclaredType(calleeMember->object.get());
                if (isVectorType(recvType)) {
                    e.message = "`reserve` on `" + recvType + "`: element type is not primitive";
                    e.help = "`reserve` is only allowed on vectors of primitive elements "
                             "(int, byte, char, ...), matching `resize`'s own restriction -- "
                             "v1 has no motivating case for reserving capacity on an "
                             "object-element vector.";
                }
            }
            if (auto calleeMember = dynamic_cast<const MemberAccess*>(call->callee.get());
                calleeMember != nullptr && calleeMember->member == "copyWithin") {
                const std::string recvType = resolveDeclaredType(calleeMember->object.get());
                if (isVectorType(recvType)) {
                    e.message = "`copyWithin` on `" + recvType + "`: element type is not primitive";
                    e.help = "`copyWithin` is only allowed on vectors of primitive elements "
                             "(int, byte, char, ...), matching `resize`/`reserve`'s own "
                             "restriction -- its codegen computes a byte count from the "
                             "element type (len * sizeof(T)), which is meaningless for an "
                             "object element.";
                }
            }
            errors_.push_back(e);
        }

        // Virtual call from a constructor (CLAUDE.md Phase 2.7). In C++ this does NOT
        // dispatch to the derived override -- the vtable still points at the base
        // while the base's constructor runs -- so the code silently does something
        // other than what it visually says. Purely structural: no provenance or
        // liveness needed, just "is this a call, inside a constructor, to a method
        // that is virtual anywhere in this class's hierarchy". Only constructors are
        // checked because CeeLing has no destructor syntax (the other half of the
        // classic trap simply cannot be written).
        if (inConstructor_ && !currentClass_.empty()) {
            // Both `this->method()` (bare name, implicit receiver) and an explicit
            // same-class receiver resolve to the enclosing class's method set.
            std::string virtualName;
            if (!call->callee) {
                virtualName = call->function_name;
            } else if (auto m = dynamic_cast<const MemberAccess*>(call->callee.get())) {
                if (resolveObjectType(m->object.get()) == currentClass_) {
                    virtualName = m->member;
                }
            }
            if (!virtualName.empty()) {
                auto clsIt = classVirtualMethods_.find(currentClass_);
                if (clsIt != classVirtualMethods_.end() && clsIt->second.count(virtualName) > 0) {
                    EscapeHatchError e;
                    e.line = expr->loc.line;
                    e.column = expr->loc.column;
                    e.message = "virtual method `" + virtualName +
                                "` called from a constructor of `" + currentClass_ + "`";
                    e.help = "A virtual call inside a constructor does NOT dispatch to a "
                             "derived class's override -- the object is still being built, "
                             "so the base version runs instead. That makes the code do "
                             "something different from what it looks like it does. Move the "
                             "call to after construction, or call a non-virtual method.";
                    errors_.push_back(e);
                }
            }
        }

        // `this` passed as a call argument from a CONSTRUCTOR must be rejected --
        // see rejectThisEscapeInConstructor's own doc comment for the full
        // bad_weak_ptr rationale. Checked narrowly (each argument must be
        // DIRECTLY the bare Identifier `this`) so `this.foo()` -- an ordinary,
        // always-safe member access that never extracts a shared_ptr copy -- is
        // never caught by this: that shape's `this` is checked via the
        // MemberAccess branch below, which this check deliberately does not touch.
        for (const auto& arg : call->arguments) {
            rejectThisEscapeInConstructor(arg.get(), expr);
        }

        checkExpression(call->callee.get());
        for (const auto& arg : call->arguments) {
            checkExpression(arg.get());
        }
        return;
    }
    if (auto member = dynamic_cast<const MemberAccess*>(expr)) {
        checkExpression(member->object.get());
        return;
    }
    if (auto sub = dynamic_cast<const SubscriptExpression*>(expr)) {
        // `n[key]` on a CLASS-typed receiver (not a container) means the user's own
        // `operator[]` -- subject to the same default-reject-unknown-calls rule an
        // ordinary method call already gets, since it's really `n.operator[](key)`
        // in disguise. A container receiver (vector/map) is unaffected: this only
        // fires when resolveObjectType finds an actual class.
        const std::string objClass = resolveObjectType(sub->object.get());
        if (!objClass.empty()) {
            auto it = classMethods_.find(objClass);
            const bool hasOperatorSubscript = it != classMethods_.end() &&
                it->second.count("operator[]") > 0;
            if (!hasOperatorSubscript) {
                EscapeHatchError e;
                e.line = expr->loc.line;
                e.column = expr->loc.column;
                e.message = "call to unknown function/method `operator[]`";
                e.help = "`" + sub->object->toString() + "` is a " + objClass +
                         ", which has no `operator[]` method -- CeeLing has no borrow/"
                         "ownership summary for anything it did not compile itself, so "
                         "subscripting a class value is rejected unless the class "
                         "declares its own `operator[]`.";
                errors_.push_back(e);
            }
        }
        checkExpression(sub->object.get());
        checkExpression(sub->index.get());
        return;
    }
    if (auto bin = dynamic_cast<const BinaryExpression*>(expr)) {
        // `ClassName * identifier` has no legal meaning as multiplication (a class
        // name is never a numeric value), and CeeLing has no separate pointer-
        // declarator syntax to tell `Node* p;` apart from `a * b;` at the grammar
        // level -- so a `*` whose left operand is a known class name is conclusively
        // an attempted raw pointer declaration, not real arithmetic.
        if (bin->operator_str == "*") {
            if (auto leftId = dynamic_cast<const Identifier*>(bin->left.get());
                leftId != nullptr && classNames_.count(leftId->name)) {
                EscapeHatchError e;
                e.line = expr->loc.line;
                e.column = expr->loc.column;
                e.message = "raw pointer types (`T*`) are forbidden (found `" +
                            leftId->name + "*`)";
                e.help = "pointers have no legal path through CeeLing (CLAUDE.md hatch "
                         "#15); declare a plain object variable and let the compiler "
                         "choose the smart-pointer type.";
                errors_.push_back(e);
            }
        }
        // An operator between two OBJECT-typed operands (e.g. `a * b` where both are
        // Eigen::MatrixXd) is really an implicit third-party operator-overload CALL --
        // the exact same "did CeeLing compile this itself" question isKnownCallable
        // already asks for an explicit `.method()` call, just spelled as an operator
        // instead. Without this check, a binary operator on any unrecognized foreign
        // type silently passed through unchecked (confirmed via probe) -- a real gap
        // in the default-reject boundary. Only fires when BOTH operands resolve to a
        // known-external class type (leaves ordinary primitive arithmetic, and
        // operators on CeeLing's own classes -- which already have their own
        // operator== / operator<< routing elsewhere -- completely untouched).
        const std::string leftType = resolveDeclaredType(bin->left.get());
        const std::string rightType = resolveDeclaredType(bin->right.get());
        if (!leftType.empty() && isKnownExternalClassType(leftType) &&
            !rightType.empty() && isKnownExternalClassType(rightType)) {
            if (!isKnownExternalOperator(leftType, bin->operator_str)) {
                EscapeHatchError e;
                e.line = expr->loc.line;
                e.column = expr->loc.column;
                e.message = "call to unknown operator `" + leftType + "::operator" +
                            bin->operator_str + "`";
                e.help = "CeeLing has no borrow/ownership summary for anything it did "
                         "not compile itself, so an operator between two " + leftType +
                         " values is rejected unless explicitly allow-listed.";
                errors_.push_back(e);
            }
        }
        checkExpression(bin->left.get());
        checkExpression(bin->right.get());
        return;
    }
    if (auto un = dynamic_cast<const UnaryExpression*>(expr)) {
        checkExpression(un->operand.get());
        return;
    }
    if (auto neww = dynamic_cast<const NewExpression*>(expr)) {
        requireKnownType(neww->className, "`new " + neww->className + "`", expr->loc.line, expr->loc.column);
        for (const auto& arg : neww->arguments) {
            checkExpression(arg.get());
        }
        return;
    }
    // Identifier/Number/ScopedIdentifier/StringLiteral leaves: nothing to check.
}

// See header for the full contract. `expr` must be checked as a DIRECT bare
// ThisExpression node (grammar.js's reserved `this_expression`) -- callers
// must NOT recurse into a larger expression and call this on some inner
// sub-expression, or a legal `this.foo()` receiver would be wrongly caught
// (a ThisExpression used as a MemberAccess's own receiver is not itself
// "the whole expression at this position"; the distinction is made by which
// call sites choose to call this function at all, e.g. checkExpression's
// MemberAccess branch deliberately never calls this on `member->object`).
void EscapeHatchChecker::rejectThisEscapeInConstructor(const Expression* expr, const ASTNode* anchor) {
    if (!inConstructor_) return;
    if (dynamic_cast<const ThisExpression*>(expr) == nullptr) return;
    EscapeHatchError e;
    e.line = anchor->loc.line;
    e.column = anchor->loc.column;
    e.message = "`this` passed as an argument from a constructor";
    e.help = "passing `this` lowers to shared_from_this(), which throws "
             "std::bad_weak_ptr unconditionally when called before the "
             "object's shared_ptr control block exists -- and inside a "
             "constructor, it never does yet. Move this to after "
             "construction (e.g. a separate init() method called once the "
             "object is already held by a shared_ptr).";
    errors_.push_back(e);
}

namespace {
std::string trimType(const std::string& t) {
    size_t a = t.find_first_not_of(" \t");
    size_t b = t.find_last_not_of(" \t");
    return a == std::string::npos ? std::string() : t.substr(a, b - a + 1);
}
}  // namespace

bool EscapeHatchChecker::typeIsKnown(const std::string& rawType) const {
    std::string type = trimType(rawType);
    // A `ref` observer is written `ref T x`: the parser keeps `T` here. A trailing `&`/`*` never reaches this point (raw pointers
    // and references are rejected elsewhere), so they are left to those checks.
    if (type.empty() || type == "void") return true;
    if (!typeParameter_.empty() && type == typeParameter_) return true;
    if (isPrimitiveType(type) || isStringType(type) || isOstreamType(type)) return true;
    if (classNames_.count(type) > 0 || enumNames_.count(type) > 0) return true;
    if (isKnownExternalClassType(type)) return true;
    if (isVectorType(type)) return typeIsKnown(vectorElementType(type));
    if (isMapType(type)) return typeIsKnown(mapKeyType(type)) && typeIsKnown(mapValueType(type));
    if (isChannelType(type)) return typeIsKnown(channelElementType(type));
    return false;
}

void EscapeHatchChecker::requireKnownType(const std::string& type, const std::string& what, int line, int column, bool allowView) {
    if (isStringViewType(trimType(type))) {
        if (allowView) return;
        if (!reportedUnknownTypes_.insert(trimType(type) + "@" + std::to_string(line)).second) return;
        EscapeHatchError e;
        e.line = line;
        e.column = column;
        e.message = "`string_view` cannot be used for " + what;
        e.help = "A `string_view` does not own its characters, and the checker follows one only as a local variable or a parameter, back to the string it "
                 "was made from. As a field, a return value or a container element it could outlive that string where nothing follows it. Use a `string` "
                 "(an owner) here.";
        errors_.push_back(e);
        return;
    }
    if (typeIsKnown(type)) return;
    // one report per distinct name and line: a type used ten times on one line is one mistake
    if (!reportedUnknownTypes_.insert(trimType(type) + "@" + std::to_string(line)).second) return;
    EscapeHatchError e;
    e.line = line;
    e.column = column;
    e.message = trimType(type) == "auto" ? "`auto` has no known type" : "unknown type `" + trimType(type) + "` for " + what;
    e.help = trimType(type) == "auto"
                 ? "CeeLing passes `auto` to C++ unchanged, so nothing could follow what the variable holds (an iterator, a view, a lazy "
                   "expression): that is how a dangling reference gets past the checker. Write the type, e.g. `int`, `string` or a class name."
                 : "CeeLing has no shape for `" + trimType(type) + "`, so every check would be blind to values of it. Declare it as a "
                   "class/struct in CeeLing, or for a C++ library type add a fact pack that declares `class " + trimType(type) +
                   "` and its methods (`cling --facts mylib.clfacts`, docs/internals/facts-format.md).";
    errors_.push_back(e);
}

void EscapeHatchChecker::checkStatement(const Statement* stmt) {
    if (auto decl = dynamic_cast<const VariableDeclaration*>(stmt)) {
        requireKnownType(decl->type, "variable `" + decl->identifier + "`", stmt->loc.line, stmt->loc.column, /*allowView=*/true);
        if (decl->isObserver && isStringViewType(trimType(decl->type))) {
            EscapeHatchError e;
            e.line = stmt->loc.line;
            e.column = stmt->loc.column;
            e.message = "`ref string_view` is not allowed";
            e.help = "A `string_view` is already a non-owning view; declare it without `ref`: `string_view " + decl->identifier + " = ...;`.";
            errors_.push_back(e);
        }
        rejectThisEscapeInConstructor(decl->expression.get(), stmt);
        checkExpression(decl->expression.get());
        localType_[decl->identifier] = decl->type;
    } else if (auto cdecl = dynamic_cast<const ContainerDeclaration*>(stmt)) {
        requireKnownType(cdecl->type, "variable `" + cdecl->identifier + "`", stmt->loc.line, stmt->loc.column);
        localType_[cdecl->identifier] = cdecl->type;
    } else if (auto assign = dynamic_cast<const AssignmentStatement*>(stmt)) {
        rejectThisEscapeInConstructor(assign->expression.get(), stmt);
        checkExpression(assign->expression.get());
        if (auto sub = dynamic_cast<const SubscriptExpression*>(assign->target.get())) {
            // `n[key] = value;` on a CLASS-typed (not container) receiver needs a
            // REFERENCE-returning `operator[]` to mean anything in real C++ (the
            // reference is what the subsequent `= value` actually assigns through).
            // CeeLing has no `ref`-return capability yet (see TODO.md/Ch. 9), so
            // this is rejected outright regardless of whether `operator[]` itself
            // exists -- read position (`x = n[key];`) has no such problem and stays
            // supported; only the write-through-subscript shape is unsupported.
            const std::string objClass = resolveObjectType(sub->object.get());
            if (!objClass.empty()) {
                EscapeHatchError e;
                e.line = stmt->loc.line;
                e.column = stmt->loc.column;
                e.message = "assignment through `operator[]` is not supported";
                e.help = "`" + sub->object->toString() + "` is a " + objClass +
                         "; `" + sub->object->toString() + "[key] = value` would need "
                         "operator[] to return a reference, which CeeLing does not yet "
                         "support (no `ref` return type). Add a named setter method "
                         "instead (e.g. `void set(key, value)`).";
                errors_.push_back(e);
            }
        }
        if (assign->target) {
            checkExpression(assign->target.get());
        }
    } else if (auto ret = dynamic_cast<const ReturnStatement*>(stmt)) {
        checkExpression(ret->expression.get());
    } else if (auto ifs = dynamic_cast<const IfStatement*>(stmt)) {
        checkExpression(ifs->condition.get());
        for (const auto& inner : ifs->body) {
            checkStatement(inner.get());
        }
        for (const auto& inner : ifs->elseBody) {
            checkStatement(inner.get());
        }
    } else if (auto whiles = dynamic_cast<const WhileStatement*>(stmt)) {
        checkExpression(whiles->condition.get());
        for (const auto& inner : whiles->body) {
            checkStatement(inner.get());
        }
    } else if (auto fors = dynamic_cast<const ForStatement*>(stmt)) {
        checkExpression(fors->container.get());
        requireKnownType(fors->loopVarType, "the loop variable `" + fors->loopVarName + "`", stmt->loc.line, stmt->loc.column);
        localType_[fors->loopVarName] = fors->loopVarType;
        for (const auto& inner : fors->body) {
            checkStatement(inner.get());
        }
    } else if (auto exprStmt = dynamic_cast<const ExpressionStatement*>(stmt)) {
        checkExpression(exprStmt->expression.get());
    } else if (auto th = dynamic_cast<const ThrowStatement*>(stmt)) {
        checkExpression(th->expression.get());
    } else if (auto trys = dynamic_cast<const TryStatement*>(stmt)) {
        // Both bodies are ordinary code: an unknown call inside `try` is exactly
        // as unchecked as one outside it. (Missing this branch let any call
        // through as long as it sat inside a try block.)
        for (const auto& inner : trys->body) {
            checkStatement(inner.get());
        }
        localType_[trys->catchIdentifier] = trys->catchType;
        for (const auto& inner : trys->catchBody) {
            checkStatement(inner.get());
        }
    }
}

} // namespace cling
