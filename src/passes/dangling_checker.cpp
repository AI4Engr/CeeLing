#include "passes/dangling_checker.h"

#include <functional>
#include "syntax/ast_walk.h"
#include "syntax/class_hierarchy.h"
#include "facts/library_summary.h"
#include "passes/closure_desugar.h"

namespace cling {

namespace {
// True if `body` always leaves its block before falling through: it ends in a
// return/throw/break/continue, or in an if/else whose arms both always exit. The `if` merge uses
// it so a move in an exiting branch is not seen by the code after the `if`. Only the last
// statement is looked at; a missed exit just falls back to the stricter merge.
bool alwaysExits(const std::vector<StatementPtr>& body) {
    if (body.empty()) return false;
    const Statement* last = body.back().get();
    if (dynamic_cast<const ReturnStatement*>(last) != nullptr) return true;
    if (dynamic_cast<const ThrowStatement*>(last) != nullptr) return true;
    if (dynamic_cast<const BreakStatement*>(last) != nullptr) return true;
    if (dynamic_cast<const ContinueStatement*>(last) != nullptr) return true;
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
    classFieldTypes_.clear();
    collectClassNames(program);
    summaries_.clear();
    functionParams_.clear();
    classMethodParams_.clear();
    deriveSummaries(program);
    analyzer_ = &analyzer;
    program_ = &program;

    // Each top-level function / main / method body is analyzed independently.
    for (const auto& stmt : program.statements) {
        if (auto fn = dynamic_cast<const FunctionDeclaration*>(stmt.get())) {
            checkBody(fn->body, fn->parameters, "", fn->name);
        } else if (auto mn = dynamic_cast<const MainFunction*>(stmt.get())) {
            checkBody(mn->body, {}, "", "main");
        } else if (auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
            for (const auto& member : cls->members) {
                if (auto method = dynamic_cast<const MethodDeclaration*>(member.get())) {
                    checkBody(method->body, method->parameters, cls->name, cls->name + "::" + method->name);
                } else if (auto ctor = dynamic_cast<const ConstructorDeclaration*>(member.get())) {
                    checkBody(ctor->body, ctor->parameters, cls->name, cls->name + "::" + ctor->name);
                }
            }
        }
    }
    return errors_;
}

void DanglingChecker::collectClassNames(const Program& program) {
    // Base before derived, so a derived class's classFields_ entry already holds its base's
    // fields and `inheritedField = param;` is recognized as a field store.
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
            if (auto field = dynamic_cast<const VariableDeclaration*>(member.get())) {
                classFieldTypes_[cls->name][field->identifier] = field->type;
            } else if (auto cfield = dynamic_cast<const ContainerDeclaration*>(member.get())) {
                classFieldTypes_[cls->name][cfield->identifier] = cfield->type;
            }
        }

        // Also register under the qualified name, or a `YAML::Node` local would never be
        // move-tracked.
        if (!cls->namespaceName.empty()) {
            const std::string qualified = cls->namespaceName + "::" + cls->name;
            classNames_.insert(qualified);
            classFields_[qualified] = fields;
        }
    }
}

bool DanglingChecker::isObjectType(const std::string& type) const {
    // Classes, vectors, maps and value-type classes (structs, external value types such as
    // Eigen::MatrixXd) are tracked: they are moved into callees. `string` is not: it is a plain
    // value that is copied. A declaration-copy of a value type is not a move; that is decided
    // where declarations are processed.
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

    // Collect every `return`, including those nested in if/while/for/try.
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
        } else if (auto fors = dynamic_cast<const ForStatement*>(s)) {
            for (const auto& inner : fors->body) stack.push_back(inner.get());
        } else if (auto trys = dynamic_cast<const TryStatement*>(s)) {
            for (const auto& inner : trys->body) stack.push_back(inner.get());
            for (const auto& inner : trys->catchBody) stack.push_back(inner.get());
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
                    // Returns aliasing different parameters cannot be one index; keep the
                    // first rather than downgrade to "fresh".
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
            // Also register under the qualified name (`YAML::Load`), which is how a call
            // outside the namespace spells it.
            if (!fn->namespaceName.empty()) {
                const std::string qualified = fn->namespaceName + "::" + fn->name;
                functionParams_[qualified] = fn->parameters;
                functionReturnType_[qualified] = fn->returnType;
                summaries_[summaryKey("", qualified)] = summaries_[summaryKey("", fn->name)];
            }
        } else if (auto extFn = dynamic_cast<const ExternFunctionDeclaration*>(stmt.get())) {
            // No body: the summary is the declared `borrows N` fact (or -1).
            functionParams_[extFn->name] = extFn->parameters;
            functionReturnType_[extFn->name] = extFn->returnType;
            FunctionSummary summary;
            summary.returnBorrowsParam = extFn->returnBorrowsParam;
            summaries_[summaryKey("", extFn->name)] = summary;
            // `ref` params are borrowed for the call, not moved.
            externRefParams_[extFn->name] = extFn->refParams;
        }
    }
    // Base before derived: an inherited method the derived class does not override keeps
    // the base's parameters and summary.
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
        // Only an overloaded method name needs the arity-qualified key.
        std::map<std::string, int> nameCounts;
        for (const auto& member : cls->members) {
            if (auto method = dynamic_cast<const MethodDeclaration*>(member.get())) {
                if (method->specializationArg.empty()) nameCounts[method->name]++;
            }
        }
        for (const auto& member : cls->members) {
            if (auto method = dynamic_cast<const MethodDeclaration*>(member.get())) {
                // A specialization registers under its synthetic key ("as<int>"); an
                // overloaded method under overloadKey(name, paramCount), so one
                // declaration does not overwrite another.
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
                // A primary generic whose T is a parameter type is an out-parameter, unless
                // T is also the return type (`<T> T as(T fallback)`): then it is an
                // ordinary by-value parameter.
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

        // Also register under the qualified name: lookups are keyed by the receiver's
        // declared type string, "YAML::Node" for a `YAML::Node n;` local.
        if (!cls->namespaceName.empty()) {
            const std::string qualified = cls->namespaceName + "::" + cls->name;
            classMethodParams_[qualified] = params;
            classMethodReturnType_[qualified] = returnTypes;
            for (const auto& [methodName, _] : params) {
                if (auto it = summaries_.find(summaryKey(cls->name, methodName)); it != summaries_.end()) {
                    summaries_[summaryKey(qualified, methodName)] = it->second;
                }
            }
            // The same for outParamIndex_.
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
    // Only a bare-identifier target reaches here; `assign.variable` is the whole LHS.
    auto typeIt = localType_.find(assign.variable);
    if (typeIt == localType_.end()) return false;
    const std::string& lhsClass = typeIt->second;
    if (classNames_.find(lhsClass) == classNames_.end()) return false;  // not a class (container/primitive)
    auto methodIt = classMethodParams_.find(lhsClass);
    if (methodIt == classMethodParams_.end() || methodIt->second.count("operator=") == 0) {
        return false;  // this class has no operator= -- ordinary reassignment path
    }
    // A same-class RHS is an ordinary ownership transfer, not operator=.
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
    // The container must be a bare, live local; `f()[i]` and `a.v[i]` are not resolved.
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

bool DanglingChecker::resolveRefBinding(const Expression* expr, std::string& owner,
                                        bool& inheritsDangling) const {
    owner.clear();
    inheritsDangling = false;
    if (expr == nullptr) return true;
    if (auto id = dynamic_cast<const Identifier*>(expr)) {
        if (id->name == "nullptr") return true;
        if (observerNames_.count(id->name)) {
            auto it = observerOf_.find(id->name);
            if (it != observerOf_.end()) owner = it->second;
            inheritsDangling = danglingObservers_.count(id->name) > 0;
            return true;
        }
    }
    owner = resolveObservedOwner(expr);
    return !owner.empty();
}

void DanglingChecker::reportUntrackableRefBinding(const std::string& name,
                                                  const Expression* expr,
                                                  const ASTNode* anchor) {
    DanglingError e;
    e.line = anchor->loc.line;
    e.column = anchor->loc.column;
    e.message = "`ref " + name + "` is bound to `" + expr->toString() +
                "`, whose owner cannot be tracked";
    e.help = "a `ref` may only observe an object local, an element `v[i]` of a "
             "container local, another `ref`, or nullptr. A field (`p.child`) or a "
             "call result (`p.getChild()`, `make()`) has no local owner whose death "
             "the checker can see, so the observer could silently dangle. Use a "
             "plain (owning/sharing) local instead: `" + name + " = ...` declared "
             "without `ref`.";
    errors_.push_back(e);
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
    // Field chains, `new` and literals are not resolved.
    return {};
}

std::string DanglingChecker::resolveCallReturnAlias(const Expression* expr) const {
    auto call = dynamic_cast<const FunctionCall*>(expr);
    if (call == nullptr) return {};

    const FunctionSummary* summary = nullptr;
    const std::vector<std::pair<std::string, std::string>>* params = nullptr;

    if (call->callee) {
        // `recv.method(args)`: find the receiver's class (a name or a chained call).
        auto member = dynamic_cast<const MemberAccess*>(call->callee.get());
        if (member == nullptr) return {};
        std::string recvClass = resolveClassOfExpr(member->object.get());
        if (recvClass.empty()) return {};
        auto clsIt = classMethodParams_.find(recvClass);
        if (clsIt == classMethodParams_.end()) return {};
        // A call with a type argument resolves to the specialization's key.
        std::string methodKey = call->templateArg.empty()
            ? member->member
            : specializationKey(member->member, call->templateArg);
        auto paramIt = clsIt->second.find(methodKey);
        if (paramIt == clsIt->second.end() && call->templateArg.empty()) {
            // An overloaded name is registered under an arity key.
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

bool DanglingChecker::isSharedLocal(const std::string& name) const {
    auto typeIt = localType_.find(name);
    if (typeIt == localType_.end() || analyzer_ == nullptr) return false;
    if (classNames_.count(typeIt->second) == 0) return false;
    return analyzer_->getPointerTypeInScope(currentScope_, typeIt->second, name) == PointerType::SHARED;
}

namespace {
// Every declaration of / assignment to `name` in `body` (nested blocks included) must be `new <type>(...)`:
// that proves the variable's dynamic type is exactly `type`. Returns false at the first one that is not.
bool onlyAssignedFreshNew(const std::vector<StatementPtr>& body, const std::string& name,
                          const std::string& type) {
    auto isFresh = [&](const Expression* e) {
        auto neww = dynamic_cast<const NewExpression*>(e);
        return neww != nullptr && neww->className == type;
    };
    for (const auto& stmt : body) {
        if (auto d = dynamic_cast<const VariableDeclaration*>(stmt.get())) {
            if (d->identifier == name && !isFresh(d->expression.get())) return false;
        } else if (auto a = dynamic_cast<const AssignmentStatement*>(stmt.get())) {
            if (a->variable == name && a->target == nullptr && !isFresh(a->expression.get())) return false;
        } else if (auto i = dynamic_cast<const IfStatement*>(stmt.get())) {
            if (!onlyAssignedFreshNew(i->body, name, type) || !onlyAssignedFreshNew(i->elseBody, name, type)) return false;
        } else if (auto w = dynamic_cast<const WhileStatement*>(stmt.get())) {
            if (!onlyAssignedFreshNew(w->body, name, type)) return false;
        } else if (auto f = dynamic_cast<const ForStatement*>(stmt.get())) {
            if (f->loopVarName == name) return false;
            if (!onlyAssignedFreshNew(f->body, name, type)) return false;
        } else if (auto t = dynamic_cast<const TryStatement*>(stmt.get())) {
            if (t->catchIdentifier == name) return false;
            if (!onlyAssignedFreshNew(t->body, name, type) || !onlyAssignedFreshNew(t->catchBody, name, type)) return false;
        }
    }
    return true;
}
} // namespace

std::string DanglingChecker::spawnObjectProblem(const std::string& type, const std::string& name) const {
    const ClassDeclaration* decl = nullptr;
    bool inherited = false;
    for (const auto& stmt : program_->statements) {
        auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get());
        if (cls == nullptr) continue;
        if (cls->name == type) decl = cls;
        if (cls->baseClass == type) inherited = true;
    }
    if (decl == nullptr || decl->isExternalDeclaration) {
        return "class `" + type + "` is not defined in this file (a class from an included header may have "
               "subclasses or fields this file cannot see)";
    }
    if (decl->isValueType) return "";   // a struct is copied, not moved: handled by the value rules
    if (!decl->baseClass.empty()) return "class `" + type + "` has a base class";
    if (inherited) return "class `" + type + "` is inherited by another class in this program";
    if (analyzer_->classNeedsSharedFromThis(type)) {
        return "class `" + type + "` hands out `this`, so its objects are shared";
    }
    for (const auto& member : decl->members) {
        if (auto m = dynamic_cast<const MethodDeclaration*>(member.get())) {
            if (m->isVirtual) return "class `" + type + "` has a virtual method (a subclass could be hiding behind it)";
        } else if (auto f = dynamic_cast<const VariableDeclaration*>(member.get())) {
            if (f->isObserver || !isPlainValueType(f->type, *program_)) {
                return "field `" + f->identifier + "` of class `" + type + "` is not a plain value (type `" +
                       f->type + "`); only primitives, string, vectors of those and plain structs are allowed";
            }
        } else if (auto c = dynamic_cast<const ContainerDeclaration*>(member.get())) {
            if (!isPlainValueType(c->type, *program_)) {
                return "field `" + c->identifier + "` of class `" + type + "` is not a plain value (type `" +
                       c->type + "`)";
            }
        }
    }
    if (currentParamNames_.count(name) > 0) return "`" + name + "` is a parameter, so its dynamic type is unknown here";
    if (currentBody_ == nullptr || !onlyAssignedFreshNew(*currentBody_, name, type)) {
        return "`" + name + "` is not created only by `new " + type + "(...)` in this function, so it might hold a "
               "subclass object or an object also reachable from elsewhere";
    }
    if (analyzer_->getPointerTypeInScope(currentScope_, type, name) != PointerType::UNIQUE) {
        return "`" + name + "` has more than one owner (it is stored elsewhere or shared), so moving it would leave a "
               "live alias in this thread";
    }
    return "";
}

void DanglingChecker::checkSpawnCall(const FunctionCall* call) {
    if (call->arguments.empty() || call->arguments.size() > 2) return;
    // arguments[1], when present, is the failure handler: plain values only, nothing to move or check here
    auto neww = dynamic_cast<const NewExpression*>(call->arguments[0].get());
    if (neww == nullptr) return;
    const ClassDeclaration* job = nullptr;
    for (const auto& stmt : program_->statements) {
        auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get());
        if (cls != nullptr && cls->name == neww->className && cls->isSpawnJob) job = cls;
    }
    if (job == nullptr) return;
    for (const auto& arg : neww->arguments) {
        auto id = dynamic_cast<const Identifier*>(arg.get());
        if (id == nullptr) continue;
        auto typeIt = localType_.find(id->name);
        if (typeIt == localType_.end() || classNames_.count(typeIt->second) == 0) continue;
        if (isPlainValueType(typeIt->second, *program_)) continue;   // a struct / vector: copied, source stays usable
        const std::string problem = spawnObjectProblem(typeIt->second, id->name);
        if (!problem.empty()) {
            DanglingError e;
            e.line = call->loc.line;
            e.column = call->loc.column;
            e.code = "spawn-captures-object";
            e.message = "`spawn` cannot move `" + id->name + "` into a new thread: " + problem;
            e.help = "an object may be moved into a thread only if nothing else can reach it and nothing about "
                     "it is hidden: make the data a `struct` (copied), or serialize it to a string and rebuild it "
                     "inside the thread";
            errors_.push_back(e);
            continue;
        }
        // The job holds the object in a unique field; the analysis must agree, or the emitted code would copy.
        for (const auto& member : job->members) {
            auto f = dynamic_cast<const VariableDeclaration*>(member.get());
            if (f != nullptr && f->identifier == id->name &&
                analyzer_->getPointerTypeInScope(job->name, f->type, f->identifier) != PointerType::UNIQUE) {
                DanglingError e;
                e.line = call->loc.line;
                e.column = call->loc.column;
                e.code = "spawn-captures-object";
                e.message = "internal: the spawn job's field `" + f->identifier + "` was not inferred unique";
                e.help = "rejected rather than risk a shared handle crossing into the new thread";
                errors_.push_back(e);
            }
        }
        movedFrom_.insert(id->name);
    }
}

std::string DanglingChecker::declaredTypeOf(const Expression* expr) const {
    if (auto id = dynamic_cast<const Identifier*>(expr)) {
        if (auto it = localType_.find(id->name); it != localType_.end()) return it->second;
        if (!currentClass_.empty()) {
            if (auto cls = classFieldTypes_.find(currentClass_); cls != classFieldTypes_.end()) {
                if (auto f = cls->second.find(id->name); f != cls->second.end()) return f->second;
            }
        }
        return {};
    }
    if (auto member = dynamic_cast<const MemberAccess*>(expr)) {
        const std::string objectType = declaredTypeOf(member->object.get());
        if (auto cls = classFieldTypes_.find(objectType); cls != classFieldTypes_.end()) {
            if (auto f = cls->second.find(member->member); f != cls->second.end()) return f->second;
        }
    }
    return {};
}

void DanglingChecker::applyCallArgMoves(const Expression* expr) {
    if (expr == nullptr) return;
    if (auto call = dynamic_cast<const FunctionCall*>(expr)) {
        if (call->callee == nullptr && call->function_name == "cling::spawn") {
            applyOneCallMoves(expr);          // its arguments are a closure literal, checked as a whole
            return;
        }
        if (call->callee) applyCallArgMoves(call->callee.get());
        for (const auto& arg : call->arguments) applyCallArgMoves(arg.get());
        applyOneCallMoves(expr);
    } else if (auto bin = dynamic_cast<const BinaryExpression*>(expr)) {
        applyCallArgMoves(bin->left.get());
        applyCallArgMoves(bin->right.get());
    } else if (auto un = dynamic_cast<const UnaryExpression*>(expr)) {
        applyCallArgMoves(un->operand.get());
    } else if (auto member = dynamic_cast<const MemberAccess*>(expr)) {
        applyCallArgMoves(member->object.get());
    } else if (auto sub = dynamic_cast<const SubscriptExpression*>(expr)) {
        applyCallArgMoves(sub->object.get());
        applyCallArgMoves(sub->index.get());
    } else if (auto neww = dynamic_cast<const NewExpression*>(expr)) {
        for (const auto& arg : neww->arguments) applyCallArgMoves(arg.get());
        killFieldObservers("");      // a constructor may run any code
    }
}

void DanglingChecker::applyOneCallMoves(const Expression* expr) {
    auto call = dynamic_cast<const FunctionCall*>(expr);
    if (call == nullptr) return;

    if (call->callee == nullptr && call->function_name == "cling::spawn") {
        checkSpawnCall(call);
        return;
    }

    invalidateContainerObservers(expr);
    if (hasFieldObservers() && calleeHeapEffect(call) == 2) {
        for (const auto& arg : call->arguments) {
            auto aid = dynamic_cast<const Identifier*>(arg.get());
            auto o = aid != nullptr ? observerOf_.find(aid->name) : observerOf_.end();
            if (o != observerOf_.end() && !o->second.empty() && o->second[0] == '@') {
                DanglingError e;
                e.line = arg->loc.line;
                e.column = arg->loc.column;
                e.message = "`" + aid->name + "`, a view of the field `" + o->second.substr(1) + "`, is passed to a call that may change that field";
                e.help = "The callee could clear or grow the field and then read through the span. Copy the field into a local vector and view that, or "
                         "call something that only reads.";
                errors_.push_back(e);
            }
        }
        killFieldObservers("");
    }

    // A library method may share an argument instead of moving it (`v.push_back(x)`).
    // Resolve the receiver's type and name once for the per-argument rule.
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
    // A Channel<T> copies what it is given and hands out fresh copies: nothing is moved or aliased.
    if (call->callee) {
        if (auto member = dynamic_cast<const MemberAccess*>(call->callee.get())) {
            if (isChannelType(declaredTypeOf(member->object.get()))) return;
        }
    }
    const bool receiverIsVector = !receiverType.empty() && isVectorType(receiverType);
    // A mutable out-parameter (outParamIndex_) is never moved.
    size_t outParamIdx = call->arguments.size();  // sentinel: no out-parameter
    if (!receiverType.empty()) {
        if (auto it = outParamIndex_.find(outParamKey(receiverType, methodName));
            it != outParamIndex_.end()) {
            outParamIdx = it->second;
        }
    } else if (call->callee == nullptr && !currentClass_.empty()) {
        // an unqualified call of a method of the current class (implicit `this`)
        if (auto it = outParamIndex_.find(outParamKey(currentClass_, call->function_name)); it != outParamIndex_.end()) {
            outParamIdx = it->second;
        }
    }
    // ext_fn `ref` arguments are borrowed, not moved. ext_fn is always a free function.
    const std::set<size_t>* refParams = nullptr;
    if (receiverType.empty()) {
        if (auto it = externRefParams_.find(call->function_name); it != externRefParams_.end()) {
            refParams = &it->second;
        }
    }

    // An argument an earlier call in this statement already moved (`add(v, total(v))`) is a use
    // of a moved-from value even when this call only borrows it.
    auto reportMovedArgument = [&](size_t i) {
        const std::string src = asObjectLocalName(call->arguments[i].get());
        if (src.empty() || movedFrom_.count(src) == 0) return;
        DanglingError e;
        e.line = call->arguments[i]->loc.line;
        e.column = call->arguments[i]->loc.column;
        e.message = "use of moved-from value `" + src + "`";
        e.help = "`" + src + "` was moved by an earlier call in this same statement; split the statement or pass a copy";
        for (const auto& old : errors_) {
            if (old.line == e.line && old.column == e.column && old.message == e.message) return;
        }
        errors_.push_back(e);
    };

    for (size_t i = 0; i < call->arguments.size(); ++i) {
        if (i == outParamIdx || (refParams != nullptr && refParams->count(i))) {
            reportMovedArgument(i);
            continue;
        }
        // An inferred read-only container parameter is borrowed, not moved. It is a property
        // of the callee's parameter, so it is resolved by callee and argument index.
        if (analyzer_ != nullptr && i < call->arguments.size()) {
            const std::vector<std::pair<std::string, std::string>>* calleeParams = nullptr;
            std::string implicitClass;     // an unqualified call of a method of the current class (implicit `this`)
            if (receiverType.empty()) {
                if (auto it = functionParams_.find(call->function_name); it != functionParams_.end()) {
                    calleeParams = &it->second;
                } else if (call->callee == nullptr && !currentClass_.empty()) {
                    if (auto clsIt = classMethodParams_.find(currentClass_); clsIt != classMethodParams_.end()) {
                        if (auto mIt = clsIt->second.find(call->function_name); mIt != clsIt->second.end()) {
                            calleeParams = &mIt->second;
                            implicitClass = currentClass_;
                        }
                    }
                }
            } else if (auto clsIt = classMethodParams_.find(receiverType); clsIt != classMethodParams_.end()) {
                if (auto mIt = clsIt->second.find(methodName); mIt != clsIt->second.end()) {
                    calleeParams = &mIt->second;
                }
            }
            if (calleeParams != nullptr && i < calleeParams->size()) {
                if (isSpanType((*calleeParams)[i].first)) {
                    // Viewed for the call, not given away. A local or parameter vector cannot
                    // be reached by the callee; a field or container element can.
                    const Expression* arg = call->arguments[i].get();
                    const auto* argId = dynamic_cast<const Identifier*>(arg);
                    const bool nonLocal = (argId != nullptr && localType_.count(argId->name) == 0 && isVectorType(declaredTypeOf(arg))) ||
                                          dynamic_cast<const MemberAccess*>(arg) != nullptr || dynamic_cast<const SubscriptExpression*>(arg) != nullptr;
                    if (nonLocal && calleeHeapEffect(call) != 1) {
                        DanglingError e;
                        e.line = arg->loc.line;
                        e.column = arg->loc.column;
                        e.message = "a `span` parameter cannot view `" + arg->toString() + "`, which is not a local vector, for a callee that may change it";
                        e.help = "The callee could change that field (or container) while it holds the span, and the span would then point at freed memory. "
                                 "It is accepted when the callee provably changes no container or field it does not own (a heap-pure function); otherwise "
                                 "copy the field into a local vector first and pass the local.";
                        errors_.push_back(e);
                    }
                    reportMovedArgument(i);
                    continue;
                }
                const std::string& pname = (*calleeParams)[i].second;
                const bool isReadOnlyContainer = !implicitClass.empty()
                    ? analyzer_->isReadOnlyMethodContainerParam(implicitClass, call->function_name, pname)
                    : (receiverType.empty()
                        ? analyzer_->isReadOnlyContainerParam(call->function_name, pname)
                        : analyzer_->isReadOnlyMethodContainerParam(receiverType, methodName, pname));
                if (isReadOnlyContainer) { reportMovedArgument(i); continue; }
            }
        }
        if (receiverIsVector && vectorMethodSharesArg(methodName, i)) {
            // The element pointer kind is fixed per container. Shared element and shared
            // argument: copy, the argument survives. Shared element and unique argument, or
            // unique element: the argument is moved.
            const std::string argName = asObjectLocalName(call->arguments[i].get());
            const bool elementIsShared = analyzer_ != nullptr &&
                analyzer_->containerElementIsShared(receiverName);
            const bool argIsShared = analyzer_ != nullptr && !argName.empty() &&
                analyzer_->getPointerTypeInScope(currentScope_, localType_.count(argName) ? localType_.at(argName) : "",
                                          argName) == PointerType::SHARED;
            if (elementIsShared && argIsShared) {
                continue;  // shares -- the argument stays a live owner
            }
        }

        // An argument whose own pointer type is shared is never moved into any call: a
        // shared_ptr is copied and the source stays a live owner. Only class-typed arguments
        // qualify; the analyzer has no entry for a container type and defaults to shared, so
        // containers are decided by the vector branch above.
        const std::string argName = asObjectLocalName(call->arguments[i].get());
        const std::string argType = localType_.count(argName) ? localType_.at(argName) : "";
        if (analyzer_ != nullptr && !argName.empty() && classNames_.count(argType) > 0 &&
            analyzer_->getPointerTypeInScope(currentScope_, argType, argName) == PointerType::SHARED) {
            continue;
        }

        if (std::string src = asObjectLocalName(call->arguments[i].get()); !src.empty()) {
            if (movedFrom_.count(src) > 0) {
                // checkExpressionUses ran over the whole statement before any move, so this owner
                // was moved by an earlier call of the same statement (`f(v) + f(v)`).
                DanglingError e;
                e.line = call->arguments[i]->loc.line;
                e.column = call->arguments[i]->loc.column;
                e.message = "use of moved-from value `" + src + "`";
                e.help = "`" + src + "` was moved by an earlier call in this same statement; split the statement or pass a copy";
                errors_.push_back(e);
            }
            movedFrom_.insert(src);
        }
    }
}

void DanglingChecker::killFieldObservers(const std::string& field) {
    for (const auto& [obs, owner] : observerOf_) {
        if (!owner.empty() && owner[0] == '@' && (field.empty() || owner == "@" + field)) danglingObservers_.insert(obs);
    }
}

bool DanglingChecker::hasFieldObservers() const {
    for (const auto& [obs, owner] : observerOf_) {
        if (!owner.empty() && owner[0] == '@' && danglingObservers_.count(obs) == 0) return true;
    }
    return false;
}

int DanglingChecker::calleeHeapEffect(const FunctionCall* call) const {
    std::string name = call->function_name;
    if (call->callee != nullptr) {
        auto member = dynamic_cast<const MemberAccess*>(call->callee.get());
        if (member == nullptr) return 2;
        name = member->member;
    }
    if (name.empty()) return 2;
    bool user = false, pure = true;
    if (call->callee == nullptr && functionParams_.count(name) > 0) {
        user = true;
        pure = pure && analyzer_ != nullptr && analyzer_->isHeapPureFreeFunction(name);
    }
    for (const auto& [cls, methods] : classMethodParams_) {
        if (methods.count(name) == 0) continue;
        user = true;
        pure = pure && analyzer_ != nullptr && analyzer_->isHeapPureMethod(cls, name);
    }
    if (!user) return 0;
    return pure ? 1 : 2;
}

namespace {
// A container named by a bare identifier (`v`, a local or a field of the current class) or by `obj.field`.
std::string containerKey(const Expression* expr) {
    if (auto id = dynamic_cast<const Identifier*>(expr)) return id->name;
    if (auto member = dynamic_cast<const MemberAccess*>(expr)) {
        if (auto obj = dynamic_cast<const Identifier*>(member->object.get())) return obj->name + "." + member->member;
    }
    return {};
}
}  // namespace

void DanglingChecker::checkIteratedContainerNotMutated(const ForStatement* loop) {
    const std::string key = containerKey(loop->container.get());
    if (key.empty()) return;
    const std::string type = declaredTypeOf(loop->container.get());
    if (!isVectorType(type) && !isStringType(type) && !isMapType(type)) return;

    auto mayInvalidate = [&](const std::string& method) {
        if (isVectorType(type)) return vectorMethodInvalidatesReferences(method);
        if (isStringType(type)) return stringMethodInvalidatesReferences(method);
        return mapMethodInvalidatesReferences(method);
    };
    auto report = [&](const ASTNode& at, const std::string& what) {
        DanglingError e;
        e.line = at.loc.line;
        e.column = at.loc.column;
        e.code = "container-changed-while-iterated";
        e.message = what + " while the loop is walking it";
        e.help = "`for (" + loop->loopVarType + " " + loop->loopVarName + " : " + key + ")` holds iterators into `" + key +
                 "`, and a call that may reallocate or reshape it (`push_back`, `erase`, `clear`, `resize`, ...) leaves them "
                 "pointing at freed or moved memory. Collect the changes in another container and apply them after the loop, "
                 "or walk by index with a `while` loop.";
        errors_.push_back(e);
    };

    // A FIELD of the current class named bare (`items`): a bare call of one of this class's own methods that is not
    // heap-pure (it changes a container or field it does not own) may reach `items` through `this`.
    const bool bareField = loop->container != nullptr && dynamic_cast<const Identifier*>(loop->container.get()) != nullptr &&
                           localType_.find(key) == localType_.end();
    astwalk::walkStatements(const_cast<std::vector<StatementPtr>&>(loop->body), [&](ASTNode& node) {
        if (auto call = dynamic_cast<FunctionCall*>(&node)) {
            auto member = dynamic_cast<const MemberAccess*>(call->callee.get());
            // `r.push_back(x)` through a `ref r = v;` observer reaches the same storage as `v.push_back(x)`.
            std::string via = member != nullptr ? containerKey(member->object.get()) : std::string();
            if (!via.empty() && via != key) {
                if (auto owner = observerOf_.find(via); owner != observerOf_.end() && owner->second == key) via = key;
            }
            if (member != nullptr && via == key && mayInvalidate(member->member)) {
                const std::string spelled = containerKey(member->object.get());
                report(*call, "`" + spelled + "." + member->member + "(...)` may reallocate `" + key + "`");
            } else if (bareField && call->callee == nullptr && !currentClass_.empty()) {
                auto cls = classMethodParams_.find(currentClass_);
                if (cls != classMethodParams_.end() && cls->second.count(call->function_name) > 0 && calleeHeapEffect(call) == 2) {
                    report(*call, "`" + call->function_name + "(...)` may change `" + key + "` (it is a method of this class that is not heap-pure)");
                }
            }
        } else if (auto assign = dynamic_cast<AssignmentStatement*>(&node)) {
            const bool whole = assign->target == nullptr && assign->variable == key;
            const bool field = assign->target != nullptr && containerKey(assign->target.get()) == key;
            if (whole || field) report(*assign, "`" + key + "` is assigned a new value");
        }
    });
}

void DanglingChecker::invalidateContainerObservers(const Expression* callExpr) {
    auto call = dynamic_cast<const FunctionCall*>(callExpr);
    if (call == nullptr || !call->callee) return;
    auto member = dynamic_cast<const MemberAccess*>(call->callee.get());
    if (member == nullptr) return;
    auto id = dynamic_cast<const Identifier*>(member->object.get());
    if (id == nullptr) {
        // `other.items.push_back(x)`: a mutator on a vector reached through a member may be
        // the field a span views.
        if (auto inner = dynamic_cast<const MemberAccess*>(member->object.get());
            inner != nullptr && isVectorType(declaredTypeOf(inner)) && vectorMethodInvalidatesReferences(member->member)) {
            killFieldObservers(inner->member);
        }
        return;
    }

    auto typeIt = localType_.find(id->name);
    if (typeIt == localType_.end()) {
        // a field of the current class, named bare
        if (isVectorType(declaredTypeOf(id)) && vectorMethodInvalidatesReferences(member->member)) killFieldObservers(id->name);
        return;
    }
    if (isVectorType(typeIt->second)) {
        if (!vectorMethodInvalidatesReferences(member->member)) return;
    } else if (isStringType(typeIt->second)) {
        if (!stringMethodInvalidatesReferences(member->member)) return;
    } else if (isMapType(typeIt->second)) {
        if (!mapMethodInvalidatesReferences(member->member)) return;
    } else {
        return;
    }

    // The container may have reallocated: every observer of it is now dangling.
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

void DanglingChecker::killObserversOfOpaqueCalls(const Expression* expr) {
    if (expr == nullptr) return;
    if (auto call = dynamic_cast<const FunctionCall*>(expr)) {
        if (auto member = dynamic_cast<const MemberAccess*>(call->callee.get())) {
            if (auto recv = dynamic_cast<const Identifier*>(member->object.get())) {
                auto typeIt = localType_.find(recv->name);
                if (typeIt != localType_.end() && callKillsReceiverObservers(typeIt->second, member->member)) {
                    for (const auto& [obs, owner] : observerOf_) {
                        if (owner == recv->name) danglingObservers_.insert(obs);
                    }
                }
                if (typeIt != localType_.end() && isConservativeOpaqueCall(typeIt->second, member->member)) {
                    // An unlisted method may keep a reference to an object it is handed
                    // (`g.attach(h)`); nothing models that, so refuse.
                    for (const auto& arg : call->arguments) {
                        std::string argName = asObjectLocalName(arg.get());
                        if (argName.empty()) continue;
                        DanglingError e;
                        e.line = expr->loc.line;
                        e.column = expr->loc.column;
                        e.message = "`" + recv->name + "." + member->member + "(" + argName + ")` may keep a reference to `" + argName + "`";
                        e.help = "`" + member->member + "` has no fact in a loaded pack, so the compiler assumes it may retain what it is handed, and "
                                 "nothing follows that. Declare `fresh " + typeIt->second + "::" + member->member + "` (or `borrow`) in your --facts pack "
                                 "to state that it does not retain its arguments.";
                        errors_.push_back(e);
                        break;
                    }
                }
            }
            killObserversOfOpaqueCalls(member->object.get());
        }
        for (const auto& arg : call->arguments) killObserversOfOpaqueCalls(arg.get());
        return;
    }
    if (auto bin = dynamic_cast<const BinaryExpression*>(expr)) {
        killObserversOfOpaqueCalls(bin->left.get());
        killObserversOfOpaqueCalls(bin->right.get());
    } else if (auto un = dynamic_cast<const UnaryExpression*>(expr)) {
        killObserversOfOpaqueCalls(un->operand.get());
    } else if (auto sub = dynamic_cast<const SubscriptExpression*>(expr)) {
        killObserversOfOpaqueCalls(sub->object.get());
        killObserversOfOpaqueCalls(sub->index.get());
    } else if (auto mem = dynamic_cast<const MemberAccess*>(expr)) {
        killObserversOfOpaqueCalls(mem->object.get());
    } else if (auto neww = dynamic_cast<const NewExpression*>(expr)) {
        for (const auto& arg : neww->arguments) killObserversOfOpaqueCalls(arg.get());
    }
}

void DanglingChecker::rejectOpaqueResultWithObjectArgs(const Expression* init, const Statement* at) {
    auto call = dynamic_cast<const FunctionCall*>(init);
    if (call == nullptr) return;
    auto member = dynamic_cast<const MemberAccess*>(call->callee.get());
    if (member == nullptr) return;
    auto recv = dynamic_cast<const Identifier*>(member->object.get());
    if (recv == nullptr) return;
    auto typeIt = localType_.find(recv->name);
    if (typeIt == localType_.end() || !isConservativeOpaqueCall(typeIt->second, member->member)) return;
    for (const auto& arg : call->arguments) {
        std::string argName = asObjectLocalName(arg.get());
        if (argName.empty()) continue;
        DanglingError e;
        e.line = at->loc.line;
        e.column = at->loc.column;
        e.message = "the result of `" + recv->name + "." + member->member + "(" + argName + ")` may borrow `" + argName + "` as well as `" + recv->name + "`";
        e.help = "`" + member->member + "` has no fact in a loaded pack, so the compiler assumes the worst: its result borrows the receiver and "
                 "every argument, but an observer can follow only one owner. State what it does: `fresh " + typeIt->second + "::" + member->member +
                 "` (the result owns its data) or `borrow " + typeIt->second + "::" + member->member + "` (it borrows the receiver) in your --facts pack.";
        errors_.push_back(e);
        return;
    }
}

void DanglingChecker::rejectBorrowFromFieldReceiver(const Expression* init, const Statement* at) {
    auto call = dynamic_cast<const FunctionCall*>(init);
    if (call == nullptr) return;
    auto member = dynamic_cast<const MemberAccess*>(call->callee.get());
    if (member == nullptr) return;
    std::string recvClass, recvText;
    auto fieldTypeOf = [&](const std::string& cls, const std::string& field) -> std::string {
        auto cf = classFieldTypes_.find(cls);
        if (cf == classFieldTypes_.end()) return {};
        auto ft = cf->second.find(field);
        return ft == cf->second.end() ? std::string{} : ft->second;
    };
    // The type and spelling of a receiver that is a field or a chain of fields; "" otherwise.
    std::function<std::string(const Expression*, std::string&)> typeOf = [&](const Expression* e, std::string& text) -> std::string {
        if (auto id = dynamic_cast<const Identifier*>(e)) {
            text = id->name;
            auto lt = localType_.find(id->name);
            if (lt != localType_.end()) return lt->second;
            return fieldTypeOf(currentClass_, id->name);              // a bare name that is a field of the current class
        }
        if (auto mem = dynamic_cast<const MemberAccess*>(e)) {
            std::string baseText;
            std::string baseType = typeOf(mem->object.get(), baseText);
            text = baseText + "." + mem->member;
            return baseType.empty() ? std::string{} : fieldTypeOf(baseType, mem->member);
        }
        return {};
    };
    if (auto id = dynamic_cast<const Identifier*>(member->object.get())) {
        if (localType_.count(id->name) > 0) return;               // a local or parameter: tracked by the ordinary rules
    }
    recvClass = typeOf(member->object.get(), recvText);
    if (recvClass.empty() || !isKnownExternalClassType(recvClass) || !isKnownBorrowingMethod(recvClass, member->member)) return;
    DanglingError e;
    e.line = at->loc.line;
    e.column = at->loc.column;
    e.message = "`" + member->member + "` returns something that borrows `" + recvText + "`, which is a field";
    e.help = "A field has no owner the analysis follows, so the borrow would be dropped silently (the field could be replaced while the result lives). "
             "Copy the object into a local first, work on the local, or declare `fresh " + recvClass + "::" + member->member + "` if the result owns its data.";
    errors_.push_back(e);
}

void DanglingChecker::checkExpressionUses(const Expression* expr) {
    if (expr == nullptr) return;
    struct DepthGuard {
        int& d;
        explicit DepthGuard(int& depth) : d(depth) { ++d; }
        ~DepthGuard() { --d; }
    };
    if (exprDepth_ == 0) killObserversOfOpaqueCalls(expr);   // the outermost expression of a statement: kills first
    DepthGuard guard(exprDepth_);

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
        // Use of an observer whose object is gone: the owner was moved away or killed.
        if (auto it = observerOf_.find(id->name); it != observerOf_.end()) {
            const std::string& owner = it->second;
            // An alias observer's owner is moved-from by construction, which alone does not
            // mean the object is gone.
            const bool isAlias = aliasObservers_.find(id->name) != aliasObservers_.end();
            const bool ownerMoved = !isAlias && movedFrom_.find(owner) != movedFrom_.end();
            const bool killed = danglingObservers_.find(id->name) != danglingObservers_.end();
            if (ownerMoved || killed) {
                DanglingError e;
                e.line = expr->loc.line;
                e.column = expr->loc.column;
                e.message = "use of dangling observer `" + id->name +
                            "` (" + (!owner.empty() && owner[0] == '@' ? "the field `" + owner.substr(1) + "` may have been changed" : "its owner `" + owner + "` is gone") + ")";
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
        // A method call's receiver is a use too (`v.size()` after `v` was moved).
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

void DanglingChecker::bindStringView(const std::string& name, const Expression* init, const Statement* at) {
    auto fail = [&](const std::string& what, const std::string& help) {
        DanglingError e;
        e.line = at->loc.line;
        e.column = at->loc.column;
        e.message = "cannot tell what the `string_view` `" + name + "` would refer to: " + what;
        e.help = help;
        errors_.push_back(e);
    };
    std::string owner;
    if (init == nullptr) {
        fail("it has no initializer", "Initialize the view from a string variable or a string literal.");
        return;
    }
    if (dynamic_cast<const StringLiteral*>(init) != nullptr) {
        // a literal has static storage: the view cannot dangle
    } else if (auto id = dynamic_cast<const Identifier*>(init)) {
        auto typeIt = localType_.find(id->name);
        if (typeIt != localType_.end() && isStringType(typeIt->second) && stringOwners_.count(id->name) > 0) {
            owner = id->name;
        } else if (typeIt != localType_.end() && isStringViewType(typeIt->second)) {
            auto o = observerOf_.find(id->name);          // a copy of a view follows the same string
            if (o != observerOf_.end()) owner = o->second;
        } else {
            fail("`" + id->name + "` is not a string local or parameter",
                 "A view can be made from a string variable of this function, a string literal, or another view. A field has no owner the checker follows, "
                 "so the field could be reassigned while the view lives: copy it into a local `string` first.");
            return;
        }
    } else if (auto viewCall = dynamic_cast<const FunctionCall*>(init);
               viewCall != nullptr && viewCall->callee != nullptr &&
               dynamic_cast<const MemberAccess*>(viewCall->callee.get()) != nullptr &&
               dynamic_cast<const Identifier*>(static_cast<const MemberAccess*>(viewCall->callee.get())->object.get()) != nullptr &&
               localType_.count(static_cast<const Identifier*>(static_cast<const MemberAccess*>(viewCall->callee.get())->object.get())->name) > 0 &&
               isStringViewType(localType_.at(static_cast<const Identifier*>(static_cast<const MemberAccess*>(viewCall->callee.get())->object.get())->name)) &&
               stringViewMethodReturnsView(static_cast<const MemberAccess*>(viewCall->callee.get())->member)) {
        // `v.substr(a, b)` on a view: another view of the same string
        const std::string recv = static_cast<const Identifier*>(static_cast<const MemberAccess*>(viewCall->callee.get())->object.get())->name;
        auto o = observerOf_.find(recv);
        if (o != observerOf_.end()) owner = o->second;
    } else if (dynamic_cast<const FunctionCall*>(init) != nullptr) {
        fail("it would be made from the result of a call",
             "A call that returns a `string` returns a temporary, and a view of a temporary dangles at the end of the statement. Store the result in a "
             "`string` variable and make the view from that.");
        return;
    } else {
        fail("the expression is not a string variable or a string literal",
             "A view can be made from a string variable of this function, a string literal, or another view. Anything built on the way (a concatenation) "
             "is a temporary.");
        return;
    }
    if (owner.empty()) observerOf_.erase(name);
    else observerOf_[name] = owner;
    observerNames_.insert(name);
    danglingObservers_.erase(name);
}

void DanglingChecker::bindSpan(const std::string& name, const Expression* init, const Statement* at) {
    auto fail = [&](const std::string& what, const std::string& help) {
        DanglingError e;
        e.line = at->loc.line;
        e.column = at->loc.column;
        e.message = "cannot tell what the `span` `" + name + "` would refer to: " + what;
        e.help = help;
        errors_.push_back(e);
    };
    const std::string myElem = localType_.count(name) ? spanElementType(localType_.at(name)) : std::string{};
    std::string owner;
    if (init == nullptr) {
        fail("it has no initializer", "Initialize the span from a vector variable of this function.");
        return;
    }
    // a read-only view cannot become a writable one
    auto dropsConst = [&](const std::string& sourceType) {
        if (isSpanConstType(sourceType) && !(localType_.count(name) && isSpanConstType(localType_.at(name)))) {
            fail("it would drop the `const` of `" + sourceType + "`",
                 "A `std::span<const T>` is read-only; a `std::span<T>` could write through it. Declare the new view as `std::span<const T>`.");
            return true;
        }
        return false;
    };
    auto memberCall = [](const Expression* e) -> const MemberAccess* {
        auto c = dynamic_cast<const FunctionCall*>(e);
        return c != nullptr && c->callee != nullptr ? dynamic_cast<const MemberAccess*>(c->callee.get()) : nullptr;
    };
    if (auto id = dynamic_cast<const Identifier*>(init)) {
        auto typeIt = localType_.find(id->name);
        if (typeIt != localType_.end() && isVectorType(typeIt->second) && objectLocals_.count(id->name) > 0) {
            if (vectorElementType(typeIt->second) != myElem) {
                fail("`" + id->name + "` is a `" + typeIt->second + "`, not a vector of `" + myElem + "`",
                     "A span views the elements of a vector of the same element type.");
                return;
            }
            owner = id->name;
        } else if (typeIt != localType_.end() && isSpanType(typeIt->second)) {
            if (dropsConst(typeIt->second)) return;
            auto o = observerOf_.find(id->name);          // a copy of a span follows the same vector
            if (o != observerOf_.end()) owner = o->second;
        } else if (typeIt == localType_.end() && !currentClass_.empty() && isVectorType(declaredTypeOf(id))) {
            // a vector field of this class: followed as the pseudo owner "@name"
            if (vectorElementType(declaredTypeOf(id)) != myElem) {
                fail("`" + id->name + "` is a `" + declaredTypeOf(id) + "`, not a vector of `" + myElem + "`",
                     "A span views the elements of a vector of the same element type.");
                return;
            }
            owner = "@" + id->name;
        } else {
            fail("`" + id->name + "` is not a vector local or parameter",
                 "A span can be made from a vector variable of this function, a vector field of this class, or another span.");
            return;
        }
    } else if (const MemberAccess* mc = memberCall(init);
               mc != nullptr && dynamic_cast<const Identifier*>(mc->object.get()) != nullptr &&
               localType_.count(static_cast<const Identifier*>(mc->object.get())->name) > 0 &&
               isSpanType(localType_.at(static_cast<const Identifier*>(mc->object.get())->name)) && spanMethodReturnsView(mc->member)) {
        // `s.subspan(a, n)` / `s.first(n)` / `s.last(n)` on a span: another view of the same vector
        if (dropsConst(localType_.at(static_cast<const Identifier*>(mc->object.get())->name))) return;
        auto o = observerOf_.find(static_cast<const Identifier*>(mc->object.get())->name);
        if (o != observerOf_.end()) owner = o->second;
    } else if (dynamic_cast<const FunctionCall*>(init) != nullptr) {
        fail("it would be made from the result of a call",
             "A call that returns a vector returns a temporary, and a span of a temporary dangles at the end of the statement. Store the result in a "
             "vector variable and make the span from that.");
        return;
    } else {
        fail("the expression is not a vector variable",
             "A span can be made from a vector variable of this function, or from another span.");
        return;
    }
    if (owner.empty()) observerOf_.erase(name);
    else observerOf_[name] = owner;
    observerNames_.insert(name);
    danglingObservers_.erase(name);
}

void DanglingChecker::checkBody(const std::vector<StatementPtr>& body,
                                 const std::vector<std::pair<std::string, std::string>>& params,
                                 const std::string& ownerClass, const std::string& scope) {
    objectLocals_.clear();
    localType_.clear();
    movedFrom_.clear();
    observerNames_.clear();
    observerOf_.clear();
    danglingObservers_.clear();
    aliasObservers_.clear();
    stringOwners_.clear();
    loopStack_.clear();
    currentClass_ = ownerClass;
    currentScope_ = scope;
    currentBody_ = &body;
    currentParamNames_.clear();
    for (const auto& [ptype, pname] : params) currentParamNames_.insert(pname);

    // An object-typed parameter is tracked like an object local.
    for (const auto& [type, name] : params) {
        if (isObjectType(type)) {
            objectLocals_.insert(name);
            localType_[name] = type;
        } else if (isStringType(type) || isChannelType(type)) {
            // not move-tracked; the type is recorded for the `throw` check
            localType_[name] = type;
            if (isStringType(type)) stringOwners_.insert(name);
        } else if (isViewType(type)) {
            localType_[name] = type;     // a view parameter: the caller's data outlives the call, so no owner here
        }
    }

    for (const auto& stmtPtr : body) {
        processStatement(stmtPtr.get());
    }
}

void DanglingChecker::endBlockScope(const std::set<std::string>& ownersBefore, const std::set<std::string>* stringsBefore) {
    // Owners that appeared during the block are local to it and die at block end.
    std::set<std::string> died;
    for (const auto& owner : objectLocals_) {
        if (ownersBefore.find(owner) == ownersBefore.end()) {
            died.insert(owner);
        }
    }
    if (stringsBefore != nullptr) {
        // string locals that appeared in the block die with it; a `string_view` of one that outlives the block now dangles
        std::set<std::string> diedStrings;
        for (const auto& name : stringOwners_) {
            if (stringsBefore->find(name) == stringsBefore->end()) diedStrings.insert(name);
        }
        for (const auto& name : diedStrings) {
            for (const auto& [obs, watched] : observerOf_) {
                if (watched == name) danglingObservers_.insert(obs);
            }
            stringOwners_.erase(name);
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

void DanglingChecker::recordLoopExit(bool isContinue) {
    BranchState exit = snapshotBranchState();
    // Owners declared inside the loop die on the way out; observers of them dangle in this
    // recorded copy only.
    for (const auto& owner : objectLocals_) {
        if (loopStack_.back().ownersBefore.count(owner) != 0) continue;
        for (const auto& [obs, watched] : observerOf_) {
            if (watched == owner) exit.danglingObservers.insert(obs);
        }
        exit.movedFrom.erase(owner);
    }
    auto& ctx = loopStack_.back();
    (isContinue ? ctx.continueExits : ctx.breakExits).push_back(std::move(exit));
}

void DanglingChecker::processLoop(const Expression* condition, const ForStatement* forStmt,
                                  const std::vector<StatementPtr>& body) {
    const BranchState before = snapshotBranchState();
    const std::set<std::string> ownersBefore = objectLocals_;
    const std::set<std::string> stringsBefore = stringOwners_;
    const size_t firstError = errors_.size();
    BranchState head = before;  // state at the top of the iteration being analysed
    BranchState post = before;  // state after the loop; zero iterations leaves `before`
    auto sameState = [](const BranchState& a, const BranchState& b) {
        return a.movedFrom == b.movedFrom && a.observerNames == b.observerNames &&
               a.observerOf == b.observerOf && a.danglingObservers == b.danglingObservers &&
               a.aliasObservers == b.aliasObservers;
    };
    // Two passes catch any effect that matters (a move or kill on one iteration is visible on the
    // next); a third confirms it stopped changing. The cap bounds nested-loop cost.
    for (int pass = 0; pass < 3; ++pass) {
        restoreBranchState(head);
        if (pass > 0 && condition != nullptr) checkExpressionUses(condition);
        loopStack_.push_back({ownersBefore, {}, {}});
        // The range-for variable is a fresh owner for each iteration.
        if (forStmt != nullptr && isObjectType(forStmt->loopVarType)) {
            objectLocals_.insert(forStmt->loopVarName);
            localType_[forStmt->loopVarName] = forStmt->loopVarType;
        }
        for (const auto& inner : body) {
            processStatement(inner.get());
        }
        endBlockScope(ownersBefore, &stringsBefore);
        const BranchState end = snapshotBranchState();
        LoopContext ctx = std::move(loopStack_.back());
        loopStack_.pop_back();

        // A body ending in break/continue/return/throw never falls off its end; the recorded
        // exits carry the real states.
        const bool fallsThrough = !alwaysExits(body);
        if (fallsThrough) {
            mergeBranchStates(before, post, end);
            post = snapshotBranchState();
        }
        for (const auto& e : ctx.breakExits) {
            mergeBranchStates(before, post, e);
            post = snapshotBranchState();
        }
        BranchState nextHead = head;
        if (fallsThrough) {
            mergeBranchStates(before, nextHead, end);
            nextHead = snapshotBranchState();
        }
        for (const auto& e : ctx.continueExits) {
            mergeBranchStates(before, post, e);
            post = snapshotBranchState();
            mergeBranchStates(before, nextHead, e);
            nextHead = snapshotBranchState();
        }
        if (condition != nullptr) {
            // The condition runs again at the top of the next iteration, in the state this one
            // ended in.
            const BranchState current = snapshotBranchState();
            restoreBranchState(nextHead);
            checkExpressionUses(condition);
            restoreBranchState(current);
        }
        const bool settled = sameState(nextHead, head);
        head = nextHead;
        if (settled) break;
    }
    restoreBranchState(post);
    // A later pass re-reports what an earlier one already found.
    std::vector<DanglingError> kept(errors_.begin(), errors_.begin() + firstError);
    for (size_t i = firstError; i < errors_.size(); ++i) {
        bool dup = false;
        for (size_t k = firstError; k < kept.size(); ++k) {
            if (kept[k].line == errors_[i].line && kept[k].column == errors_[i].column &&
                kept[k].message == errors_[i].message) { dup = true; break; }
        }
        if (!dup) kept.push_back(errors_[i]);
    }
    errors_ = std::move(kept);
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

    // Observer names: the union of both branches.
    observerNames_ = thenState.observerNames;
    observerNames_.insert(elseState.observerNames.begin(), elseState.observerNames.end());

    // Dangling in either branch means dangling after.
    danglingObservers_ = thenState.danglingObservers;
    danglingObservers_.insert(elseState.danglingObservers.begin(), elseState.danglingObservers.end());

    aliasObservers_ = thenState.aliasObservers;
    aliasObservers_.insert(elseState.aliasObservers.begin(), elseState.aliasObservers.end());

    // observerOf_: for each observer either branch touched, compare what each branch says it
    // observes (the pre-if binding if a branch left it alone). Agreement keeps the binding;
    // disagreement is ambiguous, so the observer is marked dangling.
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

        // A view bound to a string on one path and to nothing with an owner (a literal, a view
        // parameter) on the other dangles after the merge only if that string dies.
        auto lt = localType_.find(name);
        const bool isView = lt != localType_.end() && isViewType(lt->second);
        if (isView && thenOwner != elseOwner && (thenOwner.empty() || elseOwner.empty())) {
            observerOf_[name] = !thenOwner.empty() ? thenOwner : elseOwner;
        } else if (thenOwner == elseOwner) {
            if (thenOwner.empty()) {
                observerOf_.erase(name);
            } else {
                observerOf_[name] = thenOwner;
            }
        } else {
            // Ambiguous. The use-site check needs an observerOf_ entry to report an owner, so
            // keep one; the dangling flag is what makes a later use an error.
            observerOf_[name] = !thenOwner.empty() ? thenOwner : elseOwner;
            danglingObservers_.insert(name);
        }
    }
}

void DanglingChecker::processStatement(const Statement* stmt) {
    if (auto decl = dynamic_cast<const VariableDeclaration*>(stmt)) {
        // Uses on the RHS are evaluated before the declaration completes.
        checkExpressionUses(decl->expression.get());

        if (isStringViewType(decl->type)) {
            localType_[decl->identifier] = decl->type;
            bindStringView(decl->identifier, decl->expression.get(), decl);
            return;
        }
        if (isSpanType(decl->type)) {
            localType_[decl->identifier] = decl->type;
            bindSpan(decl->identifier, decl->expression.get(), decl);
            return;
        }
        if (isStringType(decl->type) && !decl->isObserver) {
            stringOwners_.insert(decl->identifier);
            // a fresh string: a view of an earlier string of the same name is dead
            for (const auto& [obs, watched] : observerOf_) {
                if (watched == decl->identifier) danglingObservers_.insert(obs);
            }
        }

        if (decl->isObserver) {
            // `ref Node b = ...;` observes its RHS and does not move it. `ref Node b = nullptr;`
            // records an observer with no owner yet.
            //
            // A value type has no owner to observe and the emitted `.get()` lowering would not
            // compile for it, so it is rejected.
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
            std::string boundOwner;
            bool inheritsDangling = false;
            const bool trackable = !isObjectType(decl->type) ||
                resolveRefBinding(decl->expression.get(), boundOwner, inheritsDangling);
            if (!trackable) {
                reportUntrackableRefBinding(decl->identifier, decl->expression.get(), decl);
            }
            observerNames_.insert(decl->identifier);
            danglingObservers_.erase(decl->identifier);
            if (isObjectType(decl->type)) {
                localType_[decl->identifier] = decl->type;
                // `ref Node n = v[i];` observes the container `v`.
                if (!boundOwner.empty()) {
                    observerOf_[decl->identifier] = boundOwner;
                }
                if (inheritsDangling) {
                    danglingObservers_.insert(decl->identifier);
                }
            }
        } else {
            // `Node b = a;` moves the source, except for a value type, where it is an ordinary
            // copy and `a` stays live. A value type passed as a call argument is still moved.
            const bool srcIsValueTypeCopy = analyzer_ != nullptr &&
                analyzer_->isValueTypeClass(decl->type);
            if (isObjectType(decl->type) && decl->expression && !srcIsValueTypeCopy) {
                if (std::string src = asObjectLocalName(decl->expression.get());
                    !src.empty() && !isSharedLocal(src)) {
                    movedFrom_.insert(src);
                }
            }
            // A call RHS moves its object-typed arguments into the callee. A borrowing method
            // call like `a.block(...)` leaves the receiver `a` untouched.
            applyCallArgMoves(decl->expression.get());
            // `Node b = f(a);` where f's summary says the result aliases that parameter: `a` is
            // still moved, but `b` observes its object like an explicit `ref`.
            std::string aliasedOwner;
            if (isObjectType(decl->type) && decl->expression) {
                aliasedOwner = resolveCallReturnAlias(decl->expression.get());
            }
            // `Node b = a.block(...);` (an allow-listed borrowing method of an external class):
            // `b` observes the receiver `a`, which stays live. It is not added to
            // aliasObservers_, because `a` was not moved by the call; a later reassignment or
            // move of `a` kills `b`.
            std::string borrowedOwner;
            if (aliasedOwner.empty() && isObjectType(decl->type) && decl->expression) {
                borrowedOwner = resolveBorrowingMethodReceiver(decl->expression.get());
                if (!borrowedOwner.empty()) rejectOpaqueResultWithObjectArgs(decl->expression.get(), decl);
                else rejectBorrowFromFieldReceiver(decl->expression.get(), decl);
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
                // A fresh declaration revives the name.
                objectLocals_.insert(decl->identifier);
                movedFrom_.erase(decl->identifier);
                localType_[decl->identifier] = decl->type;
            } else if (isStringType(decl->type) || isChannelType(decl->type)) {
                // not move-tracked; the type is recorded for the `throw` check
                localType_[decl->identifier] = decl->type;
            }
        }
    } else if (auto assign = dynamic_cast<const AssignmentStatement*>(stmt)) {
        checkExpressionUses(assign->expression.get());

        if (!assign->target) {
            auto vt = localType_.find(assign->variable);
            if (vt != localType_.end() && isStringViewType(vt->second)) {
                bindStringView(assign->variable, assign->expression.get(), assign);    // a view rebinds, like an observer
                return;
            }
            if (vt != localType_.end() && isSpanType(vt->second)) {
                bindSpan(assign->variable, assign->expression.get(), assign);          // so does a span
                return;
            }
            if (vt != localType_.end() && isStringType(vt->second) && stringOwners_.count(assign->variable) > 0) {
                // reassigning a string replaces its characters: every view of it is dead
                for (const auto& [obs, watched] : observerOf_) {
                    if (watched == assign->variable) danglingObservers_.insert(obs);
                }
            }
        }

        const bool isBareFieldStore = !assign->target && !currentClass_.empty() &&
            objectLocals_.find(assign->variable) == objectLocals_.end() &&
            observerNames_.find(assign->variable) == observerNames_.end() &&
            classFields_.count(currentClass_) > 0 &&
            classFields_.at(currentClass_).count(assign->variable) > 0;
        if (assign->target || isBareFieldStore) {
            // A member target `a.b = ...` or a bare field name (`link = other;` in a method)
            // links a field: it neither moves the root object nor consumes the RHS. A local or
            // observer of the same name shadows the field. The receiver of a member target
            // must not already be dead.
            if (assign->target) {
                checkExpressionUses(assign->target.get());
            }
            // a store into a field replaces what a span of that field points into (by name)
            if (auto m = dynamic_cast<const MemberAccess*>(assign->target.get())) killFieldObservers(m->member);
            else if (!assign->target) killFieldObservers(assign->variable);
        } else if (observerNames_.find(assign->variable) != observerNames_.end()) {
            // Assigning to an observer rebinds it (`b = inner;`); it loses any alias exemption.
            danglingObservers_.erase(assign->variable);
            aliasObservers_.erase(assign->variable);
            std::string boundOwner;
            bool inheritsDangling = false;
            auto obsTypeIt = localType_.find(assign->variable);
            const bool objectObserver = obsTypeIt != localType_.end() &&
                isObjectType(obsTypeIt->second);
            if (objectObserver &&
                !resolveRefBinding(assign->expression.get(), boundOwner, inheritsDangling)) {
                reportUntrackableRefBinding(assign->variable, assign->expression.get(), assign);
            }
            if (!boundOwner.empty()) {
                observerOf_[assign->variable] = boundOwner;
            } else {
                observerOf_.erase(assign->variable);
            }
            if (inheritsDangling) {
                danglingObservers_.insert(assign->variable);
            }
        } else if (isOperatorAssignMutation(*assign)) {
            // `n = "text";` on a class with its own `operator=` is a mutating call on the
            // live object, not an ownership handoff.
        } else {
            // Reassigning an owner frees the object it held: its observers dangle.
            if (objectLocals_.find(assign->variable) != objectLocals_.end()) {
                for (const auto& [obs, owner] : observerOf_) {
                    if (owner == assign->variable) {
                        danglingObservers_.insert(obs);
                    }
                }
            }
            // Reassigning a local revives the name.
            movedFrom_.erase(assign->variable);
            // An object-local RHS is moved, unless it is a shared handle (copied, stays live).
            if (std::string src = asObjectLocalName(assign->expression.get());
                !src.empty() && !isSharedLocal(src)) {
                movedFrom_.insert(src);
            }
            applyCallArgMoves(assign->expression.get());
            // `w = g.window(...);` (a borrowing method of an external class): `w` stops being an
            // owner and observes the receiver, as in the declaration form.
            if (objectLocals_.find(assign->variable) != objectLocals_.end()) {
                if (std::string borrowedOwner = resolveBorrowingMethodReceiver(assign->expression.get()); !borrowedOwner.empty()) {
                    objectLocals_.erase(assign->variable);
                    observerNames_.insert(assign->variable);
                    danglingObservers_.erase(assign->variable);
                    observerOf_[assign->variable] = borrowedOwner;
                } else {
                    rejectBorrowFromFieldReceiver(assign->expression.get(), assign);
                }
            }
            // `x = f(a);` where f's result aliases a parameter: `x` observes that argument,
            // as in the declaration case.
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
        applyCallArgMoves(ret->expression.get());
        checkReturnedReference(ret->expression.get());
    } else if (auto ifs = dynamic_cast<const IfStatement*>(stmt)) {
        checkExpressionUses(ifs->condition.get());
        applyCallArgMoves(ifs->condition.get());
        // Each branch is analyzed from the same snapshot, then merged conservatively
        // (mergeBranchStates).
        const BranchState before = snapshotBranchState();
        BranchState thenState = before;
        {
            const std::set<std::string> ownersBefore = objectLocals_;
            const std::set<std::string> stringsBefore = stringOwners_;
            for (const auto& inner : ifs->body) {
                processStatement(inner.get());
            }
            endBlockScope(ownersBefore, &stringsBefore);
            thenState = snapshotBranchState();
        }
        restoreBranchState(before);
        BranchState elseState = before;
        if (!ifs->elseBody.empty()) {
            const std::set<std::string> ownersBefore = objectLocals_;
            const std::set<std::string> stringsBefore = stringOwners_;
            for (const auto& inner : ifs->elseBody) {
                processStatement(inner.get());
            }
            endBlockScope(ownersBefore, &stringsBefore);
            elseState = snapshotBranchState();
        }
        // A branch that always exits never reaches the code after the `if`, so when only one
        // side exits, the other side's state is used as-is.
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
        applyCallArgMoves(whiles->condition.get());      // processLoop re-checks it for later iterations
        // The body runs zero or more times; see processLoop.
        processLoop(whiles->condition.get(), nullptr, whiles->body);
    } else if (auto cdecl = dynamic_cast<const ContainerDeclaration*>(stmt)) {
        // `std::vector<T> v;` has no initializer: a fresh owner, nothing to check or move.
        objectLocals_.insert(cdecl->identifier);
        movedFrom_.erase(cdecl->identifier);
        localType_[cdecl->identifier] = cdecl->type;
    } else if (auto fors = dynamic_cast<const ForStatement*>(stmt)) {
        checkExpressionUses(fors->container.get());
        applyCallArgMoves(fors->container.get());
        checkIteratedContainerNotMutated(fors);
        processLoop(nullptr, fors, fors->body);
    } else if (auto exprStmt = dynamic_cast<const ExpressionStatement*>(stmt)) {
        // e.g. `sink(a);`: check uses, then apply the moves.
        checkExpressionUses(exprStmt->expression.get());
        applyCallArgMoves(exprStmt->expression.get());
    } else if (dynamic_cast<const BreakStatement*>(stmt) != nullptr ||
               dynamic_cast<const ContinueStatement*>(stmt) != nullptr) {
        if (loopStack_.empty()) {
            DanglingError e;
            e.line = stmt->loc.line;
            e.column = stmt->loc.column;
            e.message = std::string(dynamic_cast<const BreakStatement*>(stmt) ? "`break`" : "`continue`") +
                        " outside a loop";
            e.help = "`break` and `continue` apply to the innermost enclosing `while` or `for`; "
                     "there is no `switch` and no labelled form.";
            errors_.push_back(e);
        } else {
            recordLoopExit(dynamic_cast<const ContinueStatement*>(stmt) != nullptr);
        }
    } else if (auto throwStmt = dynamic_cast<const ThrowStatement*>(stmt)) {
        checkExpressionUses(throwStmt->expression.get());
        applyCallArgMoves(throwStmt->expression.get());
        // `throw` takes only string-valued operands.
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
    } else if (auto tryStmt = dynamic_cast<const TryStatement*>(stmt)) {
        // The try body may throw at any point, so the catch block is analyzed from the merge
        // of "try never ran" and "try ran to completion". The state after the whole construct
        // merges "try succeeded" with "catch ran".
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
        // The caught variable is a string; its type is recorded so a re-throw of it is valid.
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
