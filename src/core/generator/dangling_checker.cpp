#include "dangling_checker.h"

namespace cling {

std::vector<DanglingError> DanglingChecker::check(const Program& program) {
    errors_.clear();
    classNames_.clear();
    collectClassNames(program);
    summaries_.clear();
    functionParams_.clear();
    classMethodParams_.clear();
    deriveSummaries(program);

    // Each top-level function / main / method body is analyzed independently.
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

DanglingChecker::FunctionSummary DanglingChecker::deriveSummaryForBody(
    const std::vector<StatementPtr>& body,
    const std::vector<std::pair<std::string, std::string>>& params) const {
    FunctionSummary summary;

    // Only top-level `return` statements are considered (bodies have no nested
    // function/class scopes, so no need to recurse into unrelated declarations --
    // but `return` can appear inside `if`/`while`, so those ARE walked).
    std::vector<const ReturnStatement*> returns;
    std::vector<const Statement*> stack;
    for (const auto& s : body) stack.push_back(s.get());
    while (!stack.empty()) {
        const Statement* s = stack.back();
        stack.pop_back();
        if (auto ret = dynamic_cast<const ReturnStatement*>(s)) {
            returns.push_back(ret);
        } else if (auto ifs = dynamic_cast<const IfStatement*>(s)) {
            for (const auto& inner : ifs->body) stack.push_back(inner.get());
            for (const auto& inner : ifs->elseBody) stack.push_back(inner.get());
        } else if (auto whiles = dynamic_cast<const WhileStatement*>(s)) {
            for (const auto& inner : whiles->body) stack.push_back(inner.get());
        }
    }

    for (const auto* ret : returns) {
        auto id = dynamic_cast<const Identifier*>(ret->expression.get());
        if (id == nullptr) continue;
        for (size_t i = 0; i < params.size(); ++i) {
            if (params[i].second == id->name) {
                if (summary.returnBorrowsParam == -1) {
                    summary.returnBorrowsParam = static_cast<int>(i);
                } else if (summary.returnBorrowsParam != static_cast<int>(i)) {
                    // Two different returns alias two different parameters: not
                    // representable by a single index. Conservative-reject rule --
                    // keep treating the return as an alias (of the first parameter
                    // found) rather than silently downgrading to "fresh", since
                    // "fresh" would wrongly let a caller believe no argument is
                    // still observed.
                }
                break;
            }
        }
    }
    return summary;
}

void DanglingChecker::deriveSummaries(const Program& program) {
    for (const auto& stmt : program.statements) {
        if (auto fn = dynamic_cast<const FunctionDeclaration*>(stmt.get())) {
            functionParams_[fn->name] = fn->parameters;
            summaries_[summaryKey("", fn->name)] = deriveSummaryForBody(fn->body, fn->parameters);
        } else if (auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
            for (const auto& member : cls->members) {
                if (auto method = dynamic_cast<const MethodDeclaration*>(member.get())) {
                    classMethodParams_[cls->name][method->name] = method->parameters;
                    summaries_[summaryKey(cls->name, method->name)] =
                        deriveSummaryForBody(method->body, method->parameters);
                }
            }
        }
    }
}

std::string DanglingChecker::asObjectLocalName(const Expression* expr) const {
    if (auto id = dynamic_cast<const Identifier*>(expr)) {
        if (objectLocals_.find(id->name) != objectLocals_.end()) {
            return id->name;
        }
    }
    return {};
}

std::string DanglingChecker::resolveCallReturnAlias(const Expression* expr) const {
    auto call = dynamic_cast<const FunctionCall*>(expr);
    if (call == nullptr) return {};

    const FunctionSummary* summary = nullptr;
    const std::vector<std::pair<std::string, std::string>>* params = nullptr;

    if (call->callee) {
        // `recv.method(args)`: resolve the receiver's static class to find the
        // right class's method summary.
        auto member = dynamic_cast<const MemberAccess*>(call->callee.get());
        if (member == nullptr) return {};
        auto recvId = dynamic_cast<const Identifier*>(member->object.get());
        if (recvId == nullptr) return {};
        auto typeIt = localType_.find(recvId->name);
        if (typeIt == localType_.end()) return {};
        auto clsIt = classMethodParams_.find(typeIt->second);
        if (clsIt == classMethodParams_.end()) return {};
        auto paramIt = clsIt->second.find(member->member);
        if (paramIt == clsIt->second.end()) return {};
        params = &paramIt->second;
        auto sumIt = summaries_.find(summaryKey(typeIt->second, member->member));
        if (sumIt == summaries_.end()) return {};
        summary = &sumIt->second;
    } else {
        // Plain call `f(args)`.
        auto paramIt = functionParams_.find(call->function_name);
        if (paramIt == functionParams_.end()) return {};
        params = &paramIt->second;
        auto sumIt = summaries_.find(summaryKey("", call->function_name));
        if (sumIt == summaries_.end()) return {};
        summary = &sumIt->second;
    }

    if (summary->returnBorrowsParam < 0) return {};
    const size_t idx = static_cast<size_t>(summary->returnBorrowsParam);
    if (idx >= call->arguments.size() || idx >= params->size()) return {};
    return asObjectLocalName(call->arguments[idx].get());
}

void DanglingChecker::applyCallArgMoves(const Expression* expr) {
    auto call = dynamic_cast<const FunctionCall*>(expr);
    if (call == nullptr) return;
    for (const auto& arg : call->arguments) {
        if (std::string src = asObjectLocalName(arg.get()); !src.empty()) {
            movedFrom_.insert(src);
        }
    }
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
            // An alias observer's owner is moved-from BY CONSTRUCTION (the call that
            // produced the alias moved its argument) -- that alone doesn't mean the
            // underlying object is gone, only that its explicit death events do.
            const bool isAlias = aliasObservers_.find(id->name) != aliasObservers_.end();
            const bool ownerMoved = !isAlias && movedFrom_.find(owner) != movedFrom_.end();
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

void DanglingChecker::checkBody(const std::vector<StatementPtr>& body,
                                 const std::vector<std::pair<std::string, std::string>>& params,
                                 const std::string& ownerClass) {
    objectLocals_.clear();
    localType_.clear();
    movedFrom_.clear();
    observerNames_.clear();
    observerOf_.clear();
    danglingObservers_.clear();
    aliasObservers_.clear();
    currentClass_ = ownerClass;

    // Parameters are always-moved-into owners (CeeLing bans `ref` parameters), so an
    // object-typed parameter participates in move tracking exactly like a local
    // declared via `Node x = ...;` -- register it the same way.
    for (const auto& [type, name] : params) {
        if (isObjectType(type)) {
            objectLocals_.insert(name);
            localType_[name] = type;
        }
    }

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
                localType_[decl->identifier] = decl->type;
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
            // `Node b = f(a);` — a call RHS moves its own object-typed arguments into
            // the callee, same as any other call (see applyCallArgMoves).
            applyCallArgMoves(decl->expression.get());
            // `Node b = f(a);` where f's summary says its return value aliases the
            // parameter that `a` was passed to: `a` is still moved into `f` as usual
            // (call-argument move, applied below alongside all other call args), but
            // `b` is NOT a fresh owner -- it observes `a`'s object, the same way an
            // explicit `ref` would. Reuse the observer machinery directly so the
            // existing use-site/death-event checks cover it for free.
            std::string aliasedOwner;
            if (isObjectType(decl->type) && decl->expression) {
                aliasedOwner = resolveCallReturnAlias(decl->expression.get());
            }
            if (!aliasedOwner.empty()) {
                observerNames_.insert(decl->identifier);
                danglingObservers_.erase(decl->identifier);
                observerOf_[decl->identifier] = aliasedOwner;
                aliasObservers_.insert(decl->identifier);
                localType_[decl->identifier] = decl->type;
            } else if (isObjectType(decl->type)) {
                // Register the new object-typed owner; a fresh declaration revives
                // the name.
                objectLocals_.insert(decl->identifier);
                movedFrom_.erase(decl->identifier);
                localType_[decl->identifier] = decl->type;
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
            // Assigning to an observer rebinds what it observes: `b = inner;`. A
            // plain rebind is a real (non-alias) observation, so it no longer gets
            // the alias exemption even if `b` was originally alias-derived.
            danglingObservers_.erase(assign->variable);
            aliasObservers_.erase(assign->variable);
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
            // `x = f(a);` — a call RHS moves its own object-typed arguments, same as
            // any other call site.
            applyCallArgMoves(assign->expression.get());
            // `x = f(a);` where f's summary says the return aliases a parameter: `x`
            // becomes an observer of that argument instead of an independent owner,
            // same reasoning as the VariableDeclaration case above.
            if (objectLocals_.find(assign->variable) != objectLocals_.end()) {
                if (std::string aliasedOwner = resolveCallReturnAlias(assign->expression.get());
                    !aliasedOwner.empty()) {
                    objectLocals_.erase(assign->variable);
                    observerNames_.insert(assign->variable);
                    danglingObservers_.erase(assign->variable);
                    observerOf_[assign->variable] = aliasedOwner;
                    aliasObservers_.insert(assign->variable);
                }
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
        // Each branch starts fresh from the pre-if state (not polluted by the sibling
        // branch's moves) since only one branch actually runs. Only owner-liveness
        // (objectLocals_) carries forward normally within a branch; moved-from state
        // is independently snapshotted/restored per branch, then merged conservatively:
        // a name moved in EITHER branch is moved after the `if` (a use after the `if`
        // can't know which branch ran).
        const std::set<std::string> movedBefore = movedFrom_;
        std::set<std::string> movedAfterThen = movedBefore;
        {
            const std::set<std::string> ownersBefore = objectLocals_;
            for (const auto& inner : ifs->body) {
                processStatement(inner.get());
            }
            endBlockScope(ownersBefore);
            movedAfterThen = movedFrom_;
        }
        movedFrom_ = movedBefore;
        if (!ifs->elseBody.empty()) {
            const std::set<std::string> ownersBefore = objectLocals_;
            for (const auto& inner : ifs->elseBody) {
                processStatement(inner.get());
            }
            endBlockScope(ownersBefore);
        }
        movedFrom_.insert(movedAfterThen.begin(), movedAfterThen.end());
    } else if (auto whiles = dynamic_cast<const WhileStatement*>(stmt)) {
        checkExpressionUses(whiles->condition.get());
        // Same conservatism as `if`: the loop body may run zero or more times, so a
        // move inside it is not certain on the path after the loop -- snapshot/restore
        // moved-from state around it (dangling-observer state is not rolled back, same
        // reasoning as the `if` branch above).
        const std::set<std::string> movedBefore = movedFrom_;
        const std::set<std::string> ownersBefore = objectLocals_;
        for (const auto& inner : whiles->body) {
            processStatement(inner.get());
        }
        endBlockScope(ownersBefore);
        movedFrom_ = movedBefore;
    } else if (auto exprStmt = dynamic_cast<const ExpressionStatement*>(stmt)) {
        // A bare expression statement, e.g. `sink(a);`. First check uses, then
        // apply move effects: a user function call moves its object-typed args.
        checkExpressionUses(exprStmt->expression.get());
        applyCallArgMoves(exprStmt->expression.get());
    }
}

} // namespace cling
