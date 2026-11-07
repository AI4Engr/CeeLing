// SmartPointerAnalyzer: value types, read-only parameters, last-use and heap-pure classification
#include "passes/smart_pointer_analyzer.h"
#include "passes/smart_pointer_util.h"
#include "syntax/ast_walk.h"

#include "syntax/class_hierarchy.h"
#include "facts/library_summary.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <iostream>
#include <optional>
#include <regex>
#include <sstream>


namespace cling {

void SmartPointerAnalyzer::classifyValueTypeClasses(const Program& program) {
    for (const auto& stmt : program.statements) {
        auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get());
        if (!cls) continue;
        // Two routes into valueTypeClasses_: structural inference (a class with the iterator protocol operator*/operator++/operator!=) or an explicit `struct`
        // (ClassDeclaration::isValueType, set at parse time).
        bool hasStar = false, hasIncrement = false, hasNotEqual = false;
        for (const auto& member : cls->members) {
            auto method = dynamic_cast<const MethodDeclaration*>(member.get());
            if (!method) continue;
            if (method->name == "operator*") hasStar = true;
            else if (method->name == "operator++") hasIncrement = true;
            else if (method->name == "operator!=") hasNotEqual = true;
        }
        if (cls->isValueType || (hasStar && hasIncrement && hasNotEqual)) {
            valueTypeClasses_.insert(cls->name);
        }
    }
    // A known-external class has no ClassDeclaration, so it is added to valueTypeClasses_ here; `new Eigen::MatrixXd(...)` then emits as a bare constructor
    // call. Covers every `class` fact of every loaded fact pack.
    for (const std::string& external : knownExternalClassTypes()) {
        valueTypeClasses_.insert(external);
    }
}

bool SmartPointerAnalyzer::isValueTypeClass(const std::string& className) const {
    return valueTypeClasses_.find(className) != valueTypeClasses_.end();
}

bool SmartPointerAnalyzer::paramUseDisqualifiesReadOnly(
    const Expression* expr, const std::string& paramName, const std::string& currentClass,
    const std::set<std::string>& okCallees) const {
    if (expr == nullptr) return false;
    // A bare occurrence of paramName is what move detection keys on; `paramName.field` is a MemberAccess and never disqualifies (its object is not recursed into).
    if (auto id = dynamic_cast<const Identifier*>(expr)) {
        return id->name == paramName;
    }
    if (auto bin = dynamic_cast<const BinaryExpression*>(expr)) {
        // `paramName == x` / `!= x` compares identity; it reads the smart pointer without consuming or storing it. Only a bare paramName operand is exempt.
        if (bin->operator_str == "==" || bin->operator_str == "!=") {
            auto isBareParam = [&](const Expression* e) {
                auto id = dynamic_cast<const Identifier*>(e);
                return id != nullptr && id->name == paramName;
            };
            const bool leftOk = isBareParam(bin->left.get()) ||
                !paramUseDisqualifiesReadOnly(bin->left.get(), paramName, currentClass, okCallees);
            const bool rightOk = isBareParam(bin->right.get()) ||
                !paramUseDisqualifiesReadOnly(bin->right.get(), paramName, currentClass, okCallees);
            return !(leftOk && rightOk);
        }
        return paramUseDisqualifiesReadOnly(bin->left.get(), paramName, currentClass, okCallees) ||
               paramUseDisqualifiesReadOnly(bin->right.get(), paramName, currentClass, okCallees);
    }
    if (auto un = dynamic_cast<const UnaryExpression*>(expr)) {
        // `!paramName` (null check) reads and does not consume. A nested use inside the operand is still checked.
        if (un->operator_str == "!") {
            auto operandId = dynamic_cast<const Identifier*>(un->operand.get());
            if (operandId != nullptr && operandId->name == paramName) return false;
        }
        return paramUseDisqualifiesReadOnly(un->operand.get(), paramName, currentClass, okCallees);
    }
    if (auto call = dynamic_cast<const FunctionCall*>(expr)) {
        // A bare paramName passed as a call argument disqualifies unless the callee's parameter at that position is already read-only (`okCallees`, a fixed
        // point over free functions and methods). `!call->callee` covers a free call and an implicit-this method call; with `currentClass` set the method key is
        // tried first (a same-class bare call resolves to a sibling method), then the free-function key.
        for (size_t i = 0; i < call->arguments.size(); ++i) {
            auto argId = dynamic_cast<const Identifier*>(call->arguments[i].get());
            if (argId != nullptr && argId->name == paramName && !call->callee) {
                const std::string methodKey =
                    currentClass + "::" + call->function_name + "::" + std::to_string(i);
                const std::string freeKey = call->function_name + "::" + std::to_string(i);
                if ((!currentClass.empty() && okCallees.count(methodKey) > 0) ||
                    okCallees.count(freeKey) > 0) {
                    continue;  // passed to an already-read-only param
                }
            }
            if (paramUseDisqualifiesReadOnly(call->arguments[i].get(), paramName, currentClass, okCallees)) {
                return true;
            }
        }
        if (call->callee) {
            if (auto member = dynamic_cast<const MemberAccess*>(call->callee.get())) {
                // Bare paramName as the receiver of a method call is a read, not a move; only a nested receiver expression is descended into.
                if (auto recvMember = dynamic_cast<const MemberAccess*>(member->object.get())) {
                    return paramUseDisqualifiesReadOnly(recvMember, paramName, currentClass, okCallees);
                }
            }
        }
        return false;
    }
    // MemberAccess, SubscriptExpression, NewExpression and literals never disqualify on their own (a MemberAccess's object is not recursed into: that is the read-only access).
    return false;
}

void SmartPointerAnalyzer::classifyReadOnlyFreeFunctionParams(const Program& program) {
    // A parameter never used has no disqualifying use, but that is not evidence of read-only, and treating it so would break the ordinary move of its argument.
    // At least one genuine read of `paramName.field` (or a null check on the bare parameter) is required.
    std::function<bool(const Expression*, const std::string&, const std::string&, const std::set<std::string>&)>
        exprHasGenuineRead = [&](const Expression* expr, const std::string& paramName,
                                  const std::string& currentClass, const std::set<std::string>& okCallees) -> bool {
        if (expr == nullptr) return false;
        if (auto member = dynamic_cast<const MemberAccess*>(expr)) {
            if (auto id = dynamic_cast<const Identifier*>(member->object.get())) {
                if (id->name == paramName) return true;
            }
            return exprHasGenuineRead(member->object.get(), paramName, currentClass, okCallees);
        }
        // `paramName[index]` is as genuine a read as `paramName.field`.
        if (auto sub = dynamic_cast<const SubscriptExpression*>(expr)) {
            if (auto id = dynamic_cast<const Identifier*>(sub->object.get())) {
                if (id->name == paramName) return true;
            }
            return exprHasGenuineRead(sub->object.get(), paramName, currentClass, okCallees) ||
                   exprHasGenuineRead(sub->index.get(), paramName, currentClass, okCallees);
        }
        if (auto un = dynamic_cast<const UnaryExpression*>(expr)) {
            if (un->operator_str == "!") {
                auto operandId = dynamic_cast<const Identifier*>(un->operand.get());
                if (operandId != nullptr && operandId->name == paramName) return true;
            }
            return exprHasGenuineRead(un->operand.get(), paramName, currentClass, okCallees);
        }
        if (auto bin = dynamic_cast<const BinaryExpression*>(expr)) {
            if (bin->operator_str == "==" || bin->operator_str == "!=") {
                for (const Expression* side : {bin->left.get(), bin->right.get()}) {
                    auto sideId = dynamic_cast<const Identifier*>(side);
                    if (sideId != nullptr && sideId->name == paramName) return true;
                }
            }
            return exprHasGenuineRead(bin->left.get(), paramName, currentClass, okCallees) ||
                   exprHasGenuineRead(bin->right.get(), paramName, currentClass, okCallees);
        }
        if (auto call = dynamic_cast<const FunctionCall*>(expr)) {
            // A bare paramName passed to an already read-only callee parameter (okCallees) counts as a genuine read too, mirroring paramUseDisqualifiesReadOnly. Method-scoped key first.
            for (size_t i = 0; i < call->arguments.size(); ++i) {
                auto argId = dynamic_cast<const Identifier*>(call->arguments[i].get());
                if (argId != nullptr && argId->name == paramName && !call->callee) {
                    const std::string methodKey =
                        currentClass + "::" + call->function_name + "::" + std::to_string(i);
                    const std::string freeKey = call->function_name + "::" + std::to_string(i);
                    if ((!currentClass.empty() && okCallees.count(methodKey) > 0) ||
                        okCallees.count(freeKey) > 0) {
                        return true;
                    }
                }
                if (exprHasGenuineRead(call->arguments[i].get(), paramName, currentClass, okCallees)) return true;
            }
            if (call->callee && exprHasGenuineRead(call->callee.get(), paramName, currentClass, okCallees)) {
                return true;
            }
            return false;
        }
        return false;
    };
    std::function<bool(const std::vector<StatementPtr>&, const std::string&, const std::string&,
                        const std::set<std::string>&)>
        bodyHasGenuineRead = [&](const std::vector<StatementPtr>& body, const std::string& paramName,
                                  const std::string& currentClass, const std::set<std::string>& okCallees) -> bool {
        for (const auto& stmtPtr : body) {
            const Statement* stmt = stmtPtr.get();
            if (auto ret = dynamic_cast<const ReturnStatement*>(stmt)) {
                if (exprHasGenuineRead(ret->expression.get(), paramName, currentClass, okCallees)) return true;
            } else if (auto decl = dynamic_cast<const VariableDeclaration*>(stmt)) {
                if (exprHasGenuineRead(decl->expression.get(), paramName, currentClass, okCallees)) return true;
            } else if (auto assign = dynamic_cast<const AssignmentStatement*>(stmt)) {
                if (exprHasGenuineRead(assign->expression.get(), paramName, currentClass, okCallees)) return true;
                if (assign->target &&
                    exprHasGenuineRead(assign->target.get(), paramName, currentClass, okCallees)) return true;
            } else if (auto exprStmt = dynamic_cast<const ExpressionStatement*>(stmt)) {
                if (exprHasGenuineRead(exprStmt->expression.get(), paramName, currentClass, okCallees)) return true;
            } else if (auto ifs = dynamic_cast<const IfStatement*>(stmt)) {
                if (exprHasGenuineRead(ifs->condition.get(), paramName, currentClass, okCallees)) return true;
                if (bodyHasGenuineRead(ifs->body, paramName, currentClass, okCallees)) return true;
                if (bodyHasGenuineRead(ifs->elseBody, paramName, currentClass, okCallees)) return true;
            } else if (auto whiles = dynamic_cast<const WhileStatement*>(stmt)) {
                if (exprHasGenuineRead(whiles->condition.get(), paramName, currentClass, okCallees)) return true;
                if (bodyHasGenuineRead(whiles->body, paramName, currentClass, okCallees)) return true;
            } else if (auto fors = dynamic_cast<const ForStatement*>(stmt)) {
                if (exprHasGenuineRead(fors->container.get(), paramName, currentClass, okCallees)) return true;
                if (bodyHasGenuineRead(fors->body, paramName, currentClass, okCallees)) return true;
            } else if (auto throwStmt = dynamic_cast<const ThrowStatement*>(stmt)) {
                if (exprHasGenuineRead(throwStmt->expression.get(), paramName, currentClass, okCallees)) return true;
            } else if (auto trys = dynamic_cast<const TryStatement*>(stmt)) {
                if (bodyHasGenuineRead(trys->body, paramName, currentClass, okCallees)) return true;
                if (bodyHasGenuineRead(trys->catchBody, paramName, currentClass, okCallees)) return true;
            }
        }
        return false;
    };

    // Statement-level walk over a body for any use of paramName that would require it to be consumed (it can be an assignment or call argument, not only a
    // return). `okCallees` lets a fixed-point round re-check with a growing set of read-only callees.
    std::function<bool(const std::vector<StatementPtr>&, const std::string&, const std::string&,
                        const std::set<std::string>&)>
        bodyDisqualifies = [&](const std::vector<StatementPtr>& body, const std::string& paramName,
                                const std::string& currentClass, const std::set<std::string>& okCallees) -> bool {
        for (const auto& stmtPtr : body) {
            const Statement* stmt = stmtPtr.get();
            if (auto ret = dynamic_cast<const ReturnStatement*>(stmt)) {
                if (paramUseDisqualifiesReadOnly(ret->expression.get(), paramName, currentClass, okCallees)) {
                    return true;
                }
            } else if (auto decl = dynamic_cast<const VariableDeclaration*>(stmt)) {
                if (paramUseDisqualifiesReadOnly(decl->expression.get(), paramName, currentClass, okCallees)) {
                    return true;
                }
            } else if (auto assign = dynamic_cast<const AssignmentStatement*>(stmt)) {
                if (paramUseDisqualifiesReadOnly(assign->expression.get(), paramName, currentClass, okCallees)) {
                    return true;
                }
                if (assign->target &&
                    paramUseDisqualifiesReadOnly(assign->target.get(), paramName, currentClass, okCallees)) {
                    return true;
                }
            } else if (auto exprStmt = dynamic_cast<const ExpressionStatement*>(stmt)) {
                if (paramUseDisqualifiesReadOnly(exprStmt->expression.get(), paramName, currentClass, okCallees)) {
                    return true;
                }
            } else if (auto ifs = dynamic_cast<const IfStatement*>(stmt)) {
                if (paramUseDisqualifiesReadOnly(ifs->condition.get(), paramName, currentClass, okCallees)) {
                    return true;
                }
                if (bodyDisqualifies(ifs->body, paramName, currentClass, okCallees)) return true;
                if (bodyDisqualifies(ifs->elseBody, paramName, currentClass, okCallees)) return true;
            } else if (auto whiles = dynamic_cast<const WhileStatement*>(stmt)) {
                if (paramUseDisqualifiesReadOnly(whiles->condition.get(), paramName, currentClass, okCallees)) {
                    return true;
                }
                if (bodyDisqualifies(whiles->body, paramName, currentClass, okCallees)) return true;
            } else if (auto fors = dynamic_cast<const ForStatement*>(stmt)) {
                if (paramUseDisqualifiesReadOnly(fors->container.get(), paramName, currentClass, okCallees)) {
                    return true;
                }
                if (bodyDisqualifies(fors->body, paramName, currentClass, okCallees)) return true;
            } else if (auto throwStmt = dynamic_cast<const ThrowStatement*>(stmt)) {
                if (paramUseDisqualifiesReadOnly(throwStmt->expression.get(), paramName, currentClass, okCallees)) {
                    return true;
                }
            } else if (auto trys = dynamic_cast<const TryStatement*>(stmt)) {
                if (bodyDisqualifies(trys->body, paramName, currentClass, okCallees)) return true;
                if (bodyDisqualifies(trys->catchBody, paramName, currentClass, okCallees)) return true;
            }
        }
        return false;
    };

    // Fixed point over all free functions and class methods: a function passing paramName bare into another's already read-only parameter is itself read-only,
    // but the target may only be classified on a later round. Bounded by the number of functions and methods, since each round must classify a new pair.
    std::set<std::string> classified;         // "fnName::paramName", free functions
    std::set<std::string> classifiedMethods;  // "ClassName::methodName::paramName"
    bool changed = true;
    while (changed) {
        changed = false;
        // okCallees: "fnName::argIndex" for a read-only free-function parameter, "ClassName::methodName::argIndex" for a method parameter. Built each round from the
        // classified sets, so a newly classified callee is visible from the next round.
        std::set<std::string> okCallees;
        for (const auto& stmt : program.statements) {
            if (auto fn2 = dynamic_cast<const FunctionDeclaration*>(stmt.get())) {
                for (size_t i = 0; i < fn2->parameters.size(); ++i) {
                    if (classified.count(fn2->name + "::" + fn2->parameters[i].second) > 0) {
                        okCallees.insert(fn2->name + "::" + std::to_string(i));
                    }
                }
            } else if (auto cls2 = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
                for (const auto& member : cls2->members) {
                    auto method2 = dynamic_cast<const MethodDeclaration*>(member.get());
                    if (method2 == nullptr) continue;
                    for (size_t i = 0; i < method2->parameters.size(); ++i) {
                        const std::string key =
                            cls2->name + "::" + method2->name + "::" + method2->parameters[i].second;
                        if (classifiedMethods.count(key) > 0) {
                            okCallees.insert(cls2->name + "::" + method2->name + "::" + std::to_string(i));
                        }
                    }
                }
            }
        }

        for (const auto& stmt : program.statements) {
            if (auto fn = dynamic_cast<const FunctionDeclaration*>(stmt.get())) {
                for (const auto& [ptype, pname] : fn->parameters) {
                    const std::string classifiedKey = fn->name + "::" + pname;
                    if (classified.count(classifiedKey) > 0) continue;  // already decided

                    // Only object-typed parameters are a move-vs-share question.
                    bool isClassType = false;
                    for (const auto& s2 : program.statements) {
                        if (auto cls = dynamic_cast<const ClassDeclaration*>(s2.get())) {
                            if (cls->name == ptype) { isClassType = true; break; }
                        }
                    }
                    if (!isClassType) continue;
                    if (!bodyDisqualifies(fn->body, pname, "", okCallees) &&
                        bodyHasGenuineRead(fn->body, pname, "", okCallees)) {
                        classified.insert(classifiedKey);
                        changed = true;
                    }
                }
            } else if (auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
                for (const auto& member : cls->members) {
                    auto method = dynamic_cast<const MethodDeclaration*>(member.get());
                    // Specializations are excluded: classifiedKey is keyed by bare method name, and a specialization's out-parameter has its own mechanism (outParamIndex_).
                    if (method == nullptr || !method->specializationArg.empty()) continue;
                    for (const auto& [ptype, pname] : method->parameters) {
                        const std::string classifiedKey = cls->name + "::" + method->name + "::" + pname;
                        if (classifiedMethods.count(classifiedKey) > 0) continue;  // already decided

                        bool isClassType = false;
                        for (const auto& s2 : program.statements) {
                            if (auto cls2 = dynamic_cast<const ClassDeclaration*>(s2.get())) {
                                if (cls2->name == ptype) { isClassType = true; break; }
                            }
                        }
                        if (!isClassType) continue;
                        if (!bodyDisqualifies(method->body, pname, cls->name, okCallees) &&
                            bodyHasGenuineRead(method->body, pname, cls->name, okCallees)) {
                            classifiedMethods.insert(classifiedKey);
                            changed = true;
                        }
                    }
                }
            }
        }
    }

    for (const auto& key : classified) {
        readOnlyFreeFunctionParams_.insert(key);
    }
    for (const auto& key : classifiedMethods) {
        readOnlyMethodParams_.insert(key);
    }
    // Also register under the qualified name; isReadOnlyFreeFunctionParam is queried with the call site's spelling, qualified from outside the namespace.
    for (const auto& stmt : program.statements) {
        auto fn = dynamic_cast<const FunctionDeclaration*>(stmt.get());
        if (fn == nullptr || fn->namespaceName.empty()) continue;
        for (const auto& [ptype, pname] : fn->parameters) {
            if (classified.count(fn->name + "::" + pname) > 0) {
                readOnlyFreeFunctionParams_.insert(fn->namespaceName + "::" + fn->name + "::" + pname);
            }
        }
    }
    for (const auto& stmt : program.statements) {
        auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get());
        if (cls == nullptr || cls->namespaceName.empty()) continue;
        for (const auto& member : cls->members) {
            auto method = dynamic_cast<const MethodDeclaration*>(member.get());
            if (method == nullptr || !method->specializationArg.empty()) continue;
            for (const auto& [ptype, pname] : method->parameters) {
                if (classifiedMethods.count(cls->name + "::" + method->name + "::" + pname) > 0) {
                    readOnlyMethodParams_.insert(cls->namespaceName + "::" + cls->name + "::" + method->name +
                                                  "::" + pname);
                }
            }
        }
    }
}

bool SmartPointerAnalyzer::paramUseDisqualifiesReadOnlyContainer(
    const Expression* expr, const std::string& paramName, const std::string& currentClass,
    const std::set<std::string>& okContainerCallees) const {
    if (expr == nullptr) return false;
    // A bare occurrence of paramName disqualifies unless its context (subscript read, .size() receiver, range-for container, read-only callee argument) clears it
    // earlier; this function is only called on positions that are not such a context.
    if (auto id = dynamic_cast<const Identifier*>(expr)) {
        return id->name == paramName;
    }
    if (auto bin = dynamic_cast<const BinaryExpression*>(expr)) {
        return paramUseDisqualifiesReadOnlyContainer(bin->left.get(), paramName, currentClass, okContainerCallees) ||
               paramUseDisqualifiesReadOnlyContainer(bin->right.get(), paramName, currentClass, okContainerCallees);
    }
    if (auto un = dynamic_cast<const UnaryExpression*>(expr)) {
        return paramUseDisqualifiesReadOnlyContainer(un->operand.get(), paramName, currentClass, okContainerCallees);
    }
    if (auto sub = dynamic_cast<const SubscriptExpression*>(expr)) {
        // A subscript read (`paramName[i]` as a value) never disqualifies; only a nested object and the index are descended into. A subscript target is handled by the caller.
        if (auto id = dynamic_cast<const Identifier*>(sub->object.get()); id != nullptr && id->name == paramName) {
            return paramUseDisqualifiesReadOnlyContainer(sub->index.get(), paramName, currentClass,
                                                          okContainerCallees);
        }
        return paramUseDisqualifiesReadOnlyContainer(sub->object.get(), paramName, currentClass,
                                                      okContainerCallees) ||
               paramUseDisqualifiesReadOnlyContainer(sub->index.get(), paramName, currentClass, okContainerCallees);
    }
    if (auto call = dynamic_cast<const FunctionCall*>(expr)) {
        // `paramName.method(...)` -- the receiver.
        if (call->callee) {
            if (auto member = dynamic_cast<const MemberAccess*>(call->callee.get())) {
                if (auto recvId = dynamic_cast<const Identifier*>(member->object.get());
                    recvId != nullptr && recvId->name == paramName) {
                    // `.size()` is a genuine read. Every other method on a container either mutates (push_back/pop_back/resize), or is begin()/end() (an iterator can outlive a
                    // reallocation), so it disqualifies. `reserve` is treated the same way, conservatively.
                    if (member->member == "size") return false;
                    return true;
                }
                // A nested receiver (`paramName.field.method()`) is not descended into: reading a field through paramName never disqualifies.
            }
        }
        for (size_t i = 0; i < call->arguments.size(); ++i) {
            auto argId = dynamic_cast<const Identifier*>(call->arguments[i].get());
            if (argId != nullptr && argId->name == paramName && !call->callee) {
                // Bare paramName passed to a function's parameter at this position is fine if that parameter is already read-only-container (fixed point, same keys as
                // classifyReadOnlyContainerParams) or an ext_fn `ref` parameter (ext_fn refParams are registered into the same free-function parameter list at parse
                // time, see ExternFunctionDeclaration in ast.h).
                const std::string methodKey =
                    currentClass + "::" + call->function_name + "::" + std::to_string(i);
                const std::string freeKey = call->function_name + "::" + std::to_string(i);
                if ((!currentClass.empty() && okContainerCallees.count(methodKey) > 0) ||
                    okContainerCallees.count(freeKey) > 0) {
                    continue;
                }
            }
            if (paramUseDisqualifiesReadOnlyContainer(call->arguments[i].get(), paramName, currentClass,
                                                       okContainerCallees)) {
                return true;
            }
        }
        return false;
    }
    // MemberAccess, NewExpression and literals never disqualify on their own.
    return false;
}

void SmartPointerAnalyzer::classifyReadOnlyContainerParams(const Program& program) {
    // Genuine-read evidence, as in classifyReadOnlyFreeFunctionParams (an unused parameter must not classify as read-only): a subscript read, `.size()`, or being the
    // container of a range-for. A bare paramName passed to an already read-only-container callee parameter counts as a read too.
    std::function<bool(const Expression*, const std::string&, const std::string&, const std::set<std::string>&)>
        exprHasGenuineReadContainer = [&](const Expression* expr, const std::string& paramName,
                                          const std::string& currentClass,
                                          const std::set<std::string>& okContainerCallees) -> bool {
        if (expr == nullptr) return false;
        if (auto sub = dynamic_cast<const SubscriptExpression*>(expr)) {
            if (auto id = dynamic_cast<const Identifier*>(sub->object.get()); id != nullptr &&
                id->name == paramName) {
                return true;
            }
            return exprHasGenuineReadContainer(sub->object.get(), paramName, currentClass, okContainerCallees) ||
                   exprHasGenuineReadContainer(sub->index.get(), paramName, currentClass, okContainerCallees);
        }
        if (auto call = dynamic_cast<const FunctionCall*>(expr)) {
            if (call->callee) {
                if (auto member = dynamic_cast<const MemberAccess*>(call->callee.get())) {
                    if (auto id = dynamic_cast<const Identifier*>(member->object.get());
                        id != nullptr && id->name == paramName && member->member == "size") {
                        return true;
                    }
                }
            }
            for (size_t i = 0; i < call->arguments.size(); ++i) {
                auto argId = dynamic_cast<const Identifier*>(call->arguments[i].get());
                if (argId != nullptr && argId->name == paramName && !call->callee) {
                    const std::string methodKey =
                        currentClass + "::" + call->function_name + "::" + std::to_string(i);
                    const std::string freeKey = call->function_name + "::" + std::to_string(i);
                    if ((!currentClass.empty() && okContainerCallees.count(methodKey) > 0) ||
                        okContainerCallees.count(freeKey) > 0) {
                        return true;
                    }
                }
                if (exprHasGenuineReadContainer(call->arguments[i].get(), paramName, currentClass,
                                                 okContainerCallees)) {
                    return true;
                }
            }
            return false;
        }
        if (auto bin = dynamic_cast<const BinaryExpression*>(expr)) {
            return exprHasGenuineReadContainer(bin->left.get(), paramName, currentClass, okContainerCallees) ||
                   exprHasGenuineReadContainer(bin->right.get(), paramName, currentClass, okContainerCallees);
        }
        if (auto un = dynamic_cast<const UnaryExpression*>(expr)) {
            return exprHasGenuineReadContainer(un->operand.get(), paramName, currentClass, okContainerCallees);
        }
        return false;
    };
    std::function<bool(const std::vector<StatementPtr>&, const std::string&, const std::string&,
                        const std::set<std::string>&)>
        bodyHasGenuineRead = [&](const std::vector<StatementPtr>& body, const std::string& paramName,
                                  const std::string& currentClass,
                                  const std::set<std::string>& okContainerCallees) -> bool {
        for (const auto& stmtPtr : body) {
            const Statement* stmt = stmtPtr.get();
            if (auto ret = dynamic_cast<const ReturnStatement*>(stmt)) {
                if (exprHasGenuineReadContainer(ret->expression.get(), paramName, currentClass, okContainerCallees)) {
                    return true;
                }
            } else if (auto decl = dynamic_cast<const VariableDeclaration*>(stmt)) {
                if (exprHasGenuineReadContainer(decl->expression.get(), paramName, currentClass,
                                                 okContainerCallees)) {
                    return true;
                }
            } else if (auto assign = dynamic_cast<const AssignmentStatement*>(stmt)) {
                if (exprHasGenuineReadContainer(assign->expression.get(), paramName, currentClass,
                                                 okContainerCallees)) {
                    return true;
                }
            } else if (auto exprStmt = dynamic_cast<const ExpressionStatement*>(stmt)) {
                if (exprHasGenuineReadContainer(exprStmt->expression.get(), paramName, currentClass,
                                                 okContainerCallees)) {
                    return true;
                }
            } else if (auto ifs = dynamic_cast<const IfStatement*>(stmt)) {
                if (exprHasGenuineReadContainer(ifs->condition.get(), paramName, currentClass, okContainerCallees)) {
                    return true;
                }
                if (bodyHasGenuineRead(ifs->body, paramName, currentClass, okContainerCallees)) return true;
                if (bodyHasGenuineRead(ifs->elseBody, paramName, currentClass, okContainerCallees)) return true;
            } else if (auto whiles = dynamic_cast<const WhileStatement*>(stmt)) {
                if (exprHasGenuineReadContainer(whiles->condition.get(), paramName, currentClass,
                                                 okContainerCallees)) {
                    return true;
                }
                if (bodyHasGenuineRead(whiles->body, paramName, currentClass, okContainerCallees)) return true;
            } else if (auto fors = dynamic_cast<const ForStatement*>(stmt)) {
                // The container of a range-for is the genuine read itself (`for (x : paramName)`), not a disqualification.
                if (auto id = dynamic_cast<const Identifier*>(fors->container.get());
                    id != nullptr && id->name == paramName) {
                    return true;
                }
                if (exprHasGenuineReadContainer(fors->container.get(), paramName, currentClass,
                                                 okContainerCallees)) {
                    return true;
                }
                if (bodyHasGenuineRead(fors->body, paramName, currentClass, okContainerCallees)) return true;
            } else if (auto throwStmt = dynamic_cast<const ThrowStatement*>(stmt)) {
                if (exprHasGenuineReadContainer(throwStmt->expression.get(), paramName, currentClass,
                                                 okContainerCallees)) {
                    return true;
                }
            } else if (auto trys = dynamic_cast<const TryStatement*>(stmt)) {
                if (bodyHasGenuineRead(trys->body, paramName, currentClass, okContainerCallees)) return true;
                if (bodyHasGenuineRead(trys->catchBody, paramName, currentClass, okContainerCallees)) return true;
            }
        }
        return false;
    };
    std::function<bool(const std::vector<StatementPtr>&, const std::string&, const std::string&,
                        const std::set<std::string>&)>
        bodyDisqualifiesContainer = [&](const std::vector<StatementPtr>& body, const std::string& paramName,
                                         const std::string& currentClass,
                                         const std::set<std::string>& okContainerCallees) -> bool {
        for (const auto& stmtPtr : body) {
            const Statement* stmt = stmtPtr.get();
            if (auto ret = dynamic_cast<const ReturnStatement*>(stmt)) {
                // Returning paramName itself hands the container out past the call, which disqualifies like any escape.
                if (paramUseDisqualifiesReadOnlyContainer(ret->expression.get(), paramName, currentClass,
                                                           okContainerCallees)) {
                    return true;
                }
            } else if (auto decl = dynamic_cast<const VariableDeclaration*>(stmt)) {
                // `Foo x = paramName;` copies or moves the whole container out, which disqualifies (`= paramName[i]` is a read and is fine).
                if (paramUseDisqualifiesReadOnlyContainer(decl->expression.get(), paramName, currentClass,
                                                           okContainerCallees)) {
                    return true;
                }
            } else if (auto assign = dynamic_cast<const AssignmentStatement*>(stmt)) {
                if (paramUseDisqualifiesReadOnlyContainer(assign->expression.get(), paramName, currentClass,
                                                           okContainerCallees)) {
                    return true;
                }
                if (assign->target != nullptr) {
                    // `paramName[i] = x;` is a write and disqualifies, unlike a subscript read.
                    if (auto sub = dynamic_cast<const SubscriptExpression*>(assign->target.get())) {
                        if (auto id = dynamic_cast<const Identifier*>(sub->object.get());
                            id != nullptr && id->name == paramName) {
                            return true;
                        }
                    }
                    if (paramUseDisqualifiesReadOnlyContainer(assign->target.get(), paramName, currentClass,
                                                               okContainerCallees)) {
                        return true;
                    }
                } else if (assign->variable == paramName) {
                    // Reassigning the parameter itself (`paramName = other;`) discards the caller's container and disqualifies.
                    return true;
                }
            } else if (auto exprStmt = dynamic_cast<const ExpressionStatement*>(stmt)) {
                if (paramUseDisqualifiesReadOnlyContainer(exprStmt->expression.get(), paramName, currentClass,
                                                           okContainerCallees)) {
                    return true;
                }
            } else if (auto ifs = dynamic_cast<const IfStatement*>(stmt)) {
                if (paramUseDisqualifiesReadOnlyContainer(ifs->condition.get(), paramName, currentClass,
                                                           okContainerCallees)) {
                    return true;
                }
                if (bodyDisqualifiesContainer(ifs->body, paramName, currentClass, okContainerCallees)) return true;
                if (bodyDisqualifiesContainer(ifs->elseBody, paramName, currentClass, okContainerCallees)) {
                    return true;
                }
            } else if (auto whiles = dynamic_cast<const WhileStatement*>(stmt)) {
                if (paramUseDisqualifiesReadOnlyContainer(whiles->condition.get(), paramName, currentClass,
                                                           okContainerCallees)) {
                    return true;
                }
                if (bodyDisqualifiesContainer(whiles->body, paramName, currentClass, okContainerCallees)) {
                    return true;
                }
            } else if (auto fors = dynamic_cast<const ForStatement*>(stmt)) {
                // The range-for's own container position is the genuine-read case; do not disqualify a bare paramName there.
                bool bareContainer = false;
                if (auto id = dynamic_cast<const Identifier*>(fors->container.get());
                    id != nullptr && id->name == paramName) {
                    bareContainer = true;
                }
                if (!bareContainer && paramUseDisqualifiesReadOnlyContainer(fors->container.get(), paramName,
                                                                            currentClass, okContainerCallees)) {
                    return true;
                }
                if (bodyDisqualifiesContainer(fors->body, paramName, currentClass, okContainerCallees)) return true;
            } else if (auto throwStmt = dynamic_cast<const ThrowStatement*>(stmt)) {
                if (paramUseDisqualifiesReadOnlyContainer(throwStmt->expression.get(), paramName, currentClass,
                                                           okContainerCallees)) {
                    return true;
                }
            } else if (auto trys = dynamic_cast<const TryStatement*>(stmt)) {
                if (bodyDisqualifiesContainer(trys->body, paramName, currentClass, okContainerCallees)) return true;
                if (bodyDisqualifiesContainer(trys->catchBody, paramName, currentClass, okContainerCallees)) {
                    return true;
                }
            }
        }
        return false;
    };

    // Seed with every ext_fn `ref` container parameter (hand-declared, so not part of the fixed point), under the same "fnName::argIndex" key as an ordinary free function.
    std::set<std::string> extFnRefCallees;
    for (const auto& stmt : program.statements) {
        auto extFn = dynamic_cast<const ExternFunctionDeclaration*>(stmt.get());
        if (extFn == nullptr) continue;
        for (size_t idx : extFn->refParams) {
            if (idx < extFn->parameters.size() && isVectorType(extFn->parameters[idx].first)) {
                extFnRefCallees.insert(extFn->name + "::" + std::to_string(idx));
            }
        }
    }

    // Fixed point, same driver shape as classifyReadOnlyFreeFunctionParams.
    std::set<std::string> classified;         // "fnName::paramName"
    std::set<std::string> classifiedMethods;  // "ClassName::methodName::paramName"
    bool changed = true;
    while (changed) {
        changed = false;
        std::set<std::string> okContainerCallees = extFnRefCallees;
        for (const auto& stmt : program.statements) {
            if (auto fn2 = dynamic_cast<const FunctionDeclaration*>(stmt.get())) {
                for (size_t i = 0; i < fn2->parameters.size(); ++i) {
                    if (classified.count(fn2->name + "::" + fn2->parameters[i].second) > 0) {
                        okContainerCallees.insert(fn2->name + "::" + std::to_string(i));
                    }
                }
            } else if (auto cls2 = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
                for (const auto& member : cls2->members) {
                    auto method2 = dynamic_cast<const MethodDeclaration*>(member.get());
                    if (method2 == nullptr) continue;
                    for (size_t i = 0; i < method2->parameters.size(); ++i) {
                        const std::string key =
                            cls2->name + "::" + method2->name + "::" + method2->parameters[i].second;
                        if (classifiedMethods.count(key) > 0) {
                            okContainerCallees.insert(cls2->name + "::" + method2->name + "::" +
                                                       std::to_string(i));
                        }
                    }
                }
            }
        }

        for (const auto& stmt : program.statements) {
            if (auto fn = dynamic_cast<const FunctionDeclaration*>(stmt.get())) {
                for (const auto& [ptype, pname] : fn->parameters) {
                    const std::string classifiedKey = fn->name + "::" + pname;
                    if (classified.count(classifiedKey) > 0) continue;
                    // v1 scope: std::vector<primitive> only (as ext_fn `ref`).
                    if (!isVectorType(ptype) || !isPrimitiveType(vectorElementType(ptype))) continue;
                    if (!bodyDisqualifiesContainer(fn->body, pname, "", okContainerCallees) &&
                        bodyHasGenuineRead(fn->body, pname, "", okContainerCallees)) {
                        classified.insert(classifiedKey);
                        changed = true;
                    }
                }
            } else if (auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
                for (const auto& member : cls->members) {
                    auto method = dynamic_cast<const MethodDeclaration*>(member.get());
                    if (method == nullptr || !method->specializationArg.empty()) continue;
                    for (const auto& [ptype, pname] : method->parameters) {
                        const std::string classifiedKey = cls->name + "::" + method->name + "::" + pname;
                        if (classifiedMethods.count(classifiedKey) > 0) continue;
                        if (!isVectorType(ptype) || !isPrimitiveType(vectorElementType(ptype))) continue;
                        if (!bodyDisqualifiesContainer(method->body, pname, cls->name, okContainerCallees) &&
                            bodyHasGenuineRead(method->body, pname, cls->name, okContainerCallees)) {
                            classifiedMethods.insert(classifiedKey);
                            changed = true;
                        }
                    }
                }
            }
        }
    }

    for (const auto& key : classified) {
        readOnlyContainerParams_.insert(key);
    }
    for (const auto& key : classifiedMethods) {
        readOnlyMethodContainerParams_.insert(key);
    }
    // Also register under the qualified name, as in classifyReadOnlyFreeFunctionParams.
    for (const auto& stmt : program.statements) {
        auto fn = dynamic_cast<const FunctionDeclaration*>(stmt.get());
        if (fn == nullptr || fn->namespaceName.empty()) continue;
        for (const auto& [ptype, pname] : fn->parameters) {
            if (classified.count(fn->name + "::" + pname) > 0) {
                readOnlyContainerParams_.insert(fn->namespaceName + "::" + fn->name + "::" + pname);
            }
        }
    }
}

bool SmartPointerAnalyzer::exprReferencesName(const Expression* expr, const std::string& name) const {
    if (expr == nullptr) return false;
    if (auto id = dynamic_cast<const Identifier*>(expr)) {
        return id->name == name;
    }
    if (auto bin = dynamic_cast<const BinaryExpression*>(expr)) {
        return exprReferencesName(bin->left.get(), name) || exprReferencesName(bin->right.get(), name);
    }
    if (auto un = dynamic_cast<const UnaryExpression*>(expr)) {
        return exprReferencesName(un->operand.get(), name);
    }
    if (auto sub = dynamic_cast<const SubscriptExpression*>(expr)) {
        return exprReferencesName(sub->object.get(), name) || exprReferencesName(sub->index.get(), name);
    }
    if (auto mem = dynamic_cast<const MemberAccess*>(expr)) {
        return exprReferencesName(mem->object.get(), name);
    }
    if (auto call = dynamic_cast<const FunctionCall*>(expr)) {
        if (exprReferencesName(call->callee.get(), name)) return true;
        for (const auto& arg : call->arguments) {
            if (exprReferencesName(arg.get(), name)) return true;
        }
        return false;
    }
    if (auto neww = dynamic_cast<const NewExpression*>(expr)) {
        for (const auto& arg : neww->arguments) {
            if (exprReferencesName(arg.get(), name)) return true;
        }
        return false;
    }
    if (dynamic_cast<const ClosureExpression*>(expr) != nullptr) {
        // Any closure after the candidate assignment already disqualifies the function (bodyIsLastUseFieldAssignment gives up on closures), so the capture list is never inspected.
        return true;
    }
    // Number/StringLiteral/CharLiteral/ThisExpression/ScopedIdentifier cannot reference a bare local or parameter name.
    return false;
}

// See the header comment for the scope (straight-line bodies only, exactly one candidate assignment, no closures).
bool SmartPointerAnalyzer::bodyIsLastUseFieldAssignment(const std::vector<StatementPtr>& body,
                                                          const std::string& paramName) const {
    int candidateIndex = -1;
    for (size_t i = 0; i < body.size(); ++i) {
        const Statement* stmt = body[i].get();
        if (dynamic_cast<const IfStatement*>(stmt) != nullptr ||
            dynamic_cast<const WhileStatement*>(stmt) != nullptr ||
            dynamic_cast<const ForStatement*>(stmt) != nullptr ||
            dynamic_cast<const TryStatement*>(stmt) != nullptr) {
            return false;
        }
        if (auto assign = dynamic_cast<const AssignmentStatement*>(stmt)) {
            // The candidate: a bare-name target (whether it is really a field is the emitter's job) other than paramName itself, with the RHS exactly the bare identifier
            // `paramName`. Any other shape still goes through the containment check below, since it may reference paramName without being the assignment we may move at.
            const bool targetIsBareNameOtherThanParam =
                assign->target == nullptr && assign->variable != paramName;
            auto rhsId = dynamic_cast<const Identifier*>(assign->expression.get());
            if (targetIsBareNameOtherThanParam && rhsId != nullptr && rhsId->name == paramName) {
                if (candidateIndex != -1) return false;  // more than one -- ambiguous, give up
                candidateIndex = static_cast<int>(i);
                continue;  // the assignment's own RHS use doesn't disqualify itself
            }
            // Not the candidate shape: still check target and RHS for an ordinary reference (`x = paramName + 1;` is a use that disqualifies a later candidate).
            if (exprReferencesName(assign->target.get(), paramName) ||
                exprReferencesName(assign->expression.get(), paramName)) {
                if (candidateIndex != -1) return false;  // used again after the candidate
            }
            continue;
        }
        if (auto decl = dynamic_cast<const VariableDeclaration*>(stmt)) {
            if (exprReferencesName(decl->expression.get(), paramName) && candidateIndex != -1) return false;
            continue;
        }
        if (auto exprStmt = dynamic_cast<const ExpressionStatement*>(stmt)) {
            if (exprReferencesName(exprStmt->expression.get(), paramName) && candidateIndex != -1) return false;
            continue;
        }
        if (auto ret = dynamic_cast<const ReturnStatement*>(stmt)) {
            if (exprReferencesName(ret->expression.get(), paramName) && candidateIndex != -1) return false;
            continue;
        }
        if (auto th = dynamic_cast<const ThrowStatement*>(stmt)) {
            if (exprReferencesName(th->expression.get(), paramName) && candidateIndex != -1) return false;
            continue;
        }
        // Any other statement kind is treated as "give up", like the if/while/for/try check above.
        return false;
    }
    return candidateIndex != -1;
}

void SmartPointerAnalyzer::classifyLastUseFieldAssignedParams(const Program& program) {
    for (const auto& stmt : program.statements) {
        if (auto fn = dynamic_cast<const FunctionDeclaration*>(stmt.get())) {
            for (const auto& [ptype, pname] : fn->parameters) {
                if (!isVectorType(ptype) || !isPrimitiveType(vectorElementType(ptype))) continue;
                if (bodyIsLastUseFieldAssignment(fn->body, pname)) {
                    lastUseFieldAssignedParams_.insert(fn->name + "::" + pname);
                }
            }
        } else if (auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
            for (const auto& member : cls->members) {
                auto method = dynamic_cast<const MethodDeclaration*>(member.get());
                if (method == nullptr || !method->specializationArg.empty()) continue;
                for (const auto& [ptype, pname] : method->parameters) {
                    if (!isVectorType(ptype) || !isPrimitiveType(vectorElementType(ptype))) continue;
                    if (bodyIsLastUseFieldAssignment(method->body, pname)) {
                        lastUseMethodFieldAssignedParams_.insert(cls->name + "::" + method->name + "::" + pname);
                    }
                }
            }
        }
    }
}

bool SmartPointerAnalyzer::isLastUseFieldAssignedParam(const std::string& fnName,
                                                         const std::string& paramName) const {
    return lastUseFieldAssignedParams_.count(fnName + "::" + paramName) > 0;
}

bool SmartPointerAnalyzer::isLastUseMethodFieldAssignedParam(const std::string& className,
                                                               const std::string& methodName,
                                                               const std::string& paramName) const {
    return lastUseMethodFieldAssignedParams_.count(className + "::" + methodName + "::" + paramName) > 0;
}

bool SmartPointerAnalyzer::bodyHasImpurity(const std::vector<StatementPtr>& body, const std::string& currentClass,
                                            std::set<std::string> localNames,
                                            const std::set<std::string>& pureSoFar,
                                            const std::set<std::string>& primitiveFieldNames) const {
    // Is this a local declared in the body? Mutating a body-local is fine: nothing can hand out a reference to a local.
    auto isLocal = [&](const std::string& name) { return localNames.count(name) > 0; };

    std::function<bool(const Expression*)> callIsImpure = [&](const Expression* expr) -> bool {
        auto call = dynamic_cast<const FunctionCall*>(expr);
        if (call == nullptr) return false;
        for (const auto& a : call->arguments) {
            if (callIsImpure(a.get())) return true;
        }
        if (call->callee) {
            auto member = dynamic_cast<const MemberAccess*>(call->callee.get());
            if (member != nullptr) {
                auto recvId = dynamic_cast<const Identifier*>(member->object.get());
                // A mutating container method on anything but a body-local container is an impurity: the caller can still see that container.
                if (vectorMethodInvalidatesReferences(member->member)) {
                    if (recvId == nullptr || !isLocal(recvId->name)) return true;
                    return false;
                }
                // A call on a non-local receiver (`obj.method()`, `field.method()`) is impure unless the receiver's class is known and that method is already heap-pure. A local
                // bare receiver needs no more checking.
                if (recvId != nullptr && isLocal(recvId->name)) return false;
                // A this-implicit, field or chained receiver cannot be resolved in this structural pass, so it is impure. A same-class implicit-this call is handled below.
                return true;
            }
            return true;  // unrecognized callee shape: conservatively impure
        }
        // Implicit-this method call or free-function call (callee is null either way): pure only if already classified pure this round; method-scoped key first.
        const std::string methodKey = currentClass + "::" + call->function_name;
        const std::string freeKey = call->function_name;
        if ((!currentClass.empty() && pureSoFar.count(methodKey) > 0) || pureSoFar.count(freeKey) > 0) {
            return false;
        }
        return true;
    };

    for (const auto& stmtPtr : body) {
        const Statement* stmt = stmtPtr.get();
        if (auto ret = dynamic_cast<const ReturnStatement*>(stmt)) {
            if (callIsImpure(ret->expression.get())) return true;
        } else if (auto decl = dynamic_cast<const VariableDeclaration*>(stmt)) {
            if (callIsImpure(decl->expression.get())) return true;
            localNames.insert(decl->identifier);
        } else if (auto assign = dynamic_cast<const AssignmentStatement*>(stmt)) {
            if (callIsImpure(assign->expression.get())) return true;
            if (assign->target != nullptr) {
                // `x.field = ...` / `x[i] = ...` is impure unless `x` is a body-local.
                const Expression* root = assign->target.get();
                std::string rootName;
                if (auto mem = dynamic_cast<const MemberAccess*>(root)) {
                    if (auto id = dynamic_cast<const Identifier*>(mem->object.get())) rootName = id->name;
                } else if (auto sub = dynamic_cast<const SubscriptExpression*>(root)) {
                    if (auto id = dynamic_cast<const Identifier*>(sub->object.get())) rootName = id->name;
                }
                if (rootName.empty() || !isLocal(rootName)) {
                    // `this->field = ...` has no bare-identifier root, so it is impure (assigning through `this` is never a body-local).
                    return true;
                }
            } else if (!isLocal(assign->variable) && primitiveFieldNames.count(assign->variable) == 0) {
                // Bare `name = ...` for something not declared in this body and not a primitive field (a parameter, or a non-primitive field: field writes are bare names)
                // is impure; this keeps the answer conservative.
                return true;
            }
        } else if (auto exprStmt = dynamic_cast<const ExpressionStatement*>(stmt)) {
            if (callIsImpure(exprStmt->expression.get())) return true;
        } else if (auto ifs = dynamic_cast<const IfStatement*>(stmt)) {
            if (callIsImpure(ifs->condition.get())) return true;
            if (bodyHasImpurity(ifs->body, currentClass, localNames, pureSoFar, primitiveFieldNames)) return true;
            if (bodyHasImpurity(ifs->elseBody, currentClass, localNames, pureSoFar, primitiveFieldNames)) return true;
        } else if (auto whiles = dynamic_cast<const WhileStatement*>(stmt)) {
            if (callIsImpure(whiles->condition.get())) return true;
            if (bodyHasImpurity(whiles->body, currentClass, localNames, pureSoFar, primitiveFieldNames)) return true;
        } else if (auto fors = dynamic_cast<const ForStatement*>(stmt)) {
            if (callIsImpure(fors->container.get())) return true;
            std::set<std::string> innerLocals = localNames;
            innerLocals.insert(fors->loopVarName);
            if (bodyHasImpurity(fors->body, currentClass, innerLocals, pureSoFar, primitiveFieldNames)) return true;
        } else if (auto throwStmt = dynamic_cast<const ThrowStatement*>(stmt)) {
            if (callIsImpure(throwStmt->expression.get())) return true;
        } else if (auto trys = dynamic_cast<const TryStatement*>(stmt)) {
            if (bodyHasImpurity(trys->body, currentClass, localNames, pureSoFar, primitiveFieldNames)) return true;
            if (bodyHasImpurity(trys->catchBody, currentClass, localNames, pureSoFar, primitiveFieldNames)) return true;
        }
    }
    return false;
}

void SmartPointerAnalyzer::classifyHeapPureFunctions(const Program& program) {
    // Opposite direction from the read-only fixed points: start with every function and method pure, and remove any whose own body has an impurity (checked against
    // the current set, so calling an impure one removes it too) until nothing changes. Each round must remove one, so it terminates.
    std::set<std::string> pureFree;
    std::set<std::string> pureMethods;  // "ClassName::methodName"
    // Per-class primitive field names (a field write is a bare name; see bodyHasImpurity).
    std::map<std::string, std::set<std::string>> primitiveFieldsByClass;
    for (const auto& stmt : program.statements) {
        if (auto fn = dynamic_cast<const FunctionDeclaration*>(stmt.get())) {
            pureFree.insert(fn->name);
        } else if (auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
            auto& primitiveFields = primitiveFieldsByClass[cls->name];
            for (const auto& member : cls->members) {
                if (auto method = dynamic_cast<const MethodDeclaration*>(member.get())) {
                    pureMethods.insert(cls->name + "::" + method->name);
                } else if (auto field = dynamic_cast<const VariableDeclaration*>(member.get())) {
                    if (isPrimitiveType(field->type)) primitiveFields.insert(field->identifier);
                }
            }
        }
    }

    bool changed = true;
    while (changed) {
        changed = false;
        std::set<std::string> pureSoFar = pureFree;
        for (const auto& m : pureMethods) pureSoFar.insert(m);

        for (const auto& stmt : program.statements) {
            if (auto fn = dynamic_cast<const FunctionDeclaration*>(stmt.get())) {
                if (pureFree.count(fn->name) == 0) continue;  // already removed
                std::set<std::string> localNames;
                for (const auto& [ptype, pname] : fn->parameters) localNames.insert(pname);
                if (bodyHasImpurity(fn->body, "", localNames, pureSoFar, {})) {
                    pureFree.erase(fn->name);
                    changed = true;
                }
            } else if (auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
                for (const auto& member : cls->members) {
                    auto method = dynamic_cast<const MethodDeclaration*>(member.get());
                    if (method == nullptr) continue;
                    const std::string key = cls->name + "::" + method->name;
                    if (pureMethods.count(key) == 0) continue;
                    std::set<std::string> localNames;
                    for (const auto& [ptype, pname] : method->parameters) localNames.insert(pname);
                    const auto& primitiveFields = primitiveFieldsByClass[cls->name];
                    if (bodyHasImpurity(method->body, cls->name, localNames, pureSoFar, primitiveFields)) {
                        pureMethods.erase(key);
                        changed = true;
                    }
                }
            }
        }
    }

    heapPureFreeFunctions_ = std::move(pureFree);
    // heapPureMethods_ uses the same combined "Class::method" keys as isHeapPureMethod, so store verbatim.
    heapPureMethods_ = std::move(pureMethods);
}


// ---- read-only string parameters --------------------------------------------------------------------
//
// A `string` parameter is read-only when the body never mutates it: no `p.append/push_back/clear/reserve(...)`, no `p = ...`, no `p[i] = ...`, and it is not
// handed to an ext_fn `ref string`. Copying it onward is a read. Such a parameter is lowered to `const std::string&`.
//
// The hazard is aliasing: for a field argument the callee might mutate that very field while holding the reference. So the emitter passes an explicit copy
// for a field argument unless the callee provably mutates no field of that name. This pass computes, per function and method, the field names it
// transitively mutates (name-based and conservative; "*" means it may mutate anything, e.g. an unresolved call).
namespace {
bool isStringMutatorMethod(const std::string& m) {
    return m == "append" || m == "push_back" || m == "clear" || m == "reserve" || m == "resize" ||
           m == "insert" || m == "erase" || m == "pop_back" || m == "assign" || m == "replace" ||
           m == "copyWithin";
}
}  // namespace

void SmartPointerAnalyzer::classifyReadOnlyStringParams(const Program& program) {
    struct Fn {
        std::string key;                  // "name" for a free function, "Class::name" for a method
        std::string bareName;
        std::string className;            // "" for a free function
        const std::vector<std::pair<std::string, std::string>>* params = nullptr;
        const std::vector<StatementPtr>* body = nullptr;
        bool skipParams = false;          // virtual / override / in an inheritance hierarchy
    };
    std::vector<Fn> fns;
    std::set<std::string> freeNames;
    std::map<std::string, std::set<std::string>> methodsByName;  // bare method name -> class keys
    std::set<std::string> classesInHierarchy;
    for (const auto& stmt : program.statements) {
        if (auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
            if (!cls->baseClass.empty()) {
                classesInHierarchy.insert(cls->name);
                classesInHierarchy.insert(cls->baseClass);
            }
        }
    }
    std::map<std::string, std::set<std::string>> fieldNamesByClass;
    objectFieldNames_.clear();
    for (const auto& stmt : program.statements) {
        if (auto fn = dynamic_cast<const FunctionDeclaration*>(stmt.get())) {
            Fn f;
            f.key = fn->name;
            f.bareName = fn->name;
            f.params = &fn->parameters;
            f.body = &fn->body;
            fns.push_back(f);
            freeNames.insert(fn->name);
        } else if (auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
            for (const auto& member : cls->members) {
                if (auto field = dynamic_cast<const VariableDeclaration*>(member.get())) {
                    fieldNamesByClass[cls->name].insert(field->identifier);
                    // a field that owns objects (a class handle or a container): writing it can free them
                    if (!isPrimitiveType(field->type)) objectFieldNames_.insert(field->identifier);
                } else if (auto method = dynamic_cast<const MethodDeclaration*>(member.get())) {
                    if (!method->specializationArg.empty() || !method->typeParameter.empty()) continue;
                    Fn f;
                    f.key = cls->name + "::" + method->name;
                    f.bareName = method->name;
                    f.className = cls->name;
                    f.params = &method->parameters;
                    f.body = &method->body;
                    f.skipParams = method->isVirtual || method->isOverride ||
                                   classesInHierarchy.count(cls->name) > 0;
                    fns.push_back(f);
                    methodsByName[method->name].insert(f.key);
                } else if (auto ctor = dynamic_cast<const ConstructorDeclaration*>(member.get())) {
                    Fn f;
                    f.key = cls->name + "::" + ctor->name;
                    f.bareName = ctor->name;
                    f.className = cls->name;
                    f.params = &ctor->parameters;
                    f.body = &ctor->body;
                    f.skipParams = true;  // constructors keep by-value string parameters
                    fns.push_back(f);
                    methodsByName["<ctor>" + cls->name].insert(f.key);
                }
            }
        }
    }
    // ext_fn `ref string` parameters are mutable references
    std::map<std::string, std::set<size_t>> extFnMutableStringArgs;
    for (const auto& stmt : program.statements) {
        auto extFn = dynamic_cast<const ExternFunctionDeclaration*>(stmt.get());
        if (extFn == nullptr) continue;
        for (size_t idx : extFn->refParams) {
            if (idx < extFn->parameters.size() && isStringType(extFn->parameters[idx].first)) {
                extFnMutableStringArgs[extFn->name].insert(idx);
            }
        }
    }

    struct Facts {
        std::set<std::string> mutatedParams;
        std::set<std::string> mutatedFields;  // may contain "*"
        std::set<std::string> calls;          // keys of user functions/methods (transitive closure below)
    };
    std::map<std::string, Facts> facts;

    // one walk per function
    for (const auto& f : fns) {
        Facts& fa = facts[f.key];
        std::set<std::string> locals;
        for (const auto& [ptype, pname] : *f.params) locals.insert(pname);
        std::function<void(const std::vector<StatementPtr>&)> collectLocals =
            [&](const std::vector<StatementPtr>& stmts) {
                for (const auto& sp : stmts) {
                    if (auto d = dynamic_cast<const VariableDeclaration*>(sp.get())) locals.insert(d->identifier);
                    else if (auto c = dynamic_cast<const ContainerDeclaration*>(sp.get())) locals.insert(c->identifier);
                    else if (auto i = dynamic_cast<const IfStatement*>(sp.get())) { collectLocals(i->body); collectLocals(i->elseBody); }
                    else if (auto w = dynamic_cast<const WhileStatement*>(sp.get())) collectLocals(w->body);
                    else if (auto fo = dynamic_cast<const ForStatement*>(sp.get())) { locals.insert(fo->loopVarName); collectLocals(fo->body); }
                    else if (auto t = dynamic_cast<const TryStatement*>(sp.get())) { collectLocals(t->body); collectLocals(t->catchBody); }
                }
            };
        collectLocals(*f.body);

        // name of the field a mutation lands on, or "" when the receiver is a local
        auto fieldOfReceiver = [&](const Expression* recv) -> std::string {
            if (auto id = dynamic_cast<const Identifier*>(recv)) {
                return locals.count(id->name) > 0 ? std::string() : id->name;
            }
            if (auto m = dynamic_cast<const MemberAccess*>(recv)) return m->member;
            if (auto sub = dynamic_cast<const SubscriptExpression*>(recv)) {
                if (auto id = dynamic_cast<const Identifier*>(sub->object.get())) {
                    return locals.count(id->name) > 0 ? std::string() : id->name;
                }
                if (auto m = dynamic_cast<const MemberAccess*>(sub->object.get())) return m->member;
            }
            return std::string();
        };
        auto noteMutation = [&](const Expression* recv) {
            if (auto id = dynamic_cast<const Identifier*>(recv)) {
                if (locals.count(id->name) > 0) { fa.mutatedParams.insert(id->name); return; }
            }
            if (auto sub = dynamic_cast<const SubscriptExpression*>(recv)) {
                if (auto id = dynamic_cast<const Identifier*>(sub->object.get()); id != nullptr && locals.count(id->name) > 0) {
                    fa.mutatedParams.insert(id->name);
                    return;
                }
            }
            const std::string field = fieldOfReceiver(recv);
            if (!field.empty()) fa.mutatedFields.insert(field);
            else fa.mutatedFields.insert("*");
        };

        std::function<void(const Expression*)> visitExpr = [&](const Expression* e) {
            if (e == nullptr) return;
            if (auto b = dynamic_cast<const BinaryExpression*>(e)) { visitExpr(b->left.get()); visitExpr(b->right.get()); return; }
            if (auto u = dynamic_cast<const UnaryExpression*>(e)) { visitExpr(u->operand.get()); return; }
            if (auto m = dynamic_cast<const MemberAccess*>(e)) { visitExpr(m->object.get()); return; }
            if (auto s = dynamic_cast<const SubscriptExpression*>(e)) { visitExpr(s->object.get()); visitExpr(s->index.get()); return; }
            if (auto n = dynamic_cast<const NewExpression*>(e)) { for (const auto& a : n->arguments) visitExpr(a.get()); return; }
            if (auto call = dynamic_cast<const FunctionCall*>(e)) {
                for (const auto& a : call->arguments) visitExpr(a.get());
                if (call->callee) {
                    visitExpr(call->callee.get());
                    if (auto m = dynamic_cast<const MemberAccess*>(call->callee.get())) {
                        if (isStringMutatorMethod(m->member)) noteMutation(m->object.get());
                        // a call to a user method of that name (receiver class not resolved here: every class that has a method of this name is a candidate)
                        auto it = methodsByName.find(m->member);
                        if (it != methodsByName.end()) {
                            for (const auto& k : it->second) fa.calls.insert(k);
                        }
                    }
                } else {
                    const std::string& name = call->function_name;
                    if (freeNames.count(name) > 0) fa.calls.insert(name);
                    // implicit-this call to a method of the current class
                    if (!f.className.empty()) {
                        auto it = methodsByName.find(name);
                        if (it != methodsByName.end()) for (const auto& k : it->second) fa.calls.insert(k);
                    }
                    // a constructor-style or unknown name resolves to nothing we can see: no effect on fields
                    if (auto ext = extFnMutableStringArgs.find(name); ext != extFnMutableStringArgs.end()) {
                        for (size_t idx : ext->second) {
                            if (idx < call->arguments.size()) noteMutation(call->arguments[idx].get());
                        }
                    }
                }
                return;
            }
        };
        std::function<void(const std::vector<StatementPtr>&)> visitStmts = [&](const std::vector<StatementPtr>& stmts) {
            for (const auto& sp : stmts) {
                const Statement* st = sp.get();
                if (auto d = dynamic_cast<const VariableDeclaration*>(st)) visitExpr(d->expression.get());
                else if (auto a = dynamic_cast<const AssignmentStatement*>(st)) {
                    visitExpr(a->expression.get());
                    if (a->target) {
                        visitExpr(a->target.get());
                        noteMutation(a->target.get());
                    } else if (locals.count(a->variable) > 0) {
                        fa.mutatedParams.insert(a->variable);
                        // reassigning an object-typed parameter drops the handle the caller passed in, which can free the object a field argument lives in
                        for (const auto& [ptype2, pname2] : *f.params) {
                            if (pname2 == a->variable && !isPrimitiveType(ptype2)) fa.mutatedFields.insert("*");
                        }
                    } else {
                        fa.mutatedFields.insert(a->variable);
                    }
                }
                else if (auto es = dynamic_cast<const ExpressionStatement*>(st)) visitExpr(es->expression.get());
                else if (auto r = dynamic_cast<const ReturnStatement*>(st)) visitExpr(r->expression.get());
                else if (auto th = dynamic_cast<const ThrowStatement*>(st)) visitExpr(th->expression.get());
                else if (auto i = dynamic_cast<const IfStatement*>(st)) { visitExpr(i->condition.get()); visitStmts(i->body); visitStmts(i->elseBody); }
                else if (auto w = dynamic_cast<const WhileStatement*>(st)) { visitExpr(w->condition.get()); visitStmts(w->body); }
                else if (auto fo = dynamic_cast<const ForStatement*>(st)) { visitExpr(fo->container.get()); visitStmts(fo->body); }
                else if (auto t = dynamic_cast<const TryStatement*>(st)) { visitStmts(t->body); visitStmts(t->catchBody); }
            }
        };
        visitStmts(*f.body);
    }

    // transitive closure of the mutated-field sets over the call graph
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& [key, fa] : facts) {
            for (const auto& callee : std::set<std::string>(fa.calls)) {
                auto it = facts.find(callee);
                if (it == facts.end()) continue;
                for (const auto& fld : it->second.mutatedFields) {
                    if (fa.mutatedFields.insert(fld).second) changed = true;
                }
            }
        }
    }

    readOnlyStringParams_.clear();
    readOnlyStringPositions_.clear();
    stringMutatedFieldsByFunction_.clear();
    for (const auto& f : fns) {
        const Facts& fa = facts[f.key];
        // keyed by the bare callee name (free function or method): the emitter does not always know the receiver's class, so a call site unions every function/method of that name
        auto& set = stringMutatedFieldsByFunction_[f.bareName];
        set.insert(fa.mutatedFields.begin(), fa.mutatedFields.end());
        if (f.skipParams) continue;
        for (const auto& [ptype, pname] : *f.params) {
            if (!isStringType(ptype)) continue;
            if (fa.mutatedParams.count(pname) > 0) continue;
            readOnlyStringParams_.insert(f.key + "::" + pname);
            for (size_t pi = 0; pi < f.params->size(); ++pi) {
                if ((*f.params)[pi].second == pname) readOnlyStringPositions_.insert(f.bareName + "#" + std::to_string(pi));
            }
        }
    }
}

bool SmartPointerAnalyzer::isReadOnlyStringParam(const std::string& className, const std::string& functionName,
                                                 const std::string& paramName) const {
    const std::string key = className.empty() ? functionName : className + "::" + functionName;
    return readOnlyStringParams_.count(key + "::" + paramName) > 0;
}

bool SmartPointerAnalyzer::anyReadOnlyStringParamAt(const std::string& bareName, size_t index) const {
    return readOnlyStringPositions_.count(bareName + "#" + std::to_string(index)) > 0;
}

bool SmartPointerAnalyzer::calleeMayMutateObjects(const std::string& bareName) const {
    auto it = stringMutatedFieldsByFunction_.find(bareName);
    if (it == stringMutatedFieldsByFunction_.end()) return true;
    if (it->second.count("*") > 0) return true;
    for (const auto& fld : it->second) {
        if (objectFieldNames_.count(fld) > 0) return true;
    }
    return false;
}

bool SmartPointerAnalyzer::calleeMayMutateField(const std::string& bareName, const std::string& fieldName) const {
    auto it = stringMutatedFieldsByFunction_.find(bareName);
    if (it == stringMutatedFieldsByFunction_.end()) return true;  // unknown callee: assume it may
    return it->second.count("*") > 0 || it->second.count(fieldName) > 0;
}

bool SmartPointerAnalyzer::isReadOnlyFreeFunctionParam(const std::string& fnName,
                                                        const std::string& paramName) const {
    return readOnlyFreeFunctionParams_.count(fnName + "::" + paramName) > 0;
}

bool SmartPointerAnalyzer::isReadOnlyMethodParam(const std::string& className, const std::string& methodName,
                                                  const std::string& paramName) const {
    return readOnlyMethodParams_.count(className + "::" + methodName + "::" + paramName) > 0;
}

bool SmartPointerAnalyzer::isReadOnlyContainerParam(const std::string& fnName,
                                                     const std::string& paramName) const {
    return readOnlyContainerParams_.count(fnName + "::" + paramName) > 0;
}

bool SmartPointerAnalyzer::isReadOnlyMethodContainerParam(const std::string& className,
                                                           const std::string& methodName,
                                                           const std::string& paramName) const {
    return readOnlyMethodContainerParams_.count(className + "::" + methodName + "::" + paramName) > 0;
}

bool SmartPointerAnalyzer::isHeapPureFreeFunction(const std::string& fnName) const {
    return heapPureFreeFunctions_.count(fnName) > 0;
}

bool SmartPointerAnalyzer::isHeapPureMethod(const std::string& className, const std::string& methodName) const {
    return heapPureMethods_.count(className + "::" + methodName) > 0;
}

} // namespace cling
