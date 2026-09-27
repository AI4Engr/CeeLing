#include "dangling_checker.h"
#include "class_hierarchy.h"
#include "library_summary.h"

namespace cling {

namespace {
// True iff `body` is guaranteed to leave its enclosing function before falling
// through to whatever statement follows it (a `return`/`throw` as its LAST
// statement, or an `if`/`else` whose BOTH arms always exit). Used by the
// IfStatement branch-merge below to avoid a false use-after-move: a move
// inside a branch that unconditionally returns can never be observed by code
// AFTER the `if` (that code is only reachable via the OTHER path), so its
// effects must not be merged into the post-`if` state the way an ordinary
// (fall-through) branch's effects are. Confirmed as a real, pre-existing gap
// (not introduced by RB18 -- reproduced on an ordinary flat class with no
// NodeData/handle split at all) via a minimal probe: `if (x) { other(n);
// return; } n.use();` wrongly flagged `n` as moved-from at the LAST line,
// even though that line is only reachable when the `if` was false. Only
// looks at the LAST statement (conservative: an early return nested deeper
// inside, say, a `while`, does not make the enclosing block itself
// unconditionally exit, since the loop might not execute that path) --
// deliberately narrow, matching this checker's overall "ambiguous ->
// conservative reject" bias elsewhere; a genuinely exiting shape this misses
// just falls back to the pre-existing (safe, if occasionally over-strict)
// merge behavior.
bool alwaysExits(const std::vector<StatementPtr>& body) {
    if (body.empty()) return false;
    const Statement* last = body.back().get();
    if (dynamic_cast<const ReturnStatement*>(last) != nullptr) return true;
    if (dynamic_cast<const ThrowStatement*>(last) != nullptr) return true;
    if (auto ifs = dynamic_cast<const IfStatement*>(last)) {
        return !ifs->elseBody.empty() && alwaysExits(ifs->body) && alwaysExits(ifs->elseBody);
    }
    return false;
}
} // namespace

std::vector<DanglingError> DanglingChecker::check(const Program& program, const SmartPointerAnalyzer& analyzer) {
    errors_.clear();
    classNames_.clear();
    classFields_.clear();
    collectClassNames(program);
    summaries_.clear();
    functionParams_.clear();
    classMethodParams_.clear();
    deriveSummaries(program);
    analyzer_ = &analyzer;

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
    // Base-before-derived so classFields_ is FLAT (a derived class's entry already
    // contains its base's fields). Without this, a derived class's method body
    // wouldn't recognize `inheritedField = param;` as a field-store and would treat
    // it as an ordinary local reassignment -- the exact bug Step 19 fixed for
    // non-inherited fields, reappearing one level up the hierarchy.
    for (const ClassDeclaration* cls : classesInBaseFirstOrder(program)) {
        classNames_.insert(cls->name);
        auto& fields = classFields_[cls->name];
        if (!cls->baseClass.empty()) {
            if (auto baseIt = classFields_.find(cls->baseClass); baseIt != classFields_.end()) {
                fields = baseIt->second;
            }
        }
        for (const auto& member : cls->members) {
            if (auto field = dynamic_cast<const VariableDeclaration*>(member.get())) {
                fields.insert(field->identifier);
            }
        }

        // Dual-register under the qualified name, mirroring AstEmitter/
        // EscapeHatchChecker's collection passes: without this, a `YAML::Node`-typed
        // local/parameter fails isObjectType's classNames_ lookup and is silently
        // NEVER move-tracked at all -- not a safe over-rejection, a missed catch
        // (confirmed by a probe: `sink(a); use(a);` on a namespaced type was wrongly
        // accepted before this fix).
        if (!cls->namespaceName.empty()) {
            const std::string qualified = cls->namespaceName + "::" + cls->name;
            classNames_.insert(qualified);
            classFields_[qualified] = fields;
        }
    }
}

bool DanglingChecker::isObjectType(const std::string& type) const {
    // std::map<K,V> participates in move/observer tracking the same way
    // std::vector<T> does -- both are container types where "did you still have
    // access to the old one after passing it to a function" is a real, checkable
    // question, independent of what's inside them (this slice restricts map K/V to
    // primitives, but the container variable itself is exactly as move-trackable as
    // a vector variable). `string` deliberately stays excluded: a bare `string`
    // local is a plain C++ value type with cheap real copies everywhere, not a
    // container whose "old" and "new" handles could diverge.
    //
    // A value-type class (struct keyword, Iterator-shaped structural case, or a
    // known-external value type like Eigen::MatrixXd -- see SmartPointerAnalyzer::
    // isValueTypeClass) DOES still participate in move/observer tracking -- it is
    // move-CONSTRUCTED into a callee exactly like a class-typed local
    // (AstEmitter emits `sink(std::move(a));` for a value-type argument same as
    // any other object type), so a real use-after-move is possible for any value
    // type owning heap storage (confirmed via probe: Eigen::MatrixXd, which has
    // genuine move semantics, silently accepted `sink(a); use(a);` with zero
    // errors before this fix -- a live use-after-move gap, not a false-positive
    // guard). What must NOT be tracked as a move is a plain declaration-copy from
    // another value-type local (`Foo b = a; Foo c = a;` are both ordinary, legal
    // COPIES, not moves) -- that distinction is handled at the one call site that
    // registers a copy-declaration as a move (see the VariableDeclaration
    // else-branch in processStatement), not here by excluding value types from
    // tracking altogether the way the previous version of this function did.
    return classNames_.find(type) != classNames_.end() || isVectorType(type) ||
           isMapType(type) ||
           (analyzer_ != nullptr && analyzer_->isValueTypeClass(type));
}

bool DanglingChecker::isStringValuedExpr(const Expression* expr) const {
    if (expr == nullptr) return false;
    if (dynamic_cast<const StringLiteral*>(expr) != nullptr) return true;
    if (auto id = dynamic_cast<const Identifier*>(expr)) {
        auto typeIt = localType_.find(id->name);
        return typeIt != localType_.end() && isStringType(typeIt->second);
    }
    if (auto bin = dynamic_cast<const BinaryExpression*>(expr)) {
        return bin->operator_str == "+" &&
               isStringValuedExpr(bin->left.get()) &&
               isStringValuedExpr(bin->right.get());
    }
    return false;
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
            functionReturnType_[fn->name] = fn->returnType;
            summaries_[summaryKey("", fn->name)] = deriveSummaryForBody(fn->body, fn->parameters);
            // Dual-register under the qualified name (`YAML::Load`), same
            // reasoning as every other namespaced-declaration dual-
            // registration in this codebase: a call site outside the
            // namespace spells it qualified, and resolveCallReturnAlias's
            // plain-call branch looks up functionParams_/summaries_ keyed by
            // exactly that spelling.
            if (!fn->namespaceName.empty()) {
                const std::string qualified = fn->namespaceName + "::" + fn->name;
                functionParams_[qualified] = fn->parameters;
                functionReturnType_[qualified] = fn->returnType;
                summaries_[summaryKey("", qualified)] = summaries_[summaryKey("", fn->name)];
            }
        } else if (auto extFn = dynamic_cast<const ExternFunctionDeclaration*>(stmt.get())) {
            // No body to derive a summary FROM -- the summary is the hand-
            // declared `borrows N` fact itself (or -1, fresh/owned, matching
            // FunctionSummary's own default). Still registered into
            // functionParams_/functionReturnType_ exactly like an ordinary
            // FunctionDeclaration, since resolveCallReturnAlias and argument
            // move-application key off those same maps regardless of which
            // kind of declaration produced them.
            functionParams_[extFn->name] = extFn->parameters;
            functionReturnType_[extFn->name] = extFn->returnType;
            FunctionSummary summary;
            summary.returnBorrowsParam = extFn->returnBorrowsParam;
            summaries_[summaryKey("", extFn->name)] = summary;
            // `ref` params are borrowed for the call's duration, not moved --
            // see applyCallArgMoves's outParamIdx-shaped exemption, which
            // skips inserting these indices into movedFrom_.
            externRefParams_[extFn->name] = extFn->refParams;
        }
    }
    // Base-before-derived again, for the same reason: `d.inheritedMethod(x)` has to
    // find both the parameter list (to resolve which argument binds where) and the
    // derived summary. An inherited method that the derived class does NOT override
    // keeps the base's summary verbatim -- it is literally the same body.
    for (const ClassDeclaration* cls : classesInBaseFirstOrder(program)) {
        auto& params = classMethodParams_[cls->name];
        auto& returnTypes = classMethodReturnType_[cls->name];
        if (!cls->baseClass.empty()) {
            if (auto baseIt = classMethodParams_.find(cls->baseClass);
                baseIt != classMethodParams_.end()) {
                params = baseIt->second;
                for (const auto& [methodName, _] : baseIt->second) {
                    auto baseSummary = summaries_.find(summaryKey(cls->baseClass, methodName));
                    if (baseSummary != summaries_.end()) {
                        summaries_[summaryKey(cls->name, methodName)] = baseSummary->second;
                    }
                }
            }
            if (auto baseIt = classMethodReturnType_.find(cls->baseClass);
                baseIt != classMethodReturnType_.end()) {
                returnTypes = baseIt->second;
            }
        }
        // See AstEmitter::collectClassNames's identical pre-count -- only a
        // genuinely overloaded method name (declared more than once) needs the
        // arity-qualified key; every other method keeps its plain bare-name key.
        std::map<std::string, int> nameCounts;
        for (const auto& member : cls->members) {
            if (auto method = dynamic_cast<const MethodDeclaration*>(member.get())) {
                if (method->specializationArg.empty()) nameCounts[method->name]++;
            }
        }
        for (const auto& member : cls->members) {
            if (auto method = dynamic_cast<const MethodDeclaration*>(member.get())) {
                // A specialization registers under its synthetic key ("as<int>"),
                // not the bare method name -- see EscapeHatchChecker's identical
                // reasoning. A call resolves to the right specialization's
                // params/summary only when its own templateArg constructs the
                // matching key (see resolveCallReturnAlias/applyCallArgMoves below).
                // An OVERLOADED (same-name, different-arity) ordinary method keys
                // by overloadKey(name, paramCount) instead, so a second
                // declaration doesn't silently overwrite the first's params/
                // summary (see class_hierarchy.h's overloadKey).
                std::string key;
                if (!method->specializationArg.empty()) {
                    key = specializationKey(method->name, method->specializationArg);
                } else if (nameCounts[method->name] > 1) {
                    key = overloadKey(method->name, method->parameters.size());
                } else {
                    key = method->name;
                }
                params[key] = method->parameters;
                returnTypes[key] = method->returnType;
                summaries_[summaryKey(cls->name, key)] =
                    deriveSummaryForBody(method->body, method->parameters);
                // The PRIMARY generic (typeParameter set) with T used as a
                // parameter type, and ONLY when T is NOT ALSO the return
                // type -- see buildMethodDeclaration's validation. When T
                // IS the return type (`<T> T as(T fallback)`), a T-typed
                // parameter is an ORDINARY by-value parameter (T isn't a
                // real class type, so it already emits as a bare, verbatim
                // value -- no out-parameter lowering needed or wanted; doing
                // so anyway would wrongly turn `as(T fallback)`'s fallback
                // into a raw pointer).
                if (!method->typeParameter.empty() && method->returnType != method->typeParameter) {
                    for (size_t i = 0; i < method->parameters.size(); ++i) {
                        if (method->parameters[i].first == method->typeParameter) {
                            outParamIndex_[outParamKey(cls->name, method->name)] = i;
                            break;
                        }
                    }
                }
            }
        }

        // Dual-register under the qualified name: resolveCallReturnAlias looks up
        // classMethodParams_/summaries_ keyed by the RECEIVER's declared type string
        // (localType_'s value), which is "YAML::Node" for a `YAML::Node n;` local --
        // without this, a summary-derived alias on a namespaced type's method call
        // would silently resolve to nothing instead of the real summary.
        if (!cls->namespaceName.empty()) {
            const std::string qualified = cls->namespaceName + "::" + cls->name;
            classMethodParams_[qualified] = params;
            classMethodReturnType_[qualified] = returnTypes;
            for (const auto& [methodName, _] : params) {
                if (auto it = summaries_.find(summaryKey(cls->name, methodName)); it != summaries_.end()) {
                    summaries_[summaryKey(qualified, methodName)] = it->second;
                }
            }
            // Same reasoning, for outParamIndex_: applyCallArgMoves looks
            // this up keyed by the RECEIVER's declared type string, which is
            // "YAML::Node" for a `YAML::Node n;` local -- without this dual
            // registration, an out-parameter call through a namespaced
            // receiver falsely looked un-registered, so its argument was
            // wrongly treated as moved (confirmed via probe: yaml-cling's
            // real YAML::Node::decodeInto<T> case).
            const std::string ownPrefix = cls->name + "::";
            std::vector<std::pair<std::string, size_t>> requalified;
            for (const auto& [key, idx] : outParamIndex_) {
                if (key.compare(0, ownPrefix.size(), ownPrefix) == 0) {
                    requalified.emplace_back(qualified + "::" + key.substr(ownPrefix.size()), idx);
                }
            }
            for (auto& [key, idx] : requalified) {
                outParamIndex_[key] = idx;
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

bool DanglingChecker::isOperatorAssignMutation(const AssignmentStatement& assign) const {
    // Only a bare-identifier target reaches here (member/subscript targets are
    // handled by earlier branches); `assign->target` is null in that case, so
    // `assign.variable` is the whole LHS name.
    auto typeIt = localType_.find(assign.variable);
    if (typeIt == localType_.end()) return false;
    const std::string& lhsClass = typeIt->second;
    if (classNames_.find(lhsClass) == classNames_.end()) return false;  // not a class (container/primitive)
    auto methodIt = classMethodParams_.find(lhsClass);
    if (methodIt == classMethodParams_.end() || methodIt->second.count("operator=") == 0) {
        return false;  // this class has no operator= -- ordinary reassignment path
    }
    // Same-class RHS (`n = otherNodeLocal;`) is ordinary ownership transfer, not a
    // mutation -- only a type-mismatched RHS (RHS isn't an object local of this
    // same class) means operator=.
    if (auto rhsId = dynamic_cast<const Identifier*>(assign.expression.get())) {
        auto rhsTypeIt = localType_.find(rhsId->name);
        if (rhsTypeIt != localType_.end() && rhsTypeIt->second == lhsClass) {
            return false;
        }
    }
    return true;
}

std::string DanglingChecker::asContainerSubscript(const Expression* expr) const {
    auto sub = dynamic_cast<const SubscriptExpression*>(expr);
    if (sub == nullptr) return {};
    // The container itself must be a bare, live, container-typed local -- matches
    // asObjectLocalName's own restriction to bare identifiers (this checker does
    // lightweight resolution, not full type inference; `f()[i]` or `a.v[i]` are not
    // resolved by this lightweight pass, same conservatism as elsewhere).
    auto id = dynamic_cast<const Identifier*>(sub->object.get());
    if (id == nullptr) return {};
    if (objectLocals_.find(id->name) == objectLocals_.end()) return {};
    auto typeIt = localType_.find(id->name);
    if (typeIt == localType_.end() || !isVectorType(typeIt->second)) return {};
    return id->name;
}

std::string DanglingChecker::resolveObservedOwner(const Expression* expr) const {
    if (std::string owner = asObjectLocalName(expr); !owner.empty()) return owner;
    return asContainerSubscript(expr);
}

std::string DanglingChecker::resolveClassOfExpr(const Expression* expr) const {
    if (expr == nullptr) return {};
    if (auto id = dynamic_cast<const Identifier*>(expr)) {
        auto typeIt = localType_.find(id->name);
        if (typeIt == localType_.end()) return {};
        return classNames_.count(typeIt->second) ? typeIt->second : std::string{};
    }
    if (auto call = dynamic_cast<const FunctionCall*>(expr)) {
        if (!call->callee) {
            auto fnIt = functionReturnType_.find(call->function_name);
            if (fnIt == functionReturnType_.end()) return {};
            return classNames_.count(fnIt->second) ? fnIt->second : std::string{};
        }
        auto member = dynamic_cast<const MemberAccess*>(call->callee.get());
        if (member == nullptr) return {};
        std::string recvClass = resolveClassOfExpr(member->object.get());
        if (recvClass.empty()) return {};
        auto clsIt = classMethodReturnType_.find(recvClass);
        if (clsIt == classMethodReturnType_.end()) return {};
        auto methodIt = clsIt->second.find(member->member);
        if (methodIt == clsIt->second.end()) return {};
        const std::string& returnType = methodIt->second;
        return classNames_.count(returnType) ? returnType : std::string{};
    }
    // MemberAccess (bare field chains), `new`, literals, etc: not resolved by
    // this lightweight pass -- same pre-existing conservative fallback every
    // caller of this function already relies on (a field's declared TYPE is
    // not tracked anywhere in this checker, only its NAME via classFields_).
    return {};
}

std::string DanglingChecker::resolveCallReturnAlias(const Expression* expr) const {
    auto call = dynamic_cast<const FunctionCall*>(expr);
    if (call == nullptr) return {};

    const FunctionSummary* summary = nullptr;
    const std::vector<std::pair<std::string, std::string>>* params = nullptr;

    if (call->callee) {
        // `recv.method(args)`: resolve the receiver's static class to find the
        // right class's method summary. resolveClassOfExpr handles both a bare
        // identifier receiver AND a CHAINED call receiver (`a.b().c(...)`) --
        // see its own doc comment for why widening to a call receiver is safe.
        auto member = dynamic_cast<const MemberAccess*>(call->callee.get());
        if (member == nullptr) return {};
        std::string recvClass = resolveClassOfExpr(member->object.get());
        if (recvClass.empty()) return {};
        auto clsIt = classMethodParams_.find(recvClass);
        if (clsIt == classMethodParams_.end()) return {};
        // A call carrying a type argument resolves to the specialization's own
        // key -- see EscapeHatchChecker::isKnownCallable's identical reasoning.
        std::string methodKey = call->templateArg.empty()
            ? member->member
            : specializationKey(member->member, call->templateArg);
        auto paramIt = clsIt->second.find(methodKey);
        if (paramIt == clsIt->second.end() && call->templateArg.empty()) {
            // Same overload-key retry as AstEmitter::calleeParam -- the bare name
            // may have been registered under an arity key instead (deriveSummaries
            // only does this for a genuinely overloaded method name).
            methodKey = overloadKey(member->member, call->arguments.size());
            paramIt = clsIt->second.find(methodKey);
        }
        if (paramIt == clsIt->second.end()) return {};
        params = &paramIt->second;
        auto sumIt = summaries_.find(summaryKey(recvClass, methodKey));
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

std::string DanglingChecker::resolveBorrowingMethodReceiver(const Expression* expr) const {
    auto call = dynamic_cast<const FunctionCall*>(expr);
    if (call == nullptr || !call->callee) return {};
    auto member = dynamic_cast<const MemberAccess*>(call->callee.get());
    if (member == nullptr) return {};
    auto recvId = dynamic_cast<const Identifier*>(member->object.get());
    if (recvId == nullptr) return {};
    auto typeIt = localType_.find(recvId->name);
    if (typeIt == localType_.end()) return {};
    if (!isKnownExternalClassType(typeIt->second)) return {};
    if (!isKnownBorrowingMethod(typeIt->second, member->member)) return {};
    return recvId->name;
}

void DanglingChecker::applyCallArgMoves(const Expression* expr) {
    auto call = dynamic_cast<const FunctionCall*>(expr);
    if (call == nullptr) return;

    // A known invalidating call (push_back) kills every existing observer of its
    // container receiver first -- runs alongside the argument-move logic below
    // since both are driven by the exact same call expression, and every call
    // site in this file that calls applyCallArgMoves needs this too.
    invalidateContainerObservers(expr);

    // A known library method may SHARE an argument instead of moving it (e.g.
    // `v.push_back(x)` links x into the container, the same way storing into a
    // field does not consume the RHS -- see library_summary.h). Resolve the
    // receiver's declared type/name once so each argument can check the per-index
    // rule.
    std::string receiverType;
    std::string receiverName;
    std::string methodName;
    if (call->callee) {
        if (auto member = dynamic_cast<const MemberAccess*>(call->callee.get())) {
            methodName = member->member;
            if (auto id = dynamic_cast<const Identifier*>(member->object.get())) {
                receiverName = id->name;
                if (auto it = localType_.find(id->name); it != localType_.end()) {
                    receiverType = it->second;
                }
            }
        }
    }
    const bool receiverIsVector = !receiverType.empty() && isVectorType(receiverType);
    // A mutable out-parameter (see outParamIndex_) is never moved -- the
    // callee writes through it but the caller's argument survives.
    size_t outParamIdx = call->arguments.size();  // sentinel: no out-parameter
    if (!receiverType.empty()) {
        if (auto it = outParamIndex_.find(outParamKey(receiverType, methodName));
            it != outParamIndex_.end()) {
            outParamIdx = it->second;
        }
    }
    // ext_fn `ref` arguments (see externRefParams_) are borrowed for the
    // call's duration, not moved -- same exemption shape as outParamIdx
    // above. ext_fn is always a free function, so this only applies on the
    // plain-call path (receiverType empty, function_name set).
    const std::set<size_t>* refParams = nullptr;
    if (receiverType.empty()) {
        if (auto it = externRefParams_.find(call->function_name); it != externRefParams_.end()) {
            refParams = &it->second;
        }
    }

    for (size_t i = 0; i < call->arguments.size(); ++i) {
        if (i == outParamIdx) continue;
        if (refParams != nullptr && refParams->count(i)) continue;
        if (receiverIsVector && vectorMethodSharesArg(methodName, i)) {
            // A container's element pointer kind is decided once, across every
            // push_back call site targeting it (SmartPointerAnalyzer::
            // containerElementIsShared) -- vector<unique_ptr<T>> and
            // vector<shared_ptr<T>> are different C++ types, so it can't vary per
            // call. Whether THIS specific push shares or moves its argument
            // depends on whether the argument's OWN pointer kind already matches:
            //   - element=SHARED, arg already SHARED -> share (copy, arg survives)
            //   - element=SHARED, arg is UNIQUE       -> must move (wrap-construct
            //     a shared_ptr from the unique_ptr, consuming it)
            //   - element=UNIQUE                       -> always moves
            const std::string argName = asObjectLocalName(call->arguments[i].get());
            const bool elementIsShared = analyzer_ != nullptr &&
                analyzer_->containerElementIsShared(receiverName);
            const bool argIsShared = analyzer_ != nullptr && !argName.empty() &&
                analyzer_->getPointerType(localType_.count(argName) ? localType_.at(argName) : "",
                                          argName) == PointerType::SHARED;
            if (elementIsShared && argIsShared) {
                continue;  // shares -- the argument stays a live owner
            }
        }

        // Blanket rule, independent of the receiver/method: an argument whose OWN
        // pointer type already resolves to SHARED never needs to move into ANY
        // call (method, constructor via NewExpression at the VariableDeclaration/
        // AssignmentStatement call sites, or free function) -- a std::shared_ptr
        // is always safely copyable, so the source stays a live, usable owner
        // regardless of what the callee does with its own copy. This generalizes
        // the vector-specific share case above (which additionally has to decide
        // whether a UNIQUE argument gets wrapped into a shared_ptr construction,
        // still handled by that branch) to the ordinary case: a parameter that a
        // method/constructor body stores into an already-shared field is promoted
        // to SHARED by SmartPointerAnalyzer's own third pass (see Chapter 18), and
        // once that's true, the argument bound to it is ALSO promoted to SHARED by
        // that same pass -- so checking the argument's own pointer type here is
        // sufficient, with no need to separately resolve the callee's summary.
        //
        // Restricted to genuinely class-typed arguments: SmartPointerAnalyzer::
        // getPointerType's suffix-match has no entry at all for a container type
        // like "std::vector<Node>" (pointerDecisions is only ever keyed by real
        // class names), and its not-found fallback defaults to SHARED -- applying
        // this rule to a container argument would incorrectly skip moving it
        // (confirmed by a regression: vector_moved_into_call_rejected.clg went
        // from PASS to XFAIL without this guard). A container's own move-vs-share
        // question is exactly what the vector-specific branch above already
        // handles per-method; this blanket rule must not also apply to it.
        const std::string argName = asObjectLocalName(call->arguments[i].get());
        const std::string argType = localType_.count(argName) ? localType_.at(argName) : "";
        if (analyzer_ != nullptr && !argName.empty() && classNames_.count(argType) > 0 &&
            analyzer_->getPointerType(argType, argName) == PointerType::SHARED) {
            continue;
        }

        if (std::string src = asObjectLocalName(call->arguments[i].get()); !src.empty()) {
            movedFrom_.insert(src);
        }
    }
}

void DanglingChecker::invalidateContainerObservers(const Expression* callExpr) {
    auto call = dynamic_cast<const FunctionCall*>(callExpr);
    if (call == nullptr || !call->callee) return;
    auto member = dynamic_cast<const MemberAccess*>(call->callee.get());
    if (member == nullptr) return;
    auto id = dynamic_cast<const Identifier*>(member->object.get());
    if (id == nullptr) return;

    auto typeIt = localType_.find(id->name);
    if (typeIt == localType_.end() || !isVectorType(typeIt->second)) return;
    if (!vectorMethodInvalidatesReferences(member->member)) return;

    // The container may have reallocated -- every existing observer of it (`ref
    // Node n = v[i];`) is now dangling, the same way endBlockScope kills observers
    // of an owner whose scope just ended, and the AssignmentStatement branch kills
    // observers of a reassigned owner. Reuses danglingObservers_ (already unioned
    // conservatively across branches by mergeBranchStates), so a call inside only
    // one `if` arm still invalidates on both paths after the merge.
    for (const auto& [obs, owner] : observerOf_) {
        if (owner == id->name) {
            danglingObservers_.insert(obs);
        }
    }
}

void DanglingChecker::checkReturnedReference(const Expression* expr) {
    auto id = dynamic_cast<const Identifier*>(expr);
    if (id == nullptr) return;
    if (observerNames_.find(id->name) == observerNames_.end()) return;

    DanglingError e;
    e.line = expr->loc.line;
    e.column = expr->loc.column;
    e.message = "returning observer `" + id->name + "` (a `ref`) escapes its function";
    e.help = "`" + id->name + "` is a non-owning reference into this function's own "
             "stack frame; CeeLing has no `ref` return type, so the caller would "
             "receive a reference to an object that no longer exists. Return the "
             "owner itself, or restructure so the caller creates its own observer.";
    errors_.push_back(e);
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
    if (auto sub = dynamic_cast<const SubscriptExpression*>(expr)) {
        checkExpressionUses(sub->object.get());
        checkExpressionUses(sub->index.get());
        return;
    }
    if (auto call = dynamic_cast<const FunctionCall*>(expr)) {
        // A method call's RECEIVER is a use too (`v.size()` after `v` was moved
        // must be rejected the same way `v.field` already is via the MemberAccess
        // branch above) -- pre-existing gap found while testing Phase 2.6 container
        // fixtures: `call->callee` (the `recv.method` MemberAccess) was never
        // walked here, only the call arguments. Not container-specific -- the same
        // gap applied to any class-object method call on a moved-from receiver.
        if (call->callee) {
            checkExpressionUses(call->callee.get());
        }
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
        } else if (isStringType(type)) {
            // Same reasoning as the VariableDeclaration string case: not move-
            // tracked, but its type must be recorded for ThrowStatement's check.
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

DanglingChecker::BranchState DanglingChecker::snapshotBranchState() const {
    BranchState s;
    s.movedFrom = movedFrom_;
    s.observerNames = observerNames_;
    s.observerOf = observerOf_;
    s.danglingObservers = danglingObservers_;
    s.aliasObservers = aliasObservers_;
    return s;
}

void DanglingChecker::restoreBranchState(const BranchState& state) {
    movedFrom_ = state.movedFrom;
    observerNames_ = state.observerNames;
    observerOf_ = state.observerOf;
    danglingObservers_ = state.danglingObservers;
    aliasObservers_ = state.aliasObservers;
}

void DanglingChecker::mergeBranchStates(const BranchState& before, const BranchState& thenState,
                                         const BranchState& elseState) {
    // movedFrom_: moved in EITHER branch -> moved after (conservative union).
    movedFrom_ = thenState.movedFrom;
    movedFrom_.insert(elseState.movedFrom.begin(), elseState.movedFrom.end());

    // Observer names: the union of everything either branch declared (a name
    // declared as an observer in one branch and used only within that branch is
    // already scope-checked independently; keeping the union here just means a
    // later use-site lookup can find it, matching how objectLocals_ union already
    // works for owners declared inside an unconditional tail after the `if`).
    observerNames_ = thenState.observerNames;
    observerNames_.insert(elseState.observerNames.begin(), elseState.observerNames.end());

    // danglingObservers_: dangling in EITHER branch -> dangling after (an owner
    // dying at a branch's own scope-exit is a real death on that path, and the
    // caller can't know which branch ran).
    danglingObservers_ = thenState.danglingObservers;
    danglingObservers_.insert(elseState.danglingObservers.begin(), elseState.danglingObservers.end());

    aliasObservers_ = thenState.aliasObservers;
    aliasObservers_.insert(elseState.aliasObservers.begin(), elseState.aliasObservers.end());

    // observerOf_: this is the field the sibling-branch-pollution bug was actually
    // about. For each observer name touched by EITHER branch, compare what each
    // branch says it observes (falling back to the pre-if binding if a branch never
    // touched it):
    //  - both branches agree (or only one branch touched it and the other kept the
    //    pre-if binding) -> keep that binding, it's certain either way.
    //  - the branches disagree (rebound to different owners, or one rebound while
    //    the other implicitly kept the old one) -> ambiguous; conservative-reject
    //    rule says mark it dangling rather than guess which binding survives.
    observerOf_ = before.observerOf;
    std::set<std::string> touched;
    for (const auto& [name, owner] : thenState.observerOf) touched.insert(name);
    for (const auto& [name, owner] : elseState.observerOf) touched.insert(name);
    for (const auto& name : touched) {
        auto beforeIt = before.observerOf.find(name);
        const std::string beforeOwner = beforeIt != before.observerOf.end() ? beforeIt->second : "";

        auto thenIt = thenState.observerOf.find(name);
        const std::string thenOwner = thenIt != thenState.observerOf.end() ? thenIt->second : beforeOwner;

        auto elseIt = elseState.observerOf.find(name);
        const std::string elseOwner = elseIt != elseState.observerOf.end() ? elseIt->second : beforeOwner;

        if (thenOwner == elseOwner) {
            if (thenOwner.empty()) {
                observerOf_.erase(name);
            } else {
                observerOf_[name] = thenOwner;
            }
        } else {
            // Ambiguous: the two branches leave `name` observing different owners
            // (or one rebinds it while the other leaves it unbound). The use-site
            // check only consults danglingObservers_ for names that HAVE an
            // observerOf_ entry (it needs an owner name to report in the
            // diagnostic), so an entry must still be present here -- pick whichever
            // branch's owner is non-empty (if any) purely for that message; the
            // dangling flag below is what actually makes any use after the `if` an
            // error, independent of which owner name is shown.
            observerOf_[name] = !thenOwner.empty() ? thenOwner : elseOwner;
            danglingObservers_.insert(name);
        }
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
            //
            // A value type has no owner to observe (it's copied, not smart-pointer-
            // backed) -- AstEmitter::emitStatement's isObserver branch unconditionally
            // emits the smart-pointer-style `T* b = (a).get();` lowering regardless of
            // decl->type, which does not even compile for a value type (`.get()`
            // doesn't exist on it). Reject outright rather than silently registering
            // observer state that has no corresponding safe codegen -- matches
            // CLAUDE.md's default-reject rule (unprovable safety is rejected, not
            // passed through).
            if (analyzer_ != nullptr && analyzer_->isValueTypeClass(decl->type)) {
                DanglingError e;
                e.line = decl->loc.line;
                e.column = decl->loc.column;
                e.message = "`ref` on value type `" + decl->type + "` (`" +
                            decl->identifier + "`) has no owner to observe";
                e.help = "`" + decl->type + "` is a value type (copied, not "
                         "smart-pointer-backed); CeeLing cannot form an observer of "
                         "it. Use a plain copy instead: `" + decl->type + " " +
                         decl->identifier + " = ...;`";
                errors_.push_back(e);
            }
            observerNames_.insert(decl->identifier);
            danglingObservers_.erase(decl->identifier);
            if (isObjectType(decl->type)) {
                localType_[decl->identifier] = decl->type;
                // `ref Node n = v[i];` observes the CONTAINER `v` (see
                // asContainerSubscript) -- an element has no identity of its own to
                // observe; invalidation is a whole-container reallocation event.
                if (std::string owner = resolveObservedOwner(decl->expression.get());
                    !owner.empty()) {
                    observerOf_[decl->identifier] = owner;
                }
            }
        } else {
            // `Node b = a;` — assigning one object local from another moves the source.
            // EXCEPT for a value type (struct/Iterator-shaped/known-external -- see
            // isObjectType's own comment): `Foo b = a;` there is an ordinary COPY, not
            // a move (confirmed by AstEmitter::emitStatement's own isValueType branch,
            // which emits a plain, non-`std::move`-wrapped copy for exactly this shape)
            // -- so `a` must stay live. Only a value type passed as a CALL ARGUMENT is
            // move-constructed (applyCallArgMoves below still applies to it), since
            // that is a genuinely different codegen shape (`sink(std::move(a))`).
            const bool srcIsValueTypeCopy = analyzer_ != nullptr &&
                analyzer_->isValueTypeClass(decl->type);
            if (isObjectType(decl->type) && decl->expression && !srcIsValueTypeCopy) {
                if (std::string src = asObjectLocalName(decl->expression.get()); !src.empty()) {
                    movedFrom_.insert(src);
                }
            }
            // `Node b = f(a);` — a call RHS moves its own object-typed arguments into
            // the callee, same as any other call (see applyCallArgMoves). Note this
            // is a no-op for a borrowing-method-call RHS like `a.block(...)`: its
            // "arguments" are the block's own int coordinates, not `a` itself -- `a`
            // (the receiver) is untouched by this, exactly as it must be (see the
            // borrowing-receiver branch below).
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
            // `Node b = a.block(...);` (a known-external class's allow-listed
            // borrowing method): `b` observes RECEIVER `a`, not a moved argument --
            // `a` stays fully live and usable. Checked as a separate fallback (not
            // folded into aliasedOwner above) because it must NOT be added to
            // aliasObservers_ below: aliasObservers_ exists to suppress the "owner
            // moved" check for an alias whose owner was ALREADY moved into the
            // aliasing call by construction (an ordinary call argument) -- that does
            // not apply here, since `a.block(...)` does not consume `a`. Only a
            // genuinely later reassignment/move of `a` should kill `b` (handled by
            // the pre-existing AssignmentStatement/applyCallArgMoves paths, which
            // walk observerOf_ regardless of aliasObservers_ membership).
            std::string borrowedOwner;
            if (aliasedOwner.empty() && isObjectType(decl->type) && decl->expression) {
                borrowedOwner = resolveBorrowingMethodReceiver(decl->expression.get());
            }
            if (!aliasedOwner.empty()) {
                observerNames_.insert(decl->identifier);
                danglingObservers_.erase(decl->identifier);
                observerOf_[decl->identifier] = aliasedOwner;
                aliasObservers_.insert(decl->identifier);
                localType_[decl->identifier] = decl->type;
            } else if (!borrowedOwner.empty()) {
                observerNames_.insert(decl->identifier);
                danglingObservers_.erase(decl->identifier);
                observerOf_[decl->identifier] = borrowedOwner;
                localType_[decl->identifier] = decl->type;
            } else if (isObjectType(decl->type)) {
                // Register the new object-typed owner; a fresh declaration revives
                // the name.
                objectLocals_.insert(decl->identifier);
                movedFrom_.erase(decl->identifier);
                localType_[decl->identifier] = decl->type;
            } else if (isStringType(decl->type)) {
                // `string` isn't an object type (no move/observer tracking), but its
                // declared type still needs to be recorded so ThrowStatement can
                // verify a thrown identifier is actually string-typed (see below) --
                // the only reader of a non-object localType_ entry.
                localType_[decl->identifier] = decl->type;
            }
        }
    } else if (auto assign = dynamic_cast<const AssignmentStatement*>(stmt)) {
        checkExpressionUses(assign->expression.get());

        const bool isBareFieldStore = !assign->target && !currentClass_.empty() &&
            objectLocals_.find(assign->variable) == objectLocals_.end() &&
            observerNames_.find(assign->variable) == observerNames_.end() &&
            classFields_.count(currentClass_) > 0 &&
            classFields_.at(currentClass_).count(assign->variable) > 0;
        if (assign->target || isBareFieldStore) {
            // Member target `a.b = ...`, OR a bare identifier naming a field of the
            // current class with implicit `this` (`link = other;` inside a method):
            // both store into / link a field. Neither reassigns or moves the root
            // object, and neither consumes the RHS — storing an object into a field is
            // a link/share, not an ownership handoff that abandons the source (a
            // linked-list node is used again right after linking). A local/observer of
            // the same name (checked above) shadows the field, matching ordinary scope
            // precedence. For a member target, also check the receiver isn't already
            // dead (the RHS was already checked above); a bare field-store's receiver
            // is the implicit, always-live `this`, so there's nothing extra to check.
            if (assign->target) {
                checkExpressionUses(assign->target.get());
            }
        } else if (observerNames_.find(assign->variable) != observerNames_.end()) {
            // Assigning to an observer rebinds what it observes: `b = inner;`. A
            // plain rebind is a real (non-alias) observation, so it no longer gets
            // the alias exemption even if `b` was originally alias-derived.
            danglingObservers_.erase(assign->variable);
            aliasObservers_.erase(assign->variable);
            if (std::string owner = resolveObservedOwner(assign->expression.get());
                !owner.empty()) {
                observerOf_[assign->variable] = owner;
            } else {
                observerOf_.erase(assign->variable);
            }
        } else if (isOperatorAssignMutation(*assign)) {
            // `n = value;` where `n`'s declared type is a class that declares its
            // own `operator=`, and `value` is NOT itself the same class (e.g.
            // `n = "text";`): this is a MUTATING method call on the still-alive
            // object (`n->operator=(value)`), not an ownership handoff -- `n`'s
            // identity does not change, so none of the owner-death machinery below
            // applies. The RHS was already use-checked above; if it's an object
            // local of a DIFFERENT class this can't happen (AstEmitter/EscapeHatch-
            // Checker's C++ type system would reject the resulting call at compile
            // time, same as any other type-mismatched call argument).
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
        checkReturnedReference(ret->expression.get());
    } else if (auto ifs = dynamic_cast<const IfStatement*>(stmt)) {
        checkExpressionUses(ifs->condition.get());
        // A branch runs conditionally, so any effect inside it (a move, an observer
        // rebind, a scope-exit death) is not certain on the path after the `if` --
        // each branch is analyzed independently from the same starting snapshot, then
        // the two outcomes are merged conservatively: an effect present in EITHER
        // branch holds after the `if` (a use after the `if` can't know which branch
        // ran), and if the branches actively disagree about what an observer points
        // at, that observer is treated as dangling rather than guessed at (same
        // "ambiguous -> conservative reject" rule CLAUDE.md specifies elsewhere). See
        // mergeBranchStates for the exact per-field rules.
        const BranchState before = snapshotBranchState();
        BranchState thenState = before;
        {
            const std::set<std::string> ownersBefore = objectLocals_;
            for (const auto& inner : ifs->body) {
                processStatement(inner.get());
            }
            endBlockScope(ownersBefore);
            thenState = snapshotBranchState();
        }
        restoreBranchState(before);
        BranchState elseState = before;
        if (!ifs->elseBody.empty()) {
            const std::set<std::string> ownersBefore = objectLocals_;
            for (const auto& inner : ifs->elseBody) {
                processStatement(inner.get());
            }
            endBlockScope(ownersBefore);
            elseState = snapshotBranchState();
        }
        // A branch that unconditionally exits (return/throw, or an if/else
        // where both arms do) can never reach the code AFTER this `if` --
        // that code only runs via the OTHER branch, so an exiting branch's
        // effects must not be merged into the post-`if` state (they'd falsely
        // "leak" a move/dangling-mark from a path that can't actually reach
        // there). Only one side can meaningfully exit for this to matter (if
        // BOTH did, every statement after the `if` is unreachable and this
        // choice is moot); each case just uses the OTHER branch's state
        // as-is, skipping the union mergeBranchStates would otherwise do.
        const bool thenExits = alwaysExits(ifs->body);
        const bool elseExits = !ifs->elseBody.empty() && alwaysExits(ifs->elseBody);
        if (thenExits && !elseExits) {
            restoreBranchState(elseState);
        } else if (elseExits && !thenExits) {
            restoreBranchState(thenState);
        } else {
            mergeBranchStates(before, thenState, elseState);
        }
    } else if (auto whiles = dynamic_cast<const WhileStatement*>(stmt)) {
        checkExpressionUses(whiles->condition.get());
        // Same conservatism as `if`: the loop body may run zero or more times, so any
        // effect inside it is not certain on the path after the loop. Modeled as a
        // two-way branch against "loop body never runs" (mirrors the `if`/no-`else`
        // case above), reusing the same merge rules -- including the observer-rebind
        // disagreement handling the sibling-branch fix added for `if`.
        const BranchState before = snapshotBranchState();
        const std::set<std::string> ownersBefore = objectLocals_;
        for (const auto& inner : whiles->body) {
            processStatement(inner.get());
        }
        endBlockScope(ownersBefore);
        const BranchState afterLoop = snapshotBranchState();
        restoreBranchState(before);
        mergeBranchStates(before, afterLoop, before);
    } else if (auto cdecl = dynamic_cast<const ContainerDeclaration*>(stmt)) {
        // `std::vector<T> v;` -- no initializer (the one definite-init exception,
        // see Chapter 17/grammar.js container_declaration). Always a fresh owner;
        // no RHS uses to check and nothing to move.
        objectLocals_.insert(cdecl->identifier);
        movedFrom_.erase(cdecl->identifier);
        localType_[cdecl->identifier] = cdecl->type;
    } else if (auto fors = dynamic_cast<const ForStatement*>(stmt)) {
        checkExpressionUses(fors->container.get());
        // Same conservatism as `while`: the body may run zero or more times.
        const BranchState before = snapshotBranchState();
        const std::set<std::string> ownersBefore = objectLocals_;
        // The loop variable is a fresh owner for each (modeled: one static pass
        // over the) iteration -- registered exactly like a VariableDeclaration,
        // then cleaned up by endBlockScope same as any other block-local.
        if (isObjectType(fors->loopVarType)) {
            objectLocals_.insert(fors->loopVarName);
            localType_[fors->loopVarName] = fors->loopVarType;
        }
        for (const auto& inner : fors->body) {
            processStatement(inner.get());
        }
        endBlockScope(ownersBefore);
        const BranchState afterLoop = snapshotBranchState();
        restoreBranchState(before);
        mergeBranchStates(before, afterLoop, before);
    } else if (auto exprStmt = dynamic_cast<const ExpressionStatement*>(stmt)) {
        // A bare expression statement, e.g. `sink(a);`. First check uses, then
        // apply move effects: a user function call moves its object-typed args.
        checkExpressionUses(exprStmt->expression.get());
        applyCallArgMoves(exprStmt->expression.get());
    } else if (auto throwStmt = dynamic_cast<const ThrowStatement*>(stmt)) {
        checkExpressionUses(throwStmt->expression.get());
        // `throw` is restricted to string-typed operands this slice (CeeLing has no
        // exception-class hierarchy without inheritance -- see CLAUDE.md/TODO.md).
        // A string literal, a string-typed identifier, or a `+` concatenation of
        // such (recursively) is fine; anything else (a class object, a container, a
        // numeric expression, an unresolvable expression) is rejected, per the
        // conservative-reject rule.
        bool isStringOperand = isStringValuedExpr(throwStmt->expression.get());
        if (!isStringOperand) {
            DanglingError e;
            e.line = throwStmt->expression->loc.line;
            e.column = throwStmt->expression->loc.column;
            e.message = "`throw` operand is not a string";
            e.help = "CeeLing's `throw` is restricted to `string` messages -- there is "
                     "no exception-class hierarchy yet (that needs inheritance/"
                     "polymorphism, not implemented), so a thrown value can only be a "
                     "plain error message. Throw a string literal or a `string`-typed "
                     "variable instead.";
            errors_.push_back(e);
        }
        // No scope-exit logic needed: block-exit is already unconditional (endBlockScope
        // runs after every if/while/for/try body regardless of a return/throw inside),
        // so a throw needs no special early-exit machinery -- see Chapter 18.
    } else if (auto tryStmt = dynamic_cast<const TryStatement*>(stmt)) {
        // The try body may throw at ANY point inside it, not just fail-or-succeed as
        // a whole -- modeled the same way `while`'s "body runs zero or more times" is
        // modeled: merge "try body never ran" against "try body ran to completion" to
        // get the conservative "could have thrown after any prefix" state the catch
        // block must be analyzed from (we don't know how far execution got before the
        // throw). The state after the whole construct is a second, ordinary two-way
        // merge: "try succeeded, no exception" vs "catch ran".
        const BranchState before = snapshotBranchState();
        const std::set<std::string> ownersBeforeTry = objectLocals_;
        for (const auto& inner : tryStmt->body) {
            processStatement(inner.get());
        }
        endBlockScope(ownersBeforeTry);
        const BranchState tryState = snapshotBranchState();

        restoreBranchState(before);
        mergeBranchStates(before, before, tryState);
        const BranchState throwEntryState = snapshotBranchState();

        restoreBranchState(throwEntryState);
        const std::set<std::string> ownersBeforeCatch = objectLocals_;
        // The caught variable is a string (enforced below); not move-tracked, but its
        // type is recorded the same way a string local/param is, so a re-throw of it
        // inside the catch body is recognized as a valid string operand.
        if (!isStringType(tryStmt->catchType)) {
            DanglingError e;
            e.line = stmt->loc.line;
            e.column = stmt->loc.column;
            e.message = "`catch` type `" + tryStmt->catchType + "` is not supported";
            e.help = "CeeLing's `catch` is restricted to `string` this slice, matching "
                     "`throw`'s restriction to string messages -- there is no exception-"
                     "class hierarchy yet. Declare the catch parameter as `string`.";
            errors_.push_back(e);
        } else {
            localType_[tryStmt->catchIdentifier] = tryStmt->catchType;
        }
        for (const auto& inner : tryStmt->catchBody) {
            processStatement(inner.get());
        }
        endBlockScope(ownersBeforeCatch);
        const BranchState catchState = snapshotBranchState();

        restoreBranchState(before);
        mergeBranchStates(before, tryState, catchState);
    }
}

} // namespace cling
