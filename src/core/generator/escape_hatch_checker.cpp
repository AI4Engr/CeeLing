#include "escape_hatch_checker.h"
#include "library_summary.h"

namespace cling {

std::vector<EscapeHatchError> EscapeHatchChecker::check(const Program& program) {
    errors_.clear();
    classNames_.clear();
    classFields_.clear();
    knownFreeFunctions_.clear();
    classMethods_.clear();
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
                    checkBody(ctor->body, ctor->parameters, cls->name);
                }
            }
        }
    }
    return errors_;
}

void EscapeHatchChecker::collectClasses(const Program& program) {
    for (const auto& stmt : program.statements) {
        if (auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
            classNames_.insert(cls->name);
            auto& fields = classFields_[cls->name];
            for (const auto& member : cls->members) {
                if (auto field = dynamic_cast<const VariableDeclaration*>(member.get())) {
                    fields[field->identifier] = field->type;
                } else if (auto method = dynamic_cast<const MethodDeclaration*>(member.get())) {
                    if (method->name == "get") {
                        classesWithOwnGet_.insert(cls->name);
                    }
                }
            }
        }
    }
}

void EscapeHatchChecker::collectCallables(const Program& program) {
    for (const auto& stmt : program.statements) {
        if (auto fn = dynamic_cast<const FunctionDeclaration*>(stmt.get())) {
            knownFreeFunctions_.insert(fn->name);
        } else if (auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
            auto& methods = classMethods_[cls->name];
            for (const auto& member : cls->members) {
                if (auto method = dynamic_cast<const MethodDeclaration*>(member.get())) {
                    methods.insert(method->name);
                }
            }
        }
    }
}

std::string EscapeHatchChecker::resolveDeclaredType(const Expression* expr) const {
    auto id = dynamic_cast<const Identifier*>(expr);
    if (id == nullptr) return {};
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

        const std::string objClass = resolveObjectType(member->object.get());
        if (objClass.empty()) return false;
        auto it = classMethods_.find(objClass);
        return it != classMethods_.end() && it->second.count(member->member) > 0;
    }
    // Plain call `f(args)`: either a free function, or -- inside a method body -- a
    // bare same-class method call with an implicit `this` receiver.
    if (knownFreeFunctions_.count(call.function_name) > 0) return true;
    if (!currentClass_.empty()) {
        auto it = classMethods_.find(currentClass_);
        if (it != classMethods_.end() && it->second.count(call.function_name) > 0) {
            return true;
        }
    }
    return false;
}

void EscapeHatchChecker::checkBody(const std::vector<StatementPtr>& body,
                                    const std::vector<std::pair<std::string, std::string>>& params,
                                    const std::string& ownerClass) {
    localType_.clear();
    currentClass_ = ownerClass;
    for (const auto& [type, name] : params) {
        localType_[name] = type;
    }
    for (const auto& stmtPtr : body) {
        checkStatement(stmtPtr.get());
    }
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
