#include "escape_hatch_checker.h"
#include "class_hierarchy.h"
#include "library_summary.h"

namespace cling {

std::vector<EscapeHatchError> EscapeHatchChecker::check(const Program& program) {
    errors_.clear();
    classNames_.clear();
    classFields_.clear();
    knownFreeFunctions_.clear();
    classMethods_.clear();
    classSpecializationArities_.clear();
    collectClasses(program);
    collectCallables(program);

    for (const auto& stmt : program.statements) {
        if (auto fn = dynamic_cast<const FunctionDeclaration*>(stmt.get())) {
            checkBody(fn->body, fn->parameters, "");
        } else if (auto mn = dynamic_cast<const MainFunction*>(stmt.get())) {
            checkBody(mn->body, {}, "");
        } else if (auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
            for (const auto& member : cls->members) {
                if (auto method = dynamic_cast<const MethodDeclaration*>(member.get())) {
                    checkBody(method->body, method->parameters, cls->name);
                } else if (auto ctor = dynamic_cast<const ConstructorDeclaration*>(member.get())) {
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
            // Dual-register under the qualified name (`YAML::Load`) -- a call
            // site inside a DIFFERENT namespace (or at global scope) spells it
            // qualified, same reasoning as ClassDeclaration's identical
            // dual-registration for `YAML::Node`.
            if (!fn->namespaceName.empty()) {
                knownFreeFunctions_.insert(fn->namespaceName + "::" + fn->name);
            }
        }
    }
    // Same base-before-derived flattening as collectClasses: without it,
    // `d.baseMethod()` would fail isKnownCallable's exact lookup and be rejected by
    // the default-reject-unknown-calls rule -- a safe failure, but a false one.
    for (const ClassDeclaration* cls : classesInBaseFirstOrder(program)) {
        auto& methods = classMethods_[cls->name];
        auto& virtuals = classVirtualMethods_[cls->name];
        if (!cls->baseClass.empty()) {
            if (auto baseIt = classMethods_.find(cls->baseClass); baseIt != classMethods_.end()) {
                methods = baseIt->second;
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
            return isKnownVectorMethod(member->member);
        }
        if (isStringType(declaredType)) {
            return isKnownStringMethod(member->member);
        }
        // A known-external class (e.g. Eigen::MatrixXd) calling one of its
        // allow-listed borrowing methods (`.block()`, `.row()`, ...) -- see
        // library_summary.h's isKnownBorrowingMethod doc comment. Checked
        // before the CeeLing-own-class path below since a known-external type
        // never has a classMethods_ entry (nothing parses its declaration).
        if (!declaredType.empty() && isKnownExternalClassType(declaredType)) {
            return isKnownBorrowingMethod(declaredType, member->member);
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
    // Calls, `new`, literals, etc: not resolved by this lightweight pass.
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
        for (const auto& arg : neww->arguments) {
            checkExpression(arg.get());
        }
        return;
    }
    // Identifier/Number/ScopedIdentifier/StringLiteral leaves: nothing to check.
}

void EscapeHatchChecker::checkStatement(const Statement* stmt) {
    if (auto decl = dynamic_cast<const VariableDeclaration*>(stmt)) {
        checkExpression(decl->expression.get());
        localType_[decl->identifier] = decl->type;
    } else if (auto cdecl = dynamic_cast<const ContainerDeclaration*>(stmt)) {
        localType_[cdecl->identifier] = cdecl->type;
    } else if (auto assign = dynamic_cast<const AssignmentStatement*>(stmt)) {
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
        localType_[fors->loopVarName] = fors->loopVarType;
        for (const auto& inner : fors->body) {
            checkStatement(inner.get());
        }
    } else if (auto exprStmt = dynamic_cast<const ExpressionStatement*>(stmt)) {
        checkExpression(exprStmt->expression.get());
    }
}

} // namespace cling
