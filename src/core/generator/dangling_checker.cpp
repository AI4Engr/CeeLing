#include "dangling_checker.h"

namespace cling {

std::vector<DanglingError> DanglingChecker::check(const Program& program) {
    errors_.clear();
    classNames_.clear();
    collectClassNames(program);

    // Each top-level function / main / method body is analyzed independently.
    for (const auto& stmt : program.statements) {
        if (auto fn = dynamic_cast<const FunctionDeclaration*>(stmt.get())) {
            checkBody(fn->body);
        } else if (auto mn = dynamic_cast<const MainFunction*>(stmt.get())) {
            checkBody(mn->body);
        } else if (auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
            for (const auto& member : cls->members) {
                if (auto method = dynamic_cast<const MethodDeclaration*>(member.get())) {
                    checkBody(method->body);
                } else if (auto ctor = dynamic_cast<const ConstructorDeclaration*>(member.get())) {
                    checkBody(ctor->body);
                }
            }
        }
    }
    return errors_;
}

void DanglingChecker::collectClassNames(const Program& program) {
    for (const auto& stmt : program.statements) {
        if (auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
            classNames_.insert(cls->name);
        }
    }
}

bool DanglingChecker::isObjectType(const std::string& type) const {
    return classNames_.find(type) != classNames_.end();
}

std::string DanglingChecker::asObjectLocalName(const Expression* expr) const {
    if (auto id = dynamic_cast<const Identifier*>(expr)) {
        if (objectLocals_.find(id->name) != objectLocals_.end()) {
            return id->name;
        }
    }
    return {};
}

void DanglingChecker::checkExpressionUses(const Expression* expr) {
    if (expr == nullptr) return;

    if (auto id = dynamic_cast<const Identifier*>(expr)) {
        // Direct use of a moved-from owner.
        if (movedFrom_.find(id->name) != movedFrom_.end()) {
            DanglingError e;
            e.line = expr->loc.line;
            e.column = expr->loc.column;
            e.message = "use of moved-from value `" + id->name + "`";
            e.help = "`" + id->name + "` was moved earlier; assign a new value before using it";
            errors_.push_back(e);
            return;
        }
        // Use of an observer whose observed object is gone → dangling. The object can
        // be gone because the owner was moved away, or explicitly killed (reassignment
        // to a fresh object / scope exit → recorded in danglingObservers_).
        if (auto it = observerOf_.find(id->name); it != observerOf_.end()) {
            const std::string& owner = it->second;
            const bool ownerMoved = movedFrom_.find(owner) != movedFrom_.end();
            const bool killed = danglingObservers_.find(id->name) != danglingObservers_.end();
            if (ownerMoved || killed) {
                DanglingError e;
                e.line = expr->loc.line;
                e.column = expr->loc.column;
                e.message = "use of dangling observer `" + id->name +
                            "` (its owner `" + owner + "` is gone)";
                e.help = "`" + id->name + "` observes `" + owner +
                         "`, which no longer has a live owner";
                errors_.push_back(e);
            }
        }
        return;
    }
    if (auto member = dynamic_cast<const MemberAccess*>(expr)) {
        checkExpressionUses(member->object.get());
        return;
    }
    if (auto call = dynamic_cast<const FunctionCall*>(expr)) {
        for (const auto& arg : call->arguments) {
            checkExpressionUses(arg.get());
        }
        return;
    }
    if (auto bin = dynamic_cast<const BinaryExpression*>(expr)) {
        checkExpressionUses(bin->left.get());
        checkExpressionUses(bin->right.get());
        return;
    }
    if (auto un = dynamic_cast<const UnaryExpression*>(expr)) {
        checkExpressionUses(un->operand.get());
        return;
    }
    if (auto neww = dynamic_cast<const NewExpression*>(expr)) {
        for (const auto& arg : neww->arguments) {
            checkExpressionUses(arg.get());
        }
        return;
    }
    // Identifier/Number leaves and unmodeled nodes: nothing to check.
}

void DanglingChecker::checkBody(const std::vector<StatementPtr>& body) {
    objectLocals_.clear();
    movedFrom_.clear();
    observerNames_.clear();
    observerOf_.clear();
    danglingObservers_.clear();

    for (const auto& stmtPtr : body) {
        processStatement(stmtPtr.get());
    }
}

void DanglingChecker::endBlockScope(const std::set<std::string>& ownersBefore) {
    // Owners that appeared during the block are local to it and die at block end.
    std::set<std::string> died;
    for (const auto& owner : objectLocals_) {
        if (ownersBefore.find(owner) == ownersBefore.end()) {
            died.insert(owner);
        }
    }
    for (const auto& owner : died) {
        // Any observer of a died owner now dangles.
        for (const auto& [obs, watched] : observerOf_) {
            if (watched == owner) {
                danglingObservers_.insert(obs);
            }
        }
        objectLocals_.erase(owner);
        movedFrom_.erase(owner);
    }
}

void DanglingChecker::processStatement(const Statement* stmt) {
    if (auto decl = dynamic_cast<const VariableDeclaration*>(stmt)) {
        // First: uses on the RHS are evaluated before the declaration completes.
        checkExpressionUses(decl->expression.get());

        if (decl->isObserver) {
            // `ref Node b = ...;` — b observes its RHS (if an object local); it does
            // NOT move it. `ref Node b = nullptr;` records b as an observer with no
            // owner yet (bound later by assignment).
            observerNames_.insert(decl->identifier);
            danglingObservers_.erase(decl->identifier);
            if (isObjectType(decl->type)) {
                if (std::string owner = asObjectLocalName(decl->expression.get());
                    !owner.empty()) {
                    observerOf_[decl->identifier] = owner;
                }
            }
        } else {
            // `Node b = a;` — assigning one object local from another moves the source.
            if (isObjectType(decl->type) && decl->expression) {
                if (std::string src = asObjectLocalName(decl->expression.get()); !src.empty()) {
                    movedFrom_.insert(src);
                }
            }
            // Register the new object-typed owner; a fresh declaration revives the name.
            if (isObjectType(decl->type)) {
                objectLocals_.insert(decl->identifier);
                movedFrom_.erase(decl->identifier);
            }
        }
    } else if (auto assign = dynamic_cast<const AssignmentStatement*>(stmt)) {
        checkExpressionUses(assign->expression.get());

        if (assign->target) {
            // Member target `a.b = ...`: this stores into / links a field. It does NOT
            // reassign or move the root object, and it does NOT consume the RHS — storing
            // an object into a field is a link/share, not an ownership handoff that
            // abandons the source (a linked-list node is used again right after linking).
            // Just check the receiver and RHS aren't already dead.
            checkExpressionUses(assign->target.get());
        } else if (observerNames_.find(assign->variable) != observerNames_.end()) {
            // Assigning to an observer rebinds what it observes: `b = inner;`.
            danglingObservers_.erase(assign->variable);
            if (std::string owner = asObjectLocalName(assign->expression.get());
                !owner.empty()) {
                observerOf_[assign->variable] = owner;
            } else {
                observerOf_.erase(assign->variable);
            }
        } else {
            // Reassigning an OWNER frees the object it used to hold. Any observer of
            // that owner is now pointing at a destroyed object → mark it dangling.
            if (objectLocals_.find(assign->variable) != objectLocals_.end()) {
                for (const auto& [obs, owner] : observerOf_) {
                    if (owner == assign->variable) {
                        danglingObservers_.insert(obs);
                    }
                }
            }
            // Reassigning a local revives the name itself (no longer moved-from).
            movedFrom_.erase(assign->variable);
            // RHS being another object local moves that source.
            if (std::string src = asObjectLocalName(assign->expression.get()); !src.empty()) {
                movedFrom_.insert(src);
            }
        }
    } else if (auto ret = dynamic_cast<const ReturnStatement*>(stmt)) {
        checkExpressionUses(ret->expression.get());
    } else if (auto ifs = dynamic_cast<const IfStatement*>(stmt)) {
        checkExpressionUses(ifs->condition.get());
        // A branch runs conditionally, so a *move* that happens inside a branch is not
        // certain on the path after the `if`. We snapshot moved-from state before the
        // branches and restore it afterward, so a conditional move (e.g. `head = newNode`
        // in one arm) does not leak out and wrongly flag a later use of `newNode`. (This
        // errs toward acceptance across branches; a precise path-merge is future work.)
        // We deliberately do NOT roll back dangling-observer state: an owner dying at a
        // branch's scope exit is a real death, and a later use of an observer of it must
        // still be rejected (conservative — see the scope-exit fixture).
        const std::set<std::string> movedBefore = movedFrom_;
        {
            const std::set<std::string> ownersBefore = objectLocals_;
            for (const auto& inner : ifs->body) {
                processStatement(inner.get());
            }
            endBlockScope(ownersBefore);
        }
        if (!ifs->elseBody.empty()) {
            const std::set<std::string> ownersBefore = objectLocals_;
            for (const auto& inner : ifs->elseBody) {
                processStatement(inner.get());
            }
            endBlockScope(ownersBefore);
        }
        movedFrom_ = movedBefore;
    } else if (auto exprStmt = dynamic_cast<const ExpressionStatement*>(stmt)) {
        // A bare expression statement, e.g. `sink(a);`. First check uses, then
        // apply move effects: a user function call moves its object-typed args.
        checkExpressionUses(exprStmt->expression.get());
        if (auto call = dynamic_cast<const FunctionCall*>(exprStmt->expression.get())) {
            for (const auto& arg : call->arguments) {
                if (std::string src = asObjectLocalName(arg.get()); !src.empty()) {
                    movedFrom_.insert(src);
                }
            }
        }
    }
}

} // namespace cling
