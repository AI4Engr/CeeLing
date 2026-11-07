// SmartPointerAnalyzer: promotion of parameters and call arguments to shared ownership
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

void SmartPointerAnalyzer::forceVirtualSignaturesShared(const Program& program) {
    for (const auto& stmt : program.statements) {
        auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get());
        if (cls == nullptr) continue;
        for (const auto& member : cls->members) {
            auto method = dynamic_cast<const MethodDeclaration*>(member.get());
            if (method == nullptr || !(method->isVirtual || method->isOverride)) continue;
            const std::string scope = cls->name + "::" + method->name;
            for (const auto& [ptype, pname] : method->parameters) {
                if (classNames.count(ptype) == 0) continue;
                const std::string key = buildKey(scope, ptype, pname);
                if (variableReferences.find(key) == variableReferences.end()) continue;
                pointerDecisions[key] = PointerType::SHARED;
                pointerReasons[key] = "parameter of a virtual / override method (signatures must agree across the hierarchy)";
            }
            if (classNames.count(method->returnType) > 0) {
                methodReturnKinds_[makeMemberKey(cls->name, method->name)] = PointerType::SHARED;
            }
        }
    }
}

void SmartPointerAnalyzer::promoteAliasedLocals(const Program& program) {
    for (const auto& stmt : program.statements) {
        if (auto fn = dynamic_cast<const FunctionDeclaration*>(stmt.get())) {
            promoteAliasedLocalsInBody(fn->body, fn->parameters, fn->name);
        } else if (auto mn = dynamic_cast<const MainFunction*>(stmt.get())) {
            promoteAliasedLocalsInBody(mn->body, {}, "main");
        } else if (auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
            for (const auto& member : cls->members) {
                if (auto method = dynamic_cast<const MethodDeclaration*>(member.get())) {
                    promoteAliasedLocalsInBody(method->body, method->parameters,
                                               cls->name + "::" + method->name);
                } else if (auto ctor = dynamic_cast<const ConstructorDeclaration*>(member.get())) {
                    promoteAliasedLocalsInBody(ctor->body, ctor->parameters, cls->name + "::" + ctor->name);
                }
            }
        }
    }
}

void SmartPointerAnalyzer::promoteParamsStoredInSharedFields(const Program& program) {
    for (const auto& stmt : program.statements) {
        if (auto fn = dynamic_cast<const FunctionDeclaration*>(stmt.get())) {
            promoteParamsInBody(fn->body, "", fn->parameters, fn->name);
            continue;
        }
        if (auto mn = dynamic_cast<const MainFunction*>(stmt.get())) {
            promoteParamsInBody(mn->body, "", {}, "main");
            continue;
        }
        auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get());
        if (cls == nullptr) continue;
        // A spawn job's constructor moves its object captures into unique fields; the usual "parameter stored into a field is linked" rule would leave the creating
        // thread an owner of an object that now runs concurrently. DanglingChecker::checkSpawnCall decides what may be moved.
        if (cls->isSpawnJob) continue;
        for (const auto& member : cls->members) {
            if (auto method = dynamic_cast<const MethodDeclaration*>(member.get())) {
                promoteParamsInBody(method->body, cls->name, method->parameters, cls->name + "::" + method->name);
            } else if (auto ctor = dynamic_cast<const ConstructorDeclaration*>(member.get())) {
                promoteParamsInBody(ctor->body, cls->name, ctor->parameters, cls->name + "::" + ctor->name);
            }
        }
    }
}

void SmartPointerAnalyzer::promoteParamsInBody(const std::vector<StatementPtr>& body,
                                               const std::string& className,
                                               const std::vector<std::pair<std::string, std::string>>& params,
                                               const std::string& scope) {
    // A store is recognized as `field = param;` (bare field name, or through another object: `array.child = item;`) anywhere in a body, including inside
    // if/while/for/try and in free functions. Promotion is always safe (shared only means the caller's variable is copied instead of moved). The field's
    // owning class is resolved from the target's base expression (parameter, local or field chain), else the enclosing class.
    std::map<std::string, std::string> localTypes;
    for (const auto& [ptype, pname] : params) localTypes[pname] = ptype;
    std::function<void(const std::vector<StatementPtr>&)> collectLocals =
        [&](const std::vector<StatementPtr>& stmts) {
            for (const auto& sp : stmts) {
                if (auto decl = dynamic_cast<const VariableDeclaration*>(sp.get())) {
                    if (localTypes.find(decl->identifier) == localTypes.end()) {
                        localTypes[decl->identifier] = decl->type;
                    }
                } else if (auto ifs = dynamic_cast<const IfStatement*>(sp.get())) {
                    collectLocals(ifs->body);
                    collectLocals(ifs->elseBody);
                } else if (auto whiles = dynamic_cast<const WhileStatement*>(sp.get())) {
                    collectLocals(whiles->body);
                } else if (auto fors = dynamic_cast<const ForStatement*>(sp.get())) {
                    collectLocals(fors->body);
                } else if (auto trys = dynamic_cast<const TryStatement*>(sp.get())) {
                    collectLocals(trys->body);
                    collectLocals(trys->catchBody);
                }
            }
        };
    collectLocals(body);
    std::function<std::string(const Expression*)> classOf = [&](const Expression* e) -> std::string {
        if (auto id = dynamic_cast<const Identifier*>(e)) {
            auto it = localTypes.find(id->name);
            if (it != localTypes.end()) return it->second;
            if (!className.empty()) {
                auto f = symbolTypesByScope.find(makeSymbolKey(className, id->name));
                if (f != symbolTypesByScope.end()) return f->second;
            }
            return "";
        }
        if (auto m = dynamic_cast<const MemberAccess*>(e)) {
            const std::string owner = classOf(m->object.get());
            if (owner.empty()) return "";
            auto f = symbolTypesByScope.find(makeSymbolKey(owner, m->member));
            return f == symbolTypesByScope.end() ? std::string() : f->second;
        }
        return "";
    };
    std::function<void(const std::vector<StatementPtr>&)> visit;
    visit = [&](const std::vector<StatementPtr>& stmts) {
    for (const auto& stmtPtr : stmts) {
        if (auto ifs = dynamic_cast<const IfStatement*>(stmtPtr.get())) {
            visit(ifs->body);
            visit(ifs->elseBody);
            continue;
        }
        if (auto whiles = dynamic_cast<const WhileStatement*>(stmtPtr.get())) {
            visit(whiles->body);
            continue;
        }
        if (auto fors = dynamic_cast<const ForStatement*>(stmtPtr.get())) {
            visit(fors->body);
            continue;
        }
        if (auto trys = dynamic_cast<const TryStatement*>(stmtPtr.get())) {
            visit(trys->body);
            visit(trys->catchBody);
            continue;
        }
        auto assign = dynamic_cast<const AssignmentStatement*>(stmtPtr.get());
        if (assign == nullptr) continue;

        std::string fieldName;
        std::string fieldOwner = className;  // class whose field is being stored into
        if (!assign->target) {
            if (className.empty()) continue;  // a plain local assignment in a free function
            fieldName = assign->variable;  // bare identifier, implicit `this`
        } else if (auto member = dynamic_cast<const MemberAccess*>(assign->target.get())) {
            fieldName = member->member;
            const std::string resolved = classOf(member->object.get());
            if (!resolved.empty()) fieldOwner = resolved;
            if (fieldOwner.empty()) continue;
        } else {
            continue;
        }

        auto rhsId = dynamic_cast<const Identifier*>(assign->expression.get());
        if (rhsId == nullptr) continue;

        // RHS must be one of this body's own parameters, or (when the field is already shared) a local declared in it: a local stored into a shared field must be
        // shared, or `field = local` assigns a unique_ptr lvalue to a shared_ptr.
        const bool rhsIsParam = std::any_of(params.begin(), params.end(),
            [&](const auto& p) { return p.second == rhsId->name; });
        const bool rhsIsLocal = !rhsIsParam && localTypes.count(rhsId->name) > 0;
        if (!rhsIsParam && !rhsIsLocal) continue;

        // The field's declared type tells which (class, name) pair to ask getPointerType about.
        const auto fieldTypeIt = symbolTypesByScope.find(makeSymbolKey(fieldOwner, fieldName));
        if (fieldTypeIt == symbolTypesByScope.end()) continue;

        std::pair<std::string, std::string> paramTypeEntry;
        if (rhsIsParam) {
            auto pit = std::find_if(params.begin(), params.end(),
                [&](const auto& p) { return p.second == rhsId->name; });
            paramTypeEntry = *pit;
        } else {
            paramTypeEntry = {localTypes[rhsId->name], rhsId->name};
        }
        auto paramTypeIt = &paramTypeEntry;
        if (classNames.find(paramTypeIt->first) == classNames.end()) continue;
        if (rhsIsLocal && getPointerType(fieldTypeIt->second, fieldName) != PointerType::SHARED) continue;

        if (getPointerType(fieldTypeIt->second, fieldName) != PointerType::SHARED) {
            // The field was not decided shared by anything, most commonly by secondPassDeterminePointerTypes' base case ("default unique ownership"). An inline
            // `new` passed as a constructor argument is emitted with getPointerType's not-found default, which is shared, so a unique field would not accept it.
            // Make the field agree with that default. `pointerDecisions` has an entry for every key, so the reason string is the only way to tell the base case
            // from a real rule (hint, back-reference, bidirectional, shared-root, data-flow); a real rule is not overridden.
            bool fieldHasOwnSignal = false;
            for (const auto& [key, ref] : variableReferences) {
                if (ref.className == fieldTypeIt->second && ref.variableName == fieldName) {
                    auto reasonIt = pointerReasons.find(key);
                    if (reasonIt != pointerReasons.end() && reasonIt->second != "default unique ownership") {
                        fieldHasOwnSignal = true;
                        break;
                    }
                }
            }
            if (fieldHasOwnSignal) continue;

            for (auto& [key, ref] : variableReferences) {
                if (ref.className == fieldTypeIt->second && ref.variableName == fieldName) {
                    pointerDecisions[key] = PointerType::SHARED;
                    pointerReasons[key] =
                        "field is populated from a stored constructor parameter, matching "
                        "the SHARED default a fresh inline-constructed argument already gets";
                    ref.reason = pointerReasons[key];
                }
            }
        }

        // The parameter's declared type gives the (class, name) key to promote; registerParameters gave every object-typed parameter a variableReferences entry.
        for (auto& [key, ref] : variableReferences) {
            if (ref.className == paramTypeIt->first && ref.variableName == rhsId->name &&
                (rhsIsParam || ref.containingScope == normalizeScope(scope))) {
                pointerDecisions[key] = PointerType::SHARED;
                pointerReasons[key] = "parameter is stored into an already-shared field";
                ref.reason = pointerReasons[key];
            }
        }
    }
    };
    visit(body);
}

namespace {
// Transient lookup built once per promoteSharedCallArguments call: a callee (free function name, or "ClassName::methodName") to its declared parameter list.
struct CalleeParams {
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> freeFunctions;
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> methods;      // "Class::method"
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> constructors; // "Class"
};

CalleeParams collectCalleeParams(const Program& program) {
    CalleeParams out;
    for (const auto& stmt : program.statements) {
        if (auto fn = dynamic_cast<const FunctionDeclaration*>(stmt.get())) {
            out.freeFunctions[fn->name] = fn->parameters;
            // Also register under the qualified name ("YAML::Clone"): a call outside the namespace spells it that way (FunctionCall::function_name is verbatim).
            if (!fn->namespaceName.empty()) {
                out.freeFunctions[fn->namespaceName + "::" + fn->name] = fn->parameters;
            }
        } else if (auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
            for (const auto& member : cls->members) {
                if (auto method = dynamic_cast<const MethodDeclaration*>(member.get())) {
                    // Specializations are excluded: out.methods is keyed by bare "Class::method", and a plain call never resolves to a specialization, which would only overwrite the primary's entry.
                    if (!method->specializationArg.empty()) continue;
                    out.methods[cls->name + "::" + method->name] = method->parameters;
                } else if (auto ctor = dynamic_cast<const ConstructorDeclaration*>(member.get())) {
                    out.constructors[cls->name] = ctor->parameters;
                }
            }
        }
    }
    return out;
}
} // namespace

void SmartPointerAnalyzer::promoteSharedCallArguments(const Program& program) {
    const CalleeParams calleeParams = collectCalleeParams(program);
    // The precise scope (free function name, "main", or "Class::method") of the body visitStmt/visitExpr are walking. Every symbolTypesByScope lookup tries it
    // before the kGlobalScope fallback, which is last-write-wins and collides when unrelated functions declare a same-named local of different types.
    std::string currentScope;

    // A free function named `begin`/`end` taking one class-typed parameter exists to support ADL range-for from other .clg files. The forcing further down
    // only sees a `for` in the same file, and each .clg is compiled separately, so the parameter is forced shared here unconditionally.
    for (const char* fnName : {"begin", "end"}) {
        const auto paramsIt = calleeParams.freeFunctions.find(fnName);
        if (paramsIt == calleeParams.freeFunctions.end() || paramsIt->second.size() != 1) continue;
        const auto& [paramType, paramName] = paramsIt->second[0];
        if (classNames.find(paramType) == classNames.end()) continue;
        const std::string paramKey = buildKey(fnName, paramType, paramName);
        pointerDecisions[paramKey] = PointerType::SHARED;
        pointerReasons[paramKey] =
            "begin()/end() parameter for ADL range-for support (by-value, must accept a shared_ptr caller "
            "argument from any .clg file, not just one with a for-loop in the same transpile unit)";
    }

    // A method `operator==`/`operator!=` whose single parameter is the class itself serves comparison sites anywhere (AstEmitter routes `a == b` through it).
    // Callers compare shared_ptr locals, so the parameter is forced shared whatever this file's own usage shows.
    for (const auto& stmt : program.statements) {
        auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get());
        if (cls == nullptr) continue;
        for (const auto& member : cls->members) {
            auto method = dynamic_cast<const MethodDeclaration*>(member.get());
            if (method == nullptr) continue;
            if (method->name != "operator==" && method->name != "operator!=") continue;
            if (method->parameters.size() != 1 || method->parameters[0].first != cls->name) continue;
            const std::string methodScope = cls->name + "::" + method->name;
            const std::string paramKey =
                buildKey(methodScope, method->parameters[0].first, method->parameters[0].second);
            pointerDecisions[paramKey] = PointerType::SHARED;
            pointerReasons[paramKey] =
                "operator==/!= comparison parameter (must accept a shared_ptr caller argument from any "
                "comparison call site, not just one with matching local usage evidence in this file)";
        }
    }

    // Same cross-file reason, for every free-function parameter classified read-only (classifyReadOnlyFreeFunctionParams): the per-call-site forcing below only
    // sees calls in this file, but a read-only function is read-only whatever its callers do, so force it here unconditionally.
    for (const auto& stmt : program.statements) {
        auto fn = dynamic_cast<const FunctionDeclaration*>(stmt.get());
        if (fn == nullptr) continue;
        for (const auto& [paramType, paramName] : fn->parameters) {
            if (classNames.find(paramType) == classNames.end()) continue;
            if (!isReadOnlyFreeFunctionParam(fn->name, paramName)) continue;
            const std::string paramKey = buildKey(fn->name, paramType, paramName);
            pointerDecisions[paramKey] = PointerType::SHARED;
            pointerReasons[paramKey] =
                "read-only free-function parameter (forced regardless of call sites in this transpile unit -- "
                "a caller in ANY .clg file needs a shared_ptr to bind to)";
        }
    }

    // "ClassName" -> parameter name of that class's handle-rebind `operator=(SameClass other)` overload, if declared. Kept apart from calleeParams.methods, which
    // collapses overloads to one entry per class (a class with both `operator=(string)` and `operator=(SameClass)` would lose one). Mirrors AstEmitter::operatorAssignParamTypes_.
    std::map<std::string, std::string> handleRebindAssignParamName;
    for (const auto& stmt : program.statements) {
        auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get());
        if (cls == nullptr) continue;
        for (const auto& member : cls->members) {
            auto method = dynamic_cast<const MethodDeclaration*>(member.get());
            if (method == nullptr || method->name != "operator=" || method->parameters.size() != 1) continue;
            if (method->parameters[0].first == cls->name) {
                handleRebindAssignParamName[cls->name] = method->parameters[0].second;
            }
        }
    }

    // Resolve a call or constructor's target parameter list and promote every argument bound to an already-shared parameter, so the caller's variable is
    // shared too. Arguments are resolved lightly (bare identifier, via symbolTypesByScope).
    auto promoteArgsAgainstParams = [this, &currentScope](
        const std::vector<ExpressionPtr>& args,
        const std::vector<std::pair<std::string, std::string>>& params) {
        for (size_t i = 0; i < args.size() && i < params.size(); ++i) {
            const auto& [paramType, paramName] = params[i];
            if (classNames.find(paramType) == classNames.end()) continue;
            if (getPointerType(paramType, paramName) != PointerType::SHARED) continue;

            auto argId = dynamic_cast<const Identifier*>(args[i].get());
            if (argId == nullptr) continue;
            auto argTypeIt = currentScope.empty() ? symbolTypesByScope.end()
                : symbolTypesByScope.find(makeSymbolKey(currentScope, argId->name));
            if (argTypeIt == symbolTypesByScope.end()) {
                argTypeIt = symbolTypesByScope.find(makeSymbolKey(kGlobalScope, argId->name));
            }
            if (argTypeIt == symbolTypesByScope.end()) continue;

            for (auto& [key, ref] : variableReferences) {
                if (ref.className == argTypeIt->second && ref.variableName == argId->name) {
                    pointerDecisions[key] = PointerType::SHARED;
                    pointerReasons[key] = "passed to a parameter that is stored into an already-shared field";
                    ref.reason = pointerReasons[key];
                }
            }
        }
    };

    std::function<void(const Expression*)> visitExpr = [&](const Expression* expr) {
        if (expr == nullptr) return;
        if (auto call = dynamic_cast<const FunctionCall*>(expr)) {
            for (const auto& arg : call->arguments) visitExpr(arg.get());
            if (call->callee) {
                // `recv.method(args)`: resolve recv's class, then the method's parameter list.
                if (auto member = dynamic_cast<const MemberAccess*>(call->callee.get())) {
                    if (auto recvId = dynamic_cast<const Identifier*>(member->object.get())) {
                        auto recvTypeIt = currentScope.empty() ? symbolTypesByScope.end()
                            : symbolTypesByScope.find(makeSymbolKey(currentScope, recvId->name));
                        if (recvTypeIt == symbolTypesByScope.end()) {
                            recvTypeIt = symbolTypesByScope.find(makeSymbolKey(kGlobalScope, recvId->name));
                        }
                        if (recvTypeIt != symbolTypesByScope.end()) {
                            const auto paramsIt = calleeParams.methods.find(
                                recvTypeIt->second + "::" + member->member);
                            if (paramsIt != calleeParams.methods.end()) {
                                promoteArgsAgainstParams(call->arguments, paramsIt->second);
                            }
                        }
                    }
                }
            } else {
                // Plain call `f(args)`.
                const auto paramsIt = calleeParams.freeFunctions.find(call->function_name);
                if (paramsIt != calleeParams.freeFunctions.end()) {
                    promoteArgsAgainstParams(call->arguments, paramsIt->second);

                    // A read-only parameter (isReadOnlyFreeFunctionParam) must receive the caller's argument without consuming it. Both the parameter and the argument are
                    // forced shared, since a shared_ptr argument cannot bind to a unique_ptr parameter. buildKey needs the callee's bare name (parameters are scoped under
                    // the unqualified function name), so a qualified `call->function_name` is stripped.
                    const size_t fnSepPos = call->function_name.rfind("::");
                    const std::string calleeBareName = fnSepPos == std::string::npos
                        ? call->function_name : call->function_name.substr(fnSepPos + 2);
                    for (size_t i = 0; i < call->arguments.size() && i < paramsIt->second.size(); ++i) {
                        const auto& [paramType, paramName] = paramsIt->second[i];
                        if (!isReadOnlyFreeFunctionParam(call->function_name, paramName)) continue;

                        const std::string paramKey = buildKey(calleeBareName, paramType, paramName);
                        pointerDecisions[paramKey] = PointerType::SHARED;
                        pointerReasons[paramKey] =
                            "read-only free-function parameter (yaml-cpp API-compatibility: "
                            "must not consume the caller's argument, e.g. Clone(const Node&))";

                        auto argId = dynamic_cast<const Identifier*>(call->arguments[i].get());
                        if (argId == nullptr) continue;
                        auto argTypeIt = currentScope.empty() ? symbolTypesByScope.end()
                            : symbolTypesByScope.find(makeSymbolKey(currentScope, argId->name));
                        if (argTypeIt == symbolTypesByScope.end()) {
                            argTypeIt = symbolTypesByScope.find(makeSymbolKey(kGlobalScope, argId->name));
                        }
                        if (argTypeIt == symbolTypesByScope.end()) continue;
                        for (auto& [key, ref] : variableReferences) {
                            if (ref.className == argTypeIt->second && ref.variableName == argId->name) {
                                pointerDecisions[key] = PointerType::SHARED;
                                pointerReasons[key] =
                                    "passed to a read-only free-function parameter (must survive the call)";
                                ref.reason = pointerReasons[key];
                            }
                        }
                    }
                }
            }
        } else if (auto neww = dynamic_cast<const NewExpression*>(expr)) {
            for (const auto& arg : neww->arguments) visitExpr(arg.get());
            const auto paramsIt = calleeParams.constructors.find(neww->className);
            if (paramsIt != calleeParams.constructors.end()) {
                promoteArgsAgainstParams(neww->arguments, paramsIt->second);
            }
        } else if (auto bin = dynamic_cast<const BinaryExpression*>(expr)) {
            visitExpr(bin->left.get());
            visitExpr(bin->right.get());
            // `out << x` is a BinaryExpression, not a FunctionCall, so the call-site forcing above never sees it. A class-typed right operand streamed into a read-only
            // `operator<<` parameter needs the same promotion. Only a bare Identifier right operand is handled. `<<` is also integer shift, but promotion requires a
            // free `operator<<` whose second parameter is a known class and an operand of that type, so shifts fall through.
            if (bin->operator_str == "<<") {
                const auto opParamsIt = calleeParams.freeFunctions.find("operator<<");
                if (opParamsIt != calleeParams.freeFunctions.end() && opParamsIt->second.size() == 2) {
                    const auto& [paramType, paramName] = opParamsIt->second[1];
                    if (classNames.find(paramType) != classNames.end() &&
                        isReadOnlyFreeFunctionParam("operator<<", paramName)) {
                        if (auto rhsId = dynamic_cast<const Identifier*>(bin->right.get())) {
                            auto rhsTypeIt = currentScope.empty() ? symbolTypesByScope.end()
                                : symbolTypesByScope.find(makeSymbolKey(currentScope, rhsId->name));
                            if (rhsTypeIt == symbolTypesByScope.end()) {
                                rhsTypeIt = symbolTypesByScope.find(makeSymbolKey(kGlobalScope, rhsId->name));
                            }
                            // Strip a qualified rhs type ("YAML::Node") to its simple name before comparing with operator<<'s bare paramType.
                            std::string rhsSimpleType;
                            if (rhsTypeIt != symbolTypesByScope.end()) {
                                const size_t sepPos = rhsTypeIt->second.rfind("::");
                                rhsSimpleType = sepPos == std::string::npos
                                    ? rhsTypeIt->second : rhsTypeIt->second.substr(sepPos + 2);
                            }
                            if (rhsTypeIt != symbolTypesByScope.end() && rhsSimpleType == paramType) {
                                const std::string paramKey = buildKey("operator<<", paramType, paramName);
                                pointerDecisions[paramKey] = PointerType::SHARED;
                                pointerReasons[paramKey] =
                                    "read-only operator<< parameter (must not consume the streamed value)";
                                for (auto& [key, ref] : variableReferences) {
                                    if (ref.className == rhsTypeIt->second && ref.variableName == rhsId->name) {
                                        pointerDecisions[key] = PointerType::SHARED;
                                        pointerReasons[key] = "streamed via operator<< (must survive the call)";
                                        ref.reason = pointerReasons[key];
                                    }
                                }
                            }
                        }
                    }
                }
            }
            // `a == b` / `a != b` on the same class with its own operator==/!=: AstEmitter routes it through `a->operator==(b)`, which is not a FunctionCall, so the
            // caller's right operand needs forcing here too (the method's own parameter is forced shared near the top).
            if ((bin->operator_str == "==" || bin->operator_str == "!=")) {
                if (auto rhsId = dynamic_cast<const Identifier*>(bin->right.get())) {
                    auto rhsTypeIt = currentScope.empty() ? symbolTypesByScope.end()
                        : symbolTypesByScope.find(makeSymbolKey(currentScope, rhsId->name));
                    if (rhsTypeIt == symbolTypesByScope.end()) {
                        rhsTypeIt = symbolTypesByScope.find(makeSymbolKey(kGlobalScope, rhsId->name));
                    }
                    if (rhsTypeIt != symbolTypesByScope.end() && classNames.count(rhsTypeIt->second) > 0) {
                        const size_t sepPos = rhsTypeIt->second.rfind("::");
                        const std::string rhsSimpleType =
                            sepPos == std::string::npos ? rhsTypeIt->second : rhsTypeIt->second.substr(sepPos + 2);
                        const std::string methodName = "operator" + bin->operator_str;
                        const auto opParamsIt = calleeParams.methods.find(rhsSimpleType + "::" + methodName);
                        if (opParamsIt != calleeParams.methods.end() && opParamsIt->second.size() == 1 &&
                            opParamsIt->second[0].first == rhsSimpleType) {
                            for (auto& [key, ref] : variableReferences) {
                                if (ref.className == rhsTypeIt->second && ref.variableName == rhsId->name) {
                                    pointerDecisions[key] = PointerType::SHARED;
                                    pointerReasons[key] = "passed as the right operand of a class-typed ==/!= "
                                                           "comparison (must bind to the shared_ptr parameter)";
                                    ref.reason = pointerReasons[key];
                                }
                            }
                        }
                    }
                }
            }
        } else if (auto un = dynamic_cast<const UnaryExpression*>(expr)) {
            visitExpr(un->operand.get());
        } else if (auto member = dynamic_cast<const MemberAccess*>(expr)) {
            visitExpr(member->object.get());
        } else if (auto sub = dynamic_cast<const SubscriptExpression*>(expr)) {
            visitExpr(sub->object.get());
            visitExpr(sub->index.get());
        }
    };

    std::function<void(const Statement*)> visitStmt = [&](const Statement* stmt) {
        if (stmt == nullptr) return;
        if (auto exprStmt = dynamic_cast<const ExpressionStatement*>(stmt)) {
            visitExpr(exprStmt->expression.get());
        } else if (auto decl = dynamic_cast<const VariableDeclaration*>(stmt)) {
            visitExpr(decl->expression.get());
        } else if (auto assign = dynamic_cast<const AssignmentStatement*>(stmt)) {
            visitExpr(assign->expression.get());
            // Same-class `n = otherNode;` handle-rebind is handled by forceHandleRebindAssignments below, which needs class/method scope: an ordinary loop-advancement
            // local (`cur = cur.nextSibling();`) has the same shape but must stay a plain repoint.
        } else if (auto ret = dynamic_cast<const ReturnStatement*>(stmt)) {
            visitExpr(ret->expression.get());
        } else if (auto ifs = dynamic_cast<const IfStatement*>(stmt)) {
            visitExpr(ifs->condition.get());
            for (const auto& inner : ifs->body) visitStmt(inner.get());
            for (const auto& inner : ifs->elseBody) visitStmt(inner.get());
        } else if (auto whiles = dynamic_cast<const WhileStatement*>(stmt)) {
            visitExpr(whiles->condition.get());
            for (const auto& inner : whiles->body) visitStmt(inner.get());
        } else if (auto fors = dynamic_cast<const ForStatement*>(stmt)) {
            visitExpr(fors->container.get());
            // Range-for over a class-typed container using ADL `begin(container)` / `end(container)` calls both free functions in the generated C++, invisible to the
            // walk below. The container variable and both functions' parameters are forced shared: they take the container by value twice, which a unique_ptr cannot do.
            if (auto containerId = dynamic_cast<const Identifier*>(fors->container.get())) {
                auto classIt = currentScope.empty() ? symbolTypesByScope.end()
                    : symbolTypesByScope.find(makeSymbolKey(currentScope, containerId->name));
                if (classIt == symbolTypesByScope.end()) {
                    classIt = symbolTypesByScope.find(makeSymbolKey(kGlobalScope, containerId->name));
                }
                if (classIt != symbolTypesByScope.end()) {
                    const std::string& containerClass = classIt->second;
                    // Strip a qualified container type ("YAML::Node") to its simple name before comparing with a bare parameter type.
                    const size_t sepPos = containerClass.rfind("::");
                    const std::string containerSimpleClass =
                        sepPos == std::string::npos ? containerClass : containerClass.substr(sepPos + 2);
                    const bool hasMemberBegin =
                        calleeParams.methods.find(containerClass + "::begin") != calleeParams.methods.end() ||
                        calleeParams.methods.find(containerSimpleClass + "::begin") != calleeParams.methods.end();
                    if (!hasMemberBegin && classNames.find(containerClass) != classNames.end()) {
                        const bool hasFreeBegin = calleeParams.freeFunctions.count("begin") > 0;
                        const bool hasFreeEnd = calleeParams.freeFunctions.count("end") > 0;
                        if (hasFreeBegin && hasFreeEnd) {
                            // Force the container variable itself.
                            for (auto& [key, ref] : variableReferences) {
                                if (ref.className == containerClass && ref.variableName == containerId->name) {
                                    pointerDecisions[key] = PointerType::SHARED;
                                    pointerReasons[key] =
                                        "used as a range-for container whose iteration relies on ADL-found free begin()/end() (by-value parameters, unique_ptr cannot work)";
                                    ref.reason = pointerReasons[key];
                                }
                            }
                            // Force `begin`'s and `end`'s own first parameter.
                            for (const char* fnName : {"begin", "end"}) {
                                const auto& params = calleeParams.freeFunctions.at(fnName);
                                if (params.empty()) continue;
                                const auto& [paramType, paramName] = params[0];
                                if (paramType != containerClass && paramType != containerSimpleClass) continue;
                                const std::string paramKey = buildKey(fnName, paramType, paramName);
                                pointerDecisions[paramKey] = PointerType::SHARED;
                                pointerReasons[paramKey] =
                                    "range-for container parameter for ADL-found free begin()/end() (by-value, unique_ptr cannot work)";
                            }
                        }
                    }
                }
            }
            for (const auto& inner : fors->body) visitStmt(inner.get());
        } else if (auto tryStmt = dynamic_cast<const TryStatement*>(stmt)) {
            for (const auto& inner : tryStmt->body) visitStmt(inner.get());
            for (const auto& inner : tryStmt->catchBody) visitStmt(inner.get());
        } else if (auto throwStmt = dynamic_cast<const ThrowStatement*>(stmt)) {
            visitExpr(throwStmt->expression.get());
        }
    };

    for (const auto& stmt : program.statements) {
        if (auto fn = dynamic_cast<const FunctionDeclaration*>(stmt.get())) {
            currentScope = fn->name;
            for (const auto& inner : fn->body) visitStmt(inner.get());
        } else if (auto mn = dynamic_cast<const MainFunction*>(stmt.get())) {
            currentScope = "main";
            for (const auto& inner : mn->body) visitStmt(inner.get());
        } else if (auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
            for (const auto& member : cls->members) {
                if (auto method = dynamic_cast<const MethodDeclaration*>(member.get())) {
                    currentScope = cls->name + "::" + method->name;
                    for (const auto& inner : method->body) visitStmt(inner.get());
                } else if (auto ctor = dynamic_cast<const ConstructorDeclaration*>(member.get())) {
                    currentScope = cls->name + "::" + ctor->name;
                    for (const auto& inner : ctor->body) visitStmt(inner.get());
                }
            }
        }
    }

    // A separate walk for implicit-this calls to a read-only sibling method (`parseBlockMapInto(target, ...)`): the callee is null, like a free call, but
    // calleeParams.freeFunctions has no entry for a method. Mirrors the free-function forcing above, keyed by (class, method).
    std::function<void(const Expression*, const std::string&, const std::string&)> forceReadOnlyMethodArg =
        [&](const Expression* expr, const std::string& currentClass, const std::string& currentMethod) {
            if (expr == nullptr) return;
            if (auto call = dynamic_cast<const FunctionCall*>(expr)) {
                for (const auto& arg : call->arguments) forceReadOnlyMethodArg(arg.get(), currentClass, currentMethod);
                if (!call->callee) {
                    const auto paramsIt = calleeParams.methods.find(currentClass + "::" + call->function_name);
                    if (paramsIt != calleeParams.methods.end()) {
                        for (size_t i = 0; i < call->arguments.size() && i < paramsIt->second.size(); ++i) {
                            const auto& [paramType, paramName] = paramsIt->second[i];
                            if (!isReadOnlyMethodParam(currentClass, call->function_name, paramName)) continue;

                            const std::string paramKey =
                                buildKey(currentClass + "::" + call->function_name, paramType, paramName);
                            pointerDecisions[paramKey] = PointerType::SHARED;
                            pointerReasons[paramKey] =
                                "read-only method parameter (mutated in place across a same-class call chain, "
                                "e.g. Parser::parseValueInto's `target` -- must not consume the caller's argument)";

                            auto argId = dynamic_cast<const Identifier*>(call->arguments[i].get());
                            if (argId == nullptr) continue;
                            const auto argTypeIt = symbolTypesByScope.find(makeSymbolKey(currentMethod, argId->name));
                            const auto argTypeItGlobal =
                                symbolTypesByScope.find(makeSymbolKey(kGlobalScope, argId->name));
                            const std::string argType = argTypeIt != symbolTypesByScope.end() ? argTypeIt->second
                                : (argTypeItGlobal != symbolTypesByScope.end() ? argTypeItGlobal->second : "");
                            if (argType.empty()) continue;
                            for (auto& [key, ref] : variableReferences) {
                                if (ref.className == argType && ref.variableName == argId->name) {
                                    pointerDecisions[key] = PointerType::SHARED;
                                    pointerReasons[key] =
                                        "passed to a read-only method parameter (must survive the call)";
                                    ref.reason = pointerReasons[key];
                                }
                            }
                        }
                    }
                }
                if (call->callee) forceReadOnlyMethodArg(call->callee.get(), currentClass, currentMethod);
                return;
            }
            if (auto bin = dynamic_cast<const BinaryExpression*>(expr)) {
                forceReadOnlyMethodArg(bin->left.get(), currentClass, currentMethod);
                forceReadOnlyMethodArg(bin->right.get(), currentClass, currentMethod);
            } else if (auto un = dynamic_cast<const UnaryExpression*>(expr)) {
                forceReadOnlyMethodArg(un->operand.get(), currentClass, currentMethod);
            } else if (auto member = dynamic_cast<const MemberAccess*>(expr)) {
                forceReadOnlyMethodArg(member->object.get(), currentClass, currentMethod);
            } else if (auto sub = dynamic_cast<const SubscriptExpression*>(expr)) {
                forceReadOnlyMethodArg(sub->object.get(), currentClass, currentMethod);
                forceReadOnlyMethodArg(sub->index.get(), currentClass, currentMethod);
            }
        };
    std::function<void(const std::vector<StatementPtr>&, const std::string&, const std::string&)>
        forceReadOnlyMethodArgsInBody = [&](const std::vector<StatementPtr>& body, const std::string& currentClass,
                                             const std::string& currentMethod) {
            for (const auto& stmtPtr : body) {
                const Statement* stmt = stmtPtr.get();
                if (auto exprStmt = dynamic_cast<const ExpressionStatement*>(stmt)) {
                    forceReadOnlyMethodArg(exprStmt->expression.get(), currentClass, currentMethod);
                } else if (auto decl = dynamic_cast<const VariableDeclaration*>(stmt)) {
                    forceReadOnlyMethodArg(decl->expression.get(), currentClass, currentMethod);
                } else if (auto assign = dynamic_cast<const AssignmentStatement*>(stmt)) {
                    forceReadOnlyMethodArg(assign->expression.get(), currentClass, currentMethod);
                } else if (auto ret = dynamic_cast<const ReturnStatement*>(stmt)) {
                    forceReadOnlyMethodArg(ret->expression.get(), currentClass, currentMethod);
                } else if (auto ifs = dynamic_cast<const IfStatement*>(stmt)) {
                    forceReadOnlyMethodArg(ifs->condition.get(), currentClass, currentMethod);
                    forceReadOnlyMethodArgsInBody(ifs->body, currentClass, currentMethod);
                    forceReadOnlyMethodArgsInBody(ifs->elseBody, currentClass, currentMethod);
                } else if (auto whiles = dynamic_cast<const WhileStatement*>(stmt)) {
                    forceReadOnlyMethodArg(whiles->condition.get(), currentClass, currentMethod);
                    forceReadOnlyMethodArgsInBody(whiles->body, currentClass, currentMethod);
                } else if (auto fors = dynamic_cast<const ForStatement*>(stmt)) {
                    forceReadOnlyMethodArgsInBody(fors->body, currentClass, currentMethod);
                } else if (auto trys = dynamic_cast<const TryStatement*>(stmt)) {
                    forceReadOnlyMethodArgsInBody(trys->body, currentClass, currentMethod);
                    forceReadOnlyMethodArgsInBody(trys->catchBody, currentClass, currentMethod);
                }
            }
        };
    for (const auto& stmt : program.statements) {
        auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get());
        if (cls == nullptr) continue;
        for (const auto& member : cls->members) {
            auto method = dynamic_cast<const MethodDeclaration*>(member.get());
            if (method == nullptr) continue;
            forceReadOnlyMethodArgsInBody(method->body, cls->name, cls->name + "::" + method->name);
        }
    }

    // `n = otherNode;` handle-rebind forcing: the emitter routes a same-class assignment through the user's `operator=` only when the LHS is a parameter already
    // classified read-only, so an ordinary local (`cur = cur.nextSibling();`) must not be forced shared here. Scoped per (class, method) to query
    // isReadOnlyMethodParam / isReadOnlyFreeFunctionParam.
    std::function<void(const std::vector<StatementPtr>&, const std::string&, const std::string&)>
        forceHandleRebindAssignmentsInBody = [&](const std::vector<StatementPtr>& body,
                                                  const std::string& currentClass,
                                                  const std::string& currentFunctionName) {
            for (const auto& stmtPtr : body) {
                const Statement* stmt = stmtPtr.get();
                if (auto assign = dynamic_cast<const AssignmentStatement*>(stmt)) {
                    auto rhsId = dynamic_cast<const Identifier*>(assign->expression.get());
                    if (rhsId == nullptr) continue;
                    // currentFunctionName is scoped precisely ("Class::method" or the bare name), so try it before the kGlobalScope fallback.
                    const std::string preciseScope = currentClass.empty()
                        ? currentFunctionName : currentClass + "::" + currentFunctionName;
                    auto lhsTypeIt = symbolTypesByScope.find(makeSymbolKey(preciseScope, assign->variable));
                    if (lhsTypeIt == symbolTypesByScope.end()) {
                        lhsTypeIt = symbolTypesByScope.find(makeSymbolKey(kGlobalScope, assign->variable));
                    }
                    auto rhsTypeIt = symbolTypesByScope.find(makeSymbolKey(preciseScope, rhsId->name));
                    if (rhsTypeIt == symbolTypesByScope.end()) {
                        rhsTypeIt = symbolTypesByScope.find(makeSymbolKey(kGlobalScope, rhsId->name));
                    }
                    if (lhsTypeIt == symbolTypesByScope.end() || rhsTypeIt == symbolTypesByScope.end() ||
                        lhsTypeIt->second != rhsTypeIt->second ||
                        classNames.find(lhsTypeIt->second) == classNames.end()) {
                        continue;
                    }
                    const auto paramNameIt = handleRebindAssignParamName.find(lhsTypeIt->second);
                    if (paramNameIt == handleRebindAssignParamName.end()) continue;
                    const bool lhsIsReadOnlyParam =
                        (!currentClass.empty() &&
                         isReadOnlyMethodParam(currentClass, currentFunctionName, assign->variable)) ||
                        (currentClass.empty() && isReadOnlyFreeFunctionParam(currentFunctionName, assign->variable));
                    if (!lhsIsReadOnlyParam) continue;

                    const std::string methodScope = lhsTypeIt->second + "::operator=";
                    const std::string paramKey = buildKey(methodScope, lhsTypeIt->second, paramNameIt->second);
                    pointerDecisions[paramKey] = PointerType::SHARED;
                    pointerReasons[paramKey] =
                        "operator=(Node) handle-rebind parameter (must alias, not consume, the RHS)";
                    for (auto& [key, ref] : variableReferences) {
                        if (ref.className == rhsTypeIt->second && ref.variableName == rhsId->name) {
                            pointerDecisions[key] = PointerType::SHARED;
                            pointerReasons[key] = "passed to operator=(Node)'s handle-rebind parameter";
                            ref.reason = pointerReasons[key];
                        }
                    }
                } else if (auto ifs = dynamic_cast<const IfStatement*>(stmt)) {
                    forceHandleRebindAssignmentsInBody(ifs->body, currentClass, currentFunctionName);
                    forceHandleRebindAssignmentsInBody(ifs->elseBody, currentClass, currentFunctionName);
                } else if (auto whiles = dynamic_cast<const WhileStatement*>(stmt)) {
                    forceHandleRebindAssignmentsInBody(whiles->body, currentClass, currentFunctionName);
                } else if (auto fors = dynamic_cast<const ForStatement*>(stmt)) {
                    forceHandleRebindAssignmentsInBody(fors->body, currentClass, currentFunctionName);
                } else if (auto trys = dynamic_cast<const TryStatement*>(stmt)) {
                    forceHandleRebindAssignmentsInBody(trys->body, currentClass, currentFunctionName);
                    forceHandleRebindAssignmentsInBody(trys->catchBody, currentClass, currentFunctionName);
                }
            }
        };
    for (const auto& stmt : program.statements) {
        if (auto fn = dynamic_cast<const FunctionDeclaration*>(stmt.get())) {
            forceHandleRebindAssignmentsInBody(fn->body, "", fn->name);
        } else if (auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
            for (const auto& member : cls->members) {
                auto method = dynamic_cast<const MethodDeclaration*>(member.get());
                if (method == nullptr) continue;
                forceHandleRebindAssignmentsInBody(method->body, cls->name, method->name);
            }
        }
    }
}

} // namespace cling
