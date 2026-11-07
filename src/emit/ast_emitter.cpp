#include "emit/ast_emitter.h"
#include <functional>
#include "syntax/class_hierarchy.h"
#include "facts/library_summary.h"
#include "emit/runtime_helpers.h"
#include "passes/channel_check.h"

namespace cling {

namespace {
bool isNullptrLiteral(const Expression* expr) {
    auto id = dynamic_cast<const Identifier*>(expr);
    return id != nullptr && id->name == "nullptr";
}

// An initializer list runs in declaration order, so an expression may move into it only if it reads parameters, names that are not
// own fields, or own fields already hoisted. `this` and calls of the class's own methods are never safe. Otherwise hoisting stops
// there and the statement stays in the body.
bool initializerExprIsSafe(const Expression* expr, const std::set<std::string>& ownFields, const std::set<std::string>& hoisted,
                           const std::set<std::string>& params, const std::function<bool(const std::string&)>& isMethodName) {
    if (expr == nullptr) return true;
    if (dynamic_cast<const ThisExpression*>(expr) != nullptr) return false;
    if (auto id = dynamic_cast<const Identifier*>(expr)) {
        if (params.count(id->name) > 0) return true;
        if (ownFields.count(id->name) > 0) return hoisted.count(id->name) > 0;
        return true;
    }
    if (auto bin = dynamic_cast<const BinaryExpression*>(expr)) {
        return initializerExprIsSafe(bin->left.get(), ownFields, hoisted, params, isMethodName) &&
               initializerExprIsSafe(bin->right.get(), ownFields, hoisted, params, isMethodName);
    }
    if (auto un = dynamic_cast<const UnaryExpression*>(expr)) return initializerExprIsSafe(un->operand.get(), ownFields, hoisted, params, isMethodName);
    if (auto sub = dynamic_cast<const SubscriptExpression*>(expr)) {
        return initializerExprIsSafe(sub->object.get(), ownFields, hoisted, params, isMethodName) &&
               initializerExprIsSafe(sub->index.get(), ownFields, hoisted, params, isMethodName);
    }
    if (auto mem = dynamic_cast<const MemberAccess*>(expr)) return initializerExprIsSafe(mem->object.get(), ownFields, hoisted, params, isMethodName);
    if (auto made = dynamic_cast<const NewExpression*>(expr)) {
        for (const auto& a : made->arguments) if (!initializerExprIsSafe(a.get(), ownFields, hoisted, params, isMethodName)) return false;
        return true;
    }
    if (auto call = dynamic_cast<const FunctionCall*>(expr)) {
        if (call->callee != nullptr) {
            auto access = dynamic_cast<const MemberAccess*>(call->callee.get());
            if (access == nullptr || !initializerExprIsSafe(access->object.get(), ownFields, hoisted, params, isMethodName)) return false;
        } else if (isMethodName(call->function_name)) {
            return false;
        }
        for (const auto& a : call->arguments) if (!initializerExprIsSafe(a.get(), ownFields, hoisted, params, isMethodName)) return false;
        return true;
    }
    return true;   // literals and qualified names
}
} // namespace

std::string AstEmitter::emit(const Program& program) {
    classNames_.clear();
    varKinds_.clear();
    usedRuntimeHelpers_.clear();
    usesCopyWithin_ = false;
    usesVectorAlgorithms_ = false;
    usesBoundsCheck_ = false;
    collectClassNames(program);
    collectUsedRuntimeHelpers(program);
    usesNullCheck_ = false;
    // Classes from `.clh` includes count too: a weak field of one may be read here (`node.prev.next`).
    for (const auto& stmt : program.statements) {
        auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get());
        if (cls == nullptr) continue;
        for (const auto& member : cls->members) {
            auto field = dynamic_cast<const VariableDeclaration*>(member.get());
            if (field != nullptr && analyzer_.isWeakField(cls->name, field->identifier)) usesNullCheck_ = true;
        }
    }

    std::ostringstream out;
    // Headers the lowered constructs always need, de-duplicated against the source's own includes.
    std::set<std::string> emittedIncludes;
    for (const char* hdr : {"#include <memory>", "#include <utility>",
                            "#include <iostream>", "#include <vector>",
                            "#include <string>", "#include <string_view>", "#include <optional>", "#include <map>"}) {
        if (emittedIncludes.insert(hdr).second) out << hdr << "\n";
    }
    if (!usedRuntimeHelpers_.empty()) {
        for (const char* hdr : {"#include <fstream>", "#include <sstream>"}) {
            if (emittedIncludes.insert(hdr).second) out << hdr << "\n";
        }
    }
    if (usesCopyWithin_) {
        // std::memmove fallback of cling::copyWithin.
        if (emittedIncludes.insert("#include <cstring>").second) out << "#include <cstring>\n";
    }
    for (const auto& stmt : program.statements) {
        // Only this file's own includes; an included file carries its own.
        if (stmt->isExternalDeclaration) continue;
        if (auto inc = dynamic_cast<const IncludeStatement*>(stmt.get())) {
            if (emittedIncludes.insert(inc->includeFile).second) {
                out << inc->includeFile << "\n";
            }
        }
    }
    out << "\n";
    if (!usedRuntimeHelpers_.empty()) {
        out << requiredRuntimeHelperDefinitions(usedRuntimeHelpers_) << "\n";
    }
    if (usesCopyWithin_) {
        out << copyWithinHelperDefinition() << "\n";
    }
    if (usesVectorAlgorithms_) {
        out << vectorAlgorithmHelperDefinition() << "\n";
    }
    if (programUsesChannel(program)) {
        out << channelHelperDefinition() << "\n";
    }
    if (programUsesSpan(program)) {
        out << spanHelperDefinition() << "\n";
    }
    if (poolShared_) {
        out << poolAllocatorHelperDefinition() << "\n";
    }
    // Debug mode: the check helpers are inlined and artificial, so a debugger steps over them.
    auto forDebugger = [this](std::string helper) {
        if (!debugLines_) return helper;
        const std::string marker = "\ninline ";
        for (size_t at = helper.find(marker); at != std::string::npos; at = helper.find(marker, at + 1)) {
            helper.insert(at + 1, "__attribute__((always_inline, artificial)) ");
            at += 40;
        }
        return helper;
    };
    if (usesNullCheck_ || (nullChecks_ && !classNames_.empty())) {
        out << forDebugger(nullCheckHelperDefinition()) << "\n";
    }
    if (boundsChecks_ && usesBoundsCheck_) {
        out << forDebugger(boundsCheckHelperDefinition()) << "\n";
    }

    // Forward-declare every class this file defines (not structs, which are embedded by value), so classes can refer to each other in any order.
    {
        bool anyForward = false;
        for (const auto& stmt : program.statements) {
            if (stmt->isExternalDeclaration) continue;
            auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get());
            if (cls == nullptr || !cls->namespaceName.empty() || analyzer_.isValueTypeClass(cls->name)) continue;
            out << "class " << cls->name << ";\n";
            anyForward = true;
        }
        if (anyForward) out << "\n";
    }

    // When an earlier class's member bodies use a later class (still incomplete there, even with the forward declarations), emit all
    // class definitions first and all member bodies afterwards. Programs without that shape, and programs with namespaces, keep source order.
    deferClassBodies_ = false;
    deferredClassBodies_.clear();
    {
        bool anyNamespaced = false;
        std::map<std::string, size_t> classIndex;
        std::vector<const ClassDeclaration*> classOrder;
        for (const auto& stmt : program.statements) {
            if (stmt->isExternalDeclaration) continue;
            if (auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
                if (!cls->namespaceName.empty()) anyNamespaced = true;
                classIndex[cls->name] = classOrder.size();
                classOrder.push_back(cls);
            } else if (auto enm = dynamic_cast<const EnumDeclaration*>(stmt.get())) {
                if (!enm->namespaceName.empty()) anyNamespaced = true;
            } else if (auto fn = dynamic_cast<const FunctionDeclaration*>(stmt.get())) {
                if (!fn->namespaceName.empty()) anyNamespaced = true;
            }
        }
        auto mentionsLater = [&](const std::string& type, size_t self) {
            auto it = classIndex.find(type);
            return it != classIndex.end() && it->second > self;
        };
        for (size_t i = 0; i < classOrder.size() && !anyNamespaced && !deferClassBodies_; ++i) {
            for (const auto& member : classOrder[i]->members) {
                if (auto field = dynamic_cast<const VariableDeclaration*>(member.get())) {
                    if (mentionsLater(field->type, i)) deferClassBodies_ = true;
                } else if (auto method = dynamic_cast<const MethodDeclaration*>(member.get())) {
                    if (mentionsLater(method->returnType, i)) deferClassBodies_ = true;
                    for (const auto& [ptype, pname] : method->parameters) {
                        (void)pname;
                        if (mentionsLater(ptype, i)) deferClassBodies_ = true;
                    }
                } else if (auto ctor = dynamic_cast<const ConstructorDeclaration*>(member.get())) {
                    for (const auto& [ptype, pname] : ctor->parameters) {
                        (void)pname;
                        if (mentionsLater(ptype, i)) deferClassBodies_ = true;
                    }
                }
            }
        }
    }
    if (deferClassBodies_) {
        for (const auto& stmt : program.statements) {
            if (stmt->isExternalDeclaration) continue;
            if (dynamic_cast<const ClassDeclaration*>(stmt.get()) != nullptr ||
                dynamic_cast<const EnumDeclaration*>(stmt.get()) != nullptr) {
                emitTopLevel(stmt.get(), out);
            }
        }
        for (const auto& stmt : program.statements) {
            if (stmt->isExternalDeclaration) continue;
            if (auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
                out << deferredClassBodies_[cls->name];
            } else if (dynamic_cast<const EnumDeclaration*>(stmt.get()) == nullptr) {
                emitTopLevel(stmt.get(), out);
            }
        }
        return out.str();
    }

    // Wrap each contiguous run of one namespace in `namespace X { ... }`.
    std::string openNamespace;
    for (const auto& stmt : program.statements) {
        if (stmt->isExternalDeclaration) continue;
        std::string stmtNamespace;
        if (auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
            stmtNamespace = cls->namespaceName;
        } else if (auto enm = dynamic_cast<const EnumDeclaration*>(stmt.get())) {
            stmtNamespace = enm->namespaceName;
        } else if (auto fn = dynamic_cast<const FunctionDeclaration*>(stmt.get())) {
            stmtNamespace = fn->namespaceName;
        } else if (auto marker = dynamic_cast<const ExternalSpecializationMarker*>(stmt.get())) {
            if (auto it = classByName_.find(marker->targetClassName); it != classByName_.end()) {
                stmtNamespace = it->second->namespaceName;
            }
        }
        if (stmtNamespace != openNamespace) {
            if (!openNamespace.empty()) out << "}\n\n";
            if (!stmtNamespace.empty()) out << "namespace " << stmtNamespace << " {\n\n";
            openNamespace = stmtNamespace;
        }
        emitTopLevel(stmt.get(), out);
    }
    if (!openNamespace.empty()) out << "}\n\n";
    return out.str();
}

void AstEmitter::emitExternalSpecializationMarker(const ExternalSpecializationMarker& marker,
                                                    const ClassDeclaration& targetClass,
                                                    std::ostringstream& out) {
    const MethodDeclaration* method = marker.method;
    varKinds_.clear(); varClass_.clear(); observerClass_.clear(); containerVarType_.clear(); viewVars_.clear();
    // When the return type is the specialized type itself, the return and the locals of that type must be unwrapped (see isValueType).
    const bool returnIsSpecializedType = method->returnType == method->specializationArg;
    if (returnIsSpecializedType) {
        currentSpecializationValueType_ = method->specializationArg;
    }
    out << "template<>\n";
    emitFunction(method->returnType,
                 targetClass.name + "::" + method->name + "<" + cppTypeFor(method->specializationArg, "") + ">",
                 method->parameters, method->body, out, targetClass.name, 0, method->isConst,
                 false, false, method->name);
    if (returnIsSpecializedType) {
        currentSpecializationValueType_.clear();
    }
}

std::string AstEmitter::emitHeader(const Program& program, const std::string& guardName) {
    classNames_.clear();
    varKinds_.clear();
    collectClassNames(program);

    std::ostringstream out;
    out << "#ifndef " << guardName << "\n#define " << guardName << "\n\n";

    std::set<std::string> emittedIncludes;
    for (const char* hdr : {"#include <memory>", "#include <utility>",
                            "#include <iostream>", "#include <vector>",
                            "#include <string>", "#include <string_view>", "#include <optional>", "#include <map>"}) {
        if (emittedIncludes.insert(hdr).second) out << hdr << "\n";
    }
    if (programUsesChannel(program)) {
        out << channelHelperDefinition() << "\n";
    }
    if (programUsesSpan(program)) {
        out << spanHelperDefinition() << "\n";
    }
    for (const auto& stmt : program.statements) {
        if (stmt->isExternalDeclaration) continue;
        if (auto inc = dynamic_cast<const IncludeStatement*>(stmt.get())) {
            if (emittedIncludes.insert(inc->includeFile).second) {
                out << inc->includeFile << "\n";
            }
        }
    }
    // Forward-declare every external class this header's declarations mention, whatever the source #included: a header holds only
    // signatures, so an incomplete type suffices, and it avoids include cycles. Structs are not forward-declared (embedded by value).
    for (const auto& className : collectForwardDeclarableClasses(program)) {
        out << "class " << className << ";\n";
    }
    out << "\n";

    // Same namespace grouping as emit(): the header must nest in the same namespaces as the paired .cpp.
    std::string openNamespace;
    for (const auto& stmt : program.statements) {
        if (stmt->isExternalDeclaration) continue;
        std::string stmtNamespace;
        if (auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
            stmtNamespace = cls->namespaceName;
        } else if (auto enm = dynamic_cast<const EnumDeclaration*>(stmt.get())) {
            stmtNamespace = enm->namespaceName;
        } else if (auto fn = dynamic_cast<const FunctionDeclaration*>(stmt.get())) {
            stmtNamespace = fn->namespaceName;
        } else if (dynamic_cast<const ExternalSpecializationMarker*>(stmt.get())) {
            // Emits nothing in header mode; stay in the open namespace.
            stmtNamespace = openNamespace;
        }
        if (stmtNamespace != openNamespace) {
            if (!openNamespace.empty()) out << "}\n\n";
            if (!stmtNamespace.empty()) out << "namespace " << stmtNamespace << " {\n\n";
            openNamespace = stmtNamespace;
        }
        emitTopLevelHeader(stmt.get(), out);
    }
    if (!openNamespace.empty()) out << "}\n\n";

    out << "#endif // " << guardName << "\n";
    return out.str();
}

void AstEmitter::collectClassNames(const Program& program) {
    for (const auto& stmt : program.statements) {
        if (auto fn = dynamic_cast<const FunctionDeclaration*>(stmt.get())) {
            functionParams_[fn->name] = fn->parameters;
            functionReturnType_[fn->name] = fn->returnType;
            // Also register under the qualified name (`YAML::Load`), which is how a call outside the namespace spells it.
            if (!fn->namespaceName.empty()) {
                const std::string qualified = fn->namespaceName + "::" + fn->name;
                functionParams_[qualified] = fn->parameters;
                functionReturnType_[qualified] = fn->returnType;
            }
        } else if (auto extFn = dynamic_cast<const ExternFunctionDeclaration*>(stmt.get())) {
            functionParams_[extFn->name] = extFn->parameters;
            functionReturnType_[extFn->name] = extFn->returnType;
        }
    }
    // Flatten base classes first: a missing inherited field type would make an inherited smart-pointer field emit `.` instead of `->`.
    for (const ClassDeclaration* cls : classesInBaseFirstOrder(program)) {
        classNames_.insert(cls->name);
        classByName_[cls->name] = cls;
        if (!cls->namespaceName.empty()) {
            classByName_[cls->namespaceName + "::" + cls->name] = cls;
        }
        auto& fields = classFields_[cls->name];
        auto& methodParams = classMethodParams_[cls->name];
        auto& methodReturnTypes = classMethodReturnType_[cls->name];
        if (!cls->baseClass.empty()) {
            baseClasses_.insert(cls->baseClass);
            if (auto baseIt = classFields_.find(cls->baseClass); baseIt != classFields_.end()) {
                fields = baseIt->second;
            }
            if (auto baseIt = classMethodParams_.find(cls->baseClass);
                baseIt != classMethodParams_.end()) {
                methodParams = baseIt->second;
            }
            if (auto baseIt = classMethodReturnType_.find(cls->baseClass);
                baseIt != classMethodReturnType_.end()) {
                methodReturnTypes = baseIt->second;
            }
        }
        // Only a class that overloads a method name needs the arity-qualified key below.
        std::map<std::string, int> nameCounts;
        for (const auto& member : cls->members) {
            if (auto method = dynamic_cast<const MethodDeclaration*>(member.get())) {
                if (method->specializationArg.empty()) nameCounts[method->name]++;
            }
        }
        for (const auto& member : cls->members) {
            if (auto var = dynamic_cast<const VariableDeclaration*>(member.get())) {
                fields[var->identifier] = var->type;
            } else if (auto method = dynamic_cast<const MethodDeclaration*>(member.get())) {
                // A specialization registers under its synthetic key ("as<int>"); an overloaded method keys by overloadKey(name, paramCount).
                std::string key;
                if (!method->specializationArg.empty()) {
                    key = specializationKey(method->name, method->specializationArg);
                } else if (nameCounts[method->name] > 1) {
                    key = overloadKey(method->name, method->parameters.size());
                } else {
                    key = method->name;
                }
                methodParams[key] = method->parameters;
                methodReturnTypes[key] = method->returnType;
                // Every operator= overload's parameter type (classMethodParams_ keeps only one).
                if (method->name == "operator=" && method->parameters.size() == 1) {
                    operatorAssignParamTypes_[cls->name].insert(method->parameters[0].first);
                }
                // A primary generic with T as a parameter type but not the return type: that parameter is an out-parameter (see outParamIndex_).
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

        // Also register under the qualified name (`YAML::Node`) so a type spelled that way resolves through the same lookups.
        if (!cls->namespaceName.empty()) {
            const std::string qualified = cls->namespaceName + "::" + cls->name;
            classNames_.insert(qualified);
            classFields_[qualified] = fields;
            classMethodParams_[qualified] = methodParams;
            classMethodReturnType_[qualified] = methodReturnTypes;
            if (auto opIt = operatorAssignParamTypes_.find(cls->name); opIt != operatorAssignParamTypes_.end()) {
                operatorAssignParamTypes_[qualified] = opIt->second;
            }
            // The same for outParamIndex_, keyed by the qualified class name.
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

std::set<std::string> AstEmitter::collectForwardDeclarableClasses(const Program& program) const {
    // Classes this file declares itself get a full declaration; never forward-declare them.
    std::set<std::string> ownClasses;
    for (const auto& stmt : program.statements) {
        if (stmt->isExternalDeclaration) continue;
        if (auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
            ownClasses.insert(cls->name);
        }
    }

    // Candidates: known classes that are not namespace-qualified (`class YAML::Node;` is invalid), not this file's own, and not value types.
    std::set<std::string> result;
    auto consider = [&](const std::string& type) {
        if (type.empty()) return;
        if (type.find("::") != std::string::npos) return;
        if (ownClasses.count(type) > 0) return;
        if (classNames_.count(type) == 0) return;
        if (analyzer_.isValueTypeClass(type)) return;
        result.insert(type);
    };

    for (const auto& stmt : program.statements) {
        if (stmt->isExternalDeclaration) continue;
        if (auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
            for (const auto& member : cls->members) {
                if (auto field = dynamic_cast<const VariableDeclaration*>(member.get())) {
                    consider(field->type);
                } else if (auto method = dynamic_cast<const MethodDeclaration*>(member.get())) {
                    consider(method->returnType);
                    for (const auto& [ptype, pname] : method->parameters) {
                        (void)pname;
                        consider(ptype);
                    }
                } else if (auto ctor = dynamic_cast<const ConstructorDeclaration*>(member.get())) {
                    for (const auto& [ptype, pname] : ctor->parameters) {
                        (void)pname;
                        consider(ptype);
                    }
                }
            }
        } else if (auto fn = dynamic_cast<const FunctionDeclaration*>(stmt.get())) {
            consider(fn->returnType);
            for (const auto& [ptype, pname] : fn->parameters) {
                (void)pname;
                consider(ptype);
            }
        }
    }
    return result;
}

void AstEmitter::collectUsedRuntimeHelpers(const Program& program) {
    std::function<void(const Expression*)> visitExpr = [&](const Expression* expr) {
        if (expr == nullptr) return;
        if (auto call = dynamic_cast<const FunctionCall*>(expr)) {
            for (const auto& arg : call->arguments) visitExpr(arg.get());
            if (call->callee) {
                visitExpr(call->callee.get());
                // `v.copyWithin(...)` means <cstring> is needed. A false positive costs only an unused include.
                if (auto member = dynamic_cast<const MemberAccess*>(call->callee.get());
                    member != nullptr && member->member == "copyWithin") {
                    usesCopyWithin_ = true;
                }
                if (auto member = dynamic_cast<const MemberAccess*>(call->callee.get());
                    member != nullptr && isVectorAlgorithm(member->member)) {
                    usesVectorAlgorithms_ = true;      // (a false positive costs one unused helper, never a missing one)
                }
            } else if (isKnownRuntimeHelper(call->function_name)) {
                usedRuntimeHelpers_.insert(call->function_name);
            }
        } else if (auto neww = dynamic_cast<const NewExpression*>(expr)) {
            for (const auto& arg : neww->arguments) visitExpr(arg.get());
        } else if (auto bin = dynamic_cast<const BinaryExpression*>(expr)) {
            visitExpr(bin->left.get());
            visitExpr(bin->right.get());
        } else if (auto un = dynamic_cast<const UnaryExpression*>(expr)) {
            visitExpr(un->operand.get());
        } else if (auto member = dynamic_cast<const MemberAccess*>(expr)) {
            visitExpr(member->object.get());
        } else if (auto sub = dynamic_cast<const SubscriptExpression*>(expr)) {
            usesBoundsCheck_ = true;   // some `x[i]` exists; emitExpression decides per site
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
            // The target too: a program whose only subscripts are stores still needs the bounds-check helper.
            visitExpr(assign->target.get());
        } else if (auto ret = dynamic_cast<const ReturnStatement*>(stmt)) {
            visitExpr(ret->expression.get());
        } else if (auto th = dynamic_cast<const ThrowStatement*>(stmt)) {
            visitExpr(th->expression.get());
        } else if (auto ifs = dynamic_cast<const IfStatement*>(stmt)) {
            visitExpr(ifs->condition.get());
            for (const auto& inner : ifs->body) visitStmt(inner.get());
            for (const auto& inner : ifs->elseBody) visitStmt(inner.get());
        } else if (auto whiles = dynamic_cast<const WhileStatement*>(stmt)) {
            visitExpr(whiles->condition.get());
            for (const auto& inner : whiles->body) visitStmt(inner.get());
        } else if (auto fors = dynamic_cast<const ForStatement*>(stmt)) {
            visitExpr(fors->container.get());
            for (const auto& inner : fors->body) visitStmt(inner.get());
        } else if (auto trys = dynamic_cast<const TryStatement*>(stmt)) {
            for (const auto& inner : trys->body) visitStmt(inner.get());
            for (const auto& inner : trys->catchBody) visitStmt(inner.get());
        }
    };

    std::function<void(const std::vector<StatementPtr>&)> visitBody =
        [&](const std::vector<StatementPtr>& body) {
            for (const auto& stmt : body) visitStmt(stmt.get());
        };

    for (const auto& stmt : program.statements) {
        if (auto fn = dynamic_cast<const FunctionDeclaration*>(stmt.get())) {
            visitBody(fn->body);
        } else if (auto mn = dynamic_cast<const MainFunction*>(stmt.get())) {
            visitBody(mn->body);
        } else if (auto ctor = dynamic_cast<const ConstructorDeclaration*>(stmt.get())) {
            visitBody(ctor->body);
        } else if (auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
            for (const auto& member : cls->members) {
                if (auto method = dynamic_cast<const MethodDeclaration*>(member.get())) {
                    visitBody(method->body);
                } else if (auto ctor2 = dynamic_cast<const ConstructorDeclaration*>(member.get())) {
                    visitBody(ctor2->body);
                }
            }
        }
    }
}

std::string AstEmitter::calleeParamType(const FunctionCall& call, size_t argIndex) const {
    const std::vector<std::pair<std::string, std::string>>* params = nullptr;
    if (call.callee) {
        auto member = dynamic_cast<const MemberAccess*>(call.callee.get());
        if (member == nullptr) return {};
        const std::string objClass = exprObjectClass(member->object.get());
        if (objClass.empty()) return {};
        auto clsIt = classMethodParams_.find(objClass);
        if (clsIt == classMethodParams_.end()) return {};
        auto paramIt = clsIt->second.find(call.templateArg.empty() ? member->member : specializationKey(member->member, call.templateArg));
        if (paramIt == clsIt->second.end() && call.templateArg.empty()) paramIt = clsIt->second.find(overloadKey(member->member, call.arguments.size()));
        if (paramIt == clsIt->second.end()) return {};
        params = &paramIt->second;
    } else {
        auto paramIt = functionParams_.find(call.function_name);
        if (paramIt == functionParams_.end()) {
            // an unqualified call of a method of the CURRENT class (implicit `this`)
            auto clsIt = currentClass_.empty() ? classMethodParams_.end() : classMethodParams_.find(currentClass_);
            if (clsIt == classMethodParams_.end()) return {};
            auto mIt = clsIt->second.find(call.function_name);
            if (mIt == clsIt->second.end()) return {};
            params = &mIt->second;
        } else {
            params = &paramIt->second;
        }
    }
    return argIndex < params->size() ? (*params)[argIndex].first : std::string{};
}

bool AstEmitter::isViewExpression(const Expression* e) const {
    if (auto id = dynamic_cast<const Identifier*>(e)) return viewVars_.count(id->name) > 0;
    if (auto call = dynamic_cast<const FunctionCall*>(e)) {
        auto member = call->callee ? dynamic_cast<const MemberAccess*>(call->callee.get()) : nullptr;
        auto recv = member ? dynamic_cast<const Identifier*>(member->object.get()) : nullptr;
        return recv != nullptr && viewVars_.count(recv->name) > 0 && stringViewMethodReturnsView(member->member);
    }
    return false;
}

std::string AstEmitter::emitAsString(const Expression* e) {
    return isViewExpression(e) ? "std::string(" + emitExpression(e) + ")" : emitExpression(e);
}

std::pair<std::string, std::string> AstEmitter::calleeParam(const FunctionCall& call, size_t argIndex) const {
    const std::vector<std::pair<std::string, std::string>>* params = nullptr;
    if (call.callee) {
        // `recv.method(args)`.
        auto member = dynamic_cast<const MemberAccess*>(call.callee.get());
        if (member == nullptr) return {};
        const std::string objClass = exprObjectClass(member->object.get());
        if (objClass.empty()) return {};
        auto clsIt = classMethodParams_.find(objClass);
        if (clsIt == classMethodParams_.end()) return {};
        std::string methodKey = call.templateArg.empty()
            ? member->member
            : specializationKey(member->member, call.templateArg);
        auto paramIt = clsIt->second.find(methodKey);
        if (paramIt == clsIt->second.end() && call.templateArg.empty()) {
            // The bare name may be registered under an overload key; retry with the call's argument count.
            methodKey = overloadKey(member->member, call.arguments.size());
            paramIt = clsIt->second.find(methodKey);
        }
        if (paramIt == clsIt->second.end()) return {};
        params = &paramIt->second;
    } else {
        auto paramIt = functionParams_.find(call.function_name);
        if (paramIt == functionParams_.end()) {
            // an unqualified call of a method of the CURRENT class (implicit `this`)
            auto clsIt = currentClass_.empty() ? classMethodParams_.end() : classMethodParams_.find(currentClass_);
            if (clsIt == classMethodParams_.end()) return {};
            auto mIt = clsIt->second.find(call.function_name);
            if (mIt == clsIt->second.end()) return {};
            params = &mIt->second;
        } else {
            params = &paramIt->second;
        }
    }
    if (argIndex >= params->size()) return {};
    const auto& [ptype, pname] = (*params)[argIndex];
    // (a `span` parameter is returned too: emitMovedArg then passes a vector argument as is instead of moving it)
    return isClassType(ptype) || isSpanType(ptype) ? std::make_pair(ptype, pname) : std::pair<std::string, std::string>{};
}

// Must the string argument at `argIndex` be copied (rather than bound by const&) at this call? Only if it
// is a live alias the callee could reach: rooted in a field of this object, and the callee may rewrite that
// field (or free the objects it lives in).
bool AstEmitter::stringArgCopyType(const FunctionCall& call, size_t argIndex, const std::string& callee) const {
    const Expression* sarg = call.arguments[argIndex].get();
    const Expression* root = sarg;
    std::string fieldName;
    bool sawMember = false;
    while (true) {
        if (auto m = dynamic_cast<const MemberAccess*>(root)) {
            if (!sawMember) { fieldName = m->member; sawMember = true; }
            root = m->object.get();
        } else if (auto sub = dynamic_cast<const SubscriptExpression*>(root)) {
            root = sub->object.get();
        } else {
            break;
        }
    }
    bool rootIsField = false;
    if (auto id = dynamic_cast<const Identifier*>(root)) {
        rootIsField = isCurrentField(id->name);
        if (!sawMember) fieldName = id->name;
        if (!sawMember && !rootIsField) return false;  // a plain local or parameter: unreachable by the callee
    } else if (dynamic_cast<const ThisExpression*>(root) != nullptr) {
        rootIsField = true;
    } else {
        return false;  // a literal, or rooted in a call result: not a live alias of anything
    }
    // An argument inside a local's object can still be rewritten through another handle to it: check the field and the objects.
    if (analyzer_.calleeMayMutateField(callee, fieldName)) return true;
    if (analyzer_.calleeMayMutateObjects(callee)) return true;
    return false;
}

std::string AstEmitter::argNeedsExplicitCopyForReadOnlyContainerParam(const FunctionCall& call, size_t argIndex,
                                                                       const std::string& receiverClass,
                                                                       const std::string& methodName) const {
    // Resolve the parameter's declared type at argIndex, as calleeParam does (which returns class types only).
    const std::vector<std::pair<std::string, std::string>>* params = nullptr;
    if (receiverClass.empty() && call.callee != nullptr && !methodName.empty()) {
        // A method call whose receiver's class could not be resolved: the callee might take a `const&`
        // string at this position. Decide from the name alone, conservatively.
        if (analyzer_.anyReadOnlyStringParamAt(methodName, argIndex) && stringArgCopyType(call, argIndex, methodName)) {
            return "std::string";
        }
        return "";
    }
    if (!receiverClass.empty()) {
        auto clsIt = classMethodParams_.find(receiverClass);
        if (clsIt == classMethodParams_.end()) return "";
        auto paramIt = clsIt->second.find(methodName);
        if (paramIt == clsIt->second.end()) {
            paramIt = clsIt->second.find(overloadKey(methodName, call.arguments.size()));
        }
        if (paramIt == clsIt->second.end()) return "";
        params = &paramIt->second;
    } else {
        auto paramIt = functionParams_.find(call.function_name);
        if (paramIt == functionParams_.end()) return "";
        params = &paramIt->second;
    }
    if (argIndex >= params->size()) return "";
    const auto& [ptype, pname] = (*params)[argIndex];
    if (isStringType(ptype)) {
        if (!analyzer_.isReadOnlyStringParam(receiverClass, receiverClass.empty() ? call.function_name : methodName, pname)) {
            return "";
        }
        return stringArgCopyType(call, argIndex, receiverClass.empty() ? call.function_name : methodName) ? ptype : std::string();
    }
    if (!isVectorType(ptype)) return "";
    const bool isReadOnlyContainer = receiverClass.empty()
        ? analyzer_.isReadOnlyContainerParam(call.function_name, pname)
        : analyzer_.isReadOnlyMethodContainerParam(receiverClass, methodName, pname);
    if (!isReadOnlyContainer) return "";

    // A caller local or by-value parameter cannot be reached by the callee mid-call, so a reference is always safe.
    const Expression* arg = call.arguments[argIndex].get();
    if (auto id = dynamic_cast<const Identifier*>(arg)) {
        if (!isCurrentField(id->name)) return "";  // local/param -- reference is safe
    } else if (dynamic_cast<const ThisExpression*>(arg) == nullptr &&
               dynamic_cast<const MemberAccess*>(arg) == nullptr) {
        // Not a field/this/member chain (call result, literal): not a live alias, no copy needed.
        return "";
    }

    // A field access: pass by reference only if the callee is heap-pure.
    const bool calleeIsPure = receiverClass.empty() ? analyzer_.isHeapPureFreeFunction(call.function_name)
                                                     : analyzer_.isHeapPureMethod(receiverClass, methodName);
    if (calleeIsPure) return "";

    return ptype;  // needs an explicit copy -- return the type to construct it from
}

bool AstEmitter::isCurrentField(const std::string& name) const {
    if (currentClass_.empty()) return false;
    // A parameter of the same name hides the field, as in C++ and Java.
    if (currentParamNames_.count(name) > 0) return false;
    auto it = classFields_.find(currentClass_);
    if (it == classFields_.end()) return false;
    return it->second.count(name) > 0;
}

std::string AstEmitter::fieldType(const std::string& cls, const std::string& name) const {
    auto ci = classFields_.find(cls);
    if (ci == classFields_.end()) return "";
    auto fi = ci->second.find(name);
    return fi == ci->second.end() ? "" : fi->second;
}

bool AstEmitter::isStreamChain(const Expression* expr) const {
    // Walk down the left side of nested `<<` (`a << b << c` is `((a << b) << c)`). Only `<<` is followed, so `(x + y) << 2` stays a shift.
    const Expression* leaf = expr;
    while (auto bin = dynamic_cast<const BinaryExpression*>(leaf)) {
        if (bin->operator_str != "<<") break;
        leaf = bin->left.get();
    }
    // `std::cout`/`std::cerr`/`std::clog` -- a ScopedIdentifier, emitted verbatim.
    if (auto scoped = dynamic_cast<const ScopedIdentifier*>(leaf)) {
        return scoped->qualifiedName == "std::cout" ||
               scoped->qualifiedName == "std::cerr" ||
               scoped->qualifiedName == "std::clog";
    }
    // An `ostream`-typed local or parameter.
    if (auto id = dynamic_cast<const Identifier*>(leaf)) {
        return ostreamVars_.count(id->name) > 0;
    }
    return false;
}

std::string AstEmitter::exprObjectClass(const Expression* expr) const {
    if (dynamic_cast<const ThisExpression*>(expr) != nullptr) {
        // An explicit `this` points to the current object and needs `->` like any other handle.
        return currentClass_.empty() ? "" : currentClass_;
    }
    if (auto id = dynamic_cast<const Identifier*>(expr)) {
        // A field of the current class (accessed via this->) that is object-typed.
        if (isCurrentField(id->name)) {
            const std::string ft = fieldType(currentClass_, id->name);
            return isClassType(ft) ? ft : "";
        }
        // A local or parameter classified Value is never `->`-accessed, whatever varClass_ records (it holds only the declared type name).
        if (kindOf(id->name) == VarKind::Value) {
            return "";
        }
        // A smart-pointer local/parameter.
        if (auto it = varClass_.find(id->name); it != varClass_.end()) {
            return it->second;
        }
        // A `ref` observer (raw-pointer lowering -- also needs `->`).
        if (auto it = observerClass_.find(id->name); it != observerClass_.end()) {
            return it->second;
        }
        return "";
    }
    if (auto member = dynamic_cast<const MemberAccess*>(expr)) {
        // The class of `a.b` is the declared type of field b in a's class.
        const std::string objClass = exprObjectClass(member->object.get());
        if (objClass.empty()) return "";
        const std::string ft = fieldType(objClass, member->member);
        return isClassType(ft) ? ft : "";
    }
    if (auto sub = dynamic_cast<const SubscriptExpression*>(expr)) {
        // `v[i]` of class-typed elements is a smart pointer, so `v[i].field` needs `->`.
        const std::string containerType = containerVarType(sub->object.get());
        if (containerType.empty()) return "";
        const std::string elem = vectorElementType(containerType);
        return isClassType(elem) ? elem : "";
    }
    if (auto call = dynamic_cast<const FunctionCall*>(expr)) {
        // A call result: decide from the call's declared return type.
        if (call->callee) {
            auto member = dynamic_cast<const MemberAccess*>(call->callee.get());
            if (member == nullptr) return "";
            const std::string objClass = exprObjectClass(member->object.get());
            if (objClass.empty()) return "";
            auto clsIt = classMethodReturnType_.find(objClass);
            if (clsIt == classMethodReturnType_.end()) return "";
            std::string methodKey = call->templateArg.empty()
                ? member->member
                : specializationKey(member->member, call->templateArg);
            auto retIt = clsIt->second.find(methodKey);
            if (retIt == clsIt->second.end() && call->templateArg.empty()) {
                methodKey = overloadKey(member->member, call->arguments.size());
                retIt = clsIt->second.find(methodKey);
            }
            if (retIt == clsIt->second.end()) return "";
            return isClassType(retIt->second) ? retIt->second : "";
        }
        // Bare call: a free function, or an implicit-`this` call of a method of the same class.
        if (!currentClass_.empty()) {
            auto clsIt = classMethodReturnType_.find(currentClass_);
            if (clsIt != classMethodReturnType_.end()) {
                std::string methodKey = call->templateArg.empty()
                    ? call->function_name
                    : specializationKey(call->function_name, call->templateArg);
                auto retIt = clsIt->second.find(methodKey);
                if (retIt == clsIt->second.end() && call->templateArg.empty()) {
                    methodKey = overloadKey(call->function_name, call->arguments.size());
                    retIt = clsIt->second.find(methodKey);
                }
                if (retIt != clsIt->second.end()) {
                    return isClassType(retIt->second) ? retIt->second : "";
                }
            }
        }
        auto it = functionReturnType_.find(call->function_name);
        if (it == functionReturnType_.end()) return "";
        return isClassType(it->second) ? it->second : "";
    }
    return "";
}

bool AstEmitter::isClassType(const std::string& type) const {
    return classNames_.find(type) != classNames_.end();
}

bool AstEmitter::isValueType(const std::string& type) const {
    // Set only while emitting an external specialization whose return type is its own specialized type: that return and the locals
    // of that type must be unwrapped. Cleared when the function is done.
    if (!currentSpecializationValueType_.empty() && type == currentSpecializationValueType_) {
        return true;
    }
    return analyzer_.isValueTypeClass(type);
}

// A base class needs a callable no-arg constructor for a derived class's implicit base init (there is no base-initializer syntax).
// Synthesize `T() = default;` only when the user wrote no zero-argument constructor, since a second one is a duplicate.
bool AstEmitter::hasUserDefinedDefaultConstructor(const ClassDeclaration& cls) const {
    for (const auto& member : cls.members) {
        if (auto ctor = dynamic_cast<const ConstructorDeclaration*>(member.get())) {
            if (ctor->parameters.empty()) return true;
        }
    }
    return false;
}

std::string AstEmitter::containerVarType(const Expression* expr) const {
    if (auto id = dynamic_cast<const Identifier*>(expr)) {
        auto it = containerVarType_.find(id->name);
        if (it != containerVarType_.end()) return it->second;
        // containerVarType_ holds locals and parameters only; a container field used bare (implicit `this`) is looked up in the class's fields.
        if (isCurrentField(id->name)) {
            const std::string ft = fieldType(currentClass_, id->name);
            if (isVectorType(ft) || isChannelType(ft)) return ft;
        }
        return "";
    }
    if (auto member = dynamic_cast<const MemberAccess*>(expr)) {
        // A container field reached through an explicit receiver (`scanner.out[pos]`): its element is a smart pointer, so `.kind` needs `->`.
        const std::string objClass = exprObjectClass(member->object.get());
        if (objClass.empty()) return "";
        const std::string ft = fieldType(objClass, member->member);
        if (isVectorType(ft) || isChannelType(ft)) return ft;
        return "";
    }
    return "";
}

bool AstEmitter::isSharedReturningCall(const Expression* expr) const {
    if (auto call = dynamic_cast<const FunctionCall*>(expr)) {
        if (!call->templateArg.empty()) return false;
        if (call->callee) {
            auto member = dynamic_cast<const MemberAccess*>(call->callee.get());
            if (member == nullptr) return false;
            const std::string objClass = exprObjectClass(member->object.get());
            if (objClass.empty()) return false;
            return analyzer_.getMethodReturnPointerType(objClass, member->member) == PointerType::SHARED;
        }
        // Plain call: a free function (queried under the "" class) or an implicit-`this` method of the same class.
        if (analyzer_.getMethodReturnPointerType("", call->function_name) == PointerType::SHARED) {
            return true;
        }
        if (!currentClass_.empty() &&
            analyzer_.getMethodReturnPointerType(currentClass_, call->function_name) == PointerType::SHARED) {
            return true;
        }
        return false;
    }
    if (auto sub = dynamic_cast<const SubscriptExpression*>(expr)) {
        // `n[key]` on a class-typed receiver is emitted as `operator[]`, so the same check applies under that name.
        const std::string objClass = exprObjectClass(sub->object.get());
        if (objClass.empty()) return false;
        return analyzer_.getMethodReturnPointerType(objClass, "operator[]") == PointerType::SHARED;
    }
    return false;
}

// Is `expr` a call to a specialization whose return type is its own specialized type (`doc.as<Point>()`)? Its C++ result is a bare
// value, so a local declared from it is emitted bare too.
bool AstEmitter::isBareValueReturningSpecializationCall(const Expression* expr) const {
    auto call = dynamic_cast<const FunctionCall*>(expr);
    if (call == nullptr || call->templateArg.empty()) return false;
    std::string receiverClass;
    if (call->callee) {
        auto member = dynamic_cast<const MemberAccess*>(call->callee.get());
        if (member == nullptr) return false;
        receiverClass = exprObjectClass(member->object.get());
    } else {
        receiverClass = currentClass_;
    }
    if (receiverClass.empty()) return false;
    auto clsIt = classMethodReturnType_.find(receiverClass);
    if (clsIt == classMethodReturnType_.end()) return false;
    const std::string key = specializationKey(
        call->callee ? dynamic_cast<const MemberAccess*>(call->callee.get())->member : call->function_name,
        call->templateArg);
    auto retIt = clsIt->second.find(key);
    return retIt != clsIt->second.end() && retIt->second == call->templateArg;
}

std::string AstEmitter::cppTypeFor(const std::string& ceelingType, const std::string& varName) const {
    if (isStringType(ceelingType)) return "std::string";
    if (isStringViewType(ceelingType)) return "std::string_view";
    if (isSpanType(ceelingType)) return std::string("cling::span<") + (isSpanConstType(ceelingType) ? "const " : "") + cppTypeFor(spanElementType(ceelingType), "") + ">";
    if (isOptionalType(ceelingType)) return "std::optional<" + cppTypeFor(optionalElementType(ceelingType), "") + ">";
    // `byte` and `int64` are the primitives whose C++ spelling differs from the CeeLing one.
    if (ceelingType == "byte") return "unsigned char";
    if (ceelingType == "int64") return "long long";
    if (isMapType(ceelingType)) {
        // Map keys and values are primitives (checked at parse time), so only the element mapping applies.
        return "std::map<" + cppTypeFor(mapKeyType(ceelingType), "")
             + ", " + cppTypeFor(mapValueType(ceelingType), "") + ">";
    }
    if (isChannelType(ceelingType)) return "cling::Channel<" + cppTypeFor(channelElementType(ceelingType), "") + ">";
    const std::string elem = vectorElementType(ceelingType);
    if (elem.empty()) return ceelingType;
    // A value-type element (`struct`) is stored bare so the vector is contiguous.
    if (analyzer_.isValueTypeClass(elem)) {
        return "std::vector<" + elem + ">";
    }
    if (isClassType(elem)) {
        const std::string ptr = analyzer_.containerElementIsShared(varName)
            ? "std::shared_ptr<" : "std::unique_ptr<";
        return "std::vector<" + ptr + elem + ">>";
    }
    // Non-class element: recurse, so `vector<string>` becomes `std::vector<std::string>`.
    return "std::vector<" + cppTypeFor(elem, "") + ">";
}

AstEmitter::VarKind AstEmitter::kindOf(const std::string& name) const {
    auto it = varKinds_.find(name);
    return it == varKinds_.end() ? VarKind::Value : it->second;
}

std::string AstEmitter::checkedDeref(const Expression* obj, const std::string& text, int line) const {
    if (!nullChecks_) return text;
    if (dynamic_cast<const ThisExpression*>(obj) != nullptr) return text;
    return "cling::nn(" + text + ", \"line " + std::to_string(line) + "\")";
}

std::string AstEmitter::sharedConstruct(const std::string& cls, const std::string& args) const {
    if (!poolShared_) return "std::make_shared<" + cls + ">(" + args + ")";
    return "std::allocate_shared<" + cls + ">(cling::PoolAllocator<" + cls + ">()" +
           (args.empty() ? std::string() : ", " + args) + ")";
}

std::string AstEmitter::scopeKeyFor(const std::string& ownerClass, const std::string& name,
                                    const std::string& methodName) {
    std::string bare = methodName.empty() ? name : methodName;
    if (ownerClass.empty()) return bare;
    const auto pos = bare.rfind("::");
    if (pos != std::string::npos) bare = bare.substr(pos + 2);
    return ownerClass + "::" + bare;
}

std::string AstEmitter::smartPointerType(const std::string& className,
                                         const std::string& varName) const {
    // `className` goes to the analyzer verbatim: a namespaced local's pointer kind is registered under the spelling of its declared type.
    switch (analyzer_.getPointerTypeInScope(currentScope_, className, varName)) {
        case PointerType::UNIQUE: return "std::unique_ptr";
        case PointerType::WEAK:   return "std::weak_ptr";
        default:                  return "std::shared_ptr";
    }
}

std::string AstEmitter::factoryFor(const std::string& className, const std::string& varName,
                                   const std::vector<ExpressionPtr>& args,
                                   const std::string& declaredType) {
    // With inheritance the constructed type and the declared type differ (`Animal a = new Dog();`): construct the first, choose the pointer kind from the second.
    const std::string kindType = declaredType.empty() ? className : declaredType;
    const std::string ptr = smartPointerType(kindType, varName);
    std::string argText;
    // A spawn job's object captures are moved into the job; the checker marked the creator's local moved-from.
    auto jobIt = classByName_.find(className);
    const bool isSpawnJob = jobIt != classByName_.end() && jobIt->second->isSpawnJob;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) argText += ", ";
        argText += isSpawnJob ? emitSpawnJobArg(args[i].get()) : emitExpression(args[i].get());
    }
    if (ptr == "std::unique_ptr") return "std::make_unique<" + className + ">(" + argText + ")";
    return sharedConstruct(className, argText);
}

// ---- top level -----------------------------------------------------------

void AstEmitter::emitTopLevel(const Statement* stmt, std::ostringstream& out) {
    // Declarations merged from an included .clg are not re-emitted; their definition is in that file's own output.
    if (stmt->isExternalDeclaration) return;
    if (auto marker = dynamic_cast<const ExternalSpecializationMarker*>(stmt)) {
        if (auto it = classByName_.find(marker->targetClassName); it != classByName_.end()) {
            emitExternalSpecializationMarker(*marker, *it->second, out);
        }
        // An unresolvable target is rejected during AST building.
        return;
    }
    if (auto cls = dynamic_cast<const ClassDeclaration*>(stmt)) {
        emitClass(*cls, out);
    } else if (auto enm = dynamic_cast<const EnumDeclaration*>(stmt)) {
        // Unscoped enum: enumerators convert implicitly to int.
        out << "enum " << enm->name << " { ";
        for (size_t i = 0; i < enm->enumerators.size(); ++i) {
            if (i > 0) out << ", ";
            out << enm->enumerators[i];
        }
        out << " };\n\n";
    } else if (auto fn = dynamic_cast<const FunctionDeclaration*>(stmt)) {
        // methodName keys the return-pointer-kind query; without it a free function returning an alias stays unique_ptr.
        emitFunction(fn->returnType, fn->name, fn->parameters, fn->body, out,
                     "", 0, false, false, false, fn->name);
    } else if (auto extFn = dynamic_cast<const ExternFunctionDeclaration*>(stmt)) {
        // ext_fn emits only a forward declaration; the implementation is hand-written C++ the user links in.
        varKinds_.clear(); varClass_.clear(); observerClass_.clear(); containerVarType_.clear(); viewVars_.clear();
        currentExternRefParams_ = extFn->refParams;
        std::string sig = buildFunctionSignature(extFn->returnType, extFn->name,
                                                  extFn->parameters, "", false, false, false, extFn->name);
        currentExternRefParams_.clear();
        out << sig << ";\n\n";
    } else if (auto mn = dynamic_cast<const MainFunction*>(stmt)) {
        emitFunction(mn->returnType, "main", {}, mn->body, out);
    } else if (dynamic_cast<const IncludeStatement*>(stmt)) {
        // Includes are collected once at the top of emit().
    } else if (dynamic_cast<const CommentStatement*>(stmt)) {
        // Comments are dropped in the AST-driven output for now.
    }
}

std::string AstEmitter::perClassGuardName(const std::string& className) {
    std::string guard = className;
    for (char& c : guard) {
        c = (std::isalnum(static_cast<unsigned char>(c)) ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : '_');
    }
    return "CEELING_DECL_" + guard;
}

std::vector<std::string> AstEmitter::selfOwningFields(const ClassDeclaration& cls) const {
    std::vector<std::string> fields;
    if (!cls.baseClass.empty() || baseClasses_.count(cls.name) > 0 || isValueType(cls.name)) {
        return fields;
    }
    for (const auto& member : cls.members) {
        auto var = dynamic_cast<const VariableDeclaration*>(member.get());
        if (var == nullptr || var->type != cls.name) continue;
        if (smartPointerType(var->type, var->identifier) == "std::weak_ptr") continue;
        fields.push_back(var->identifier);
    }
    return fields;
}

std::string AstEmitter::selfOwningDestructorBody(const ClassDeclaration& cls,
                                                 const std::vector<std::string>& fields) const {
    // Detach the owned links onto a heap worklist and free them one at a time. A node that is
    // still referenced elsewhere (use_count > 1) is only released, never taken apart -- its other
    // owner keeps its links. Each freed node re-enters this destructor with empty links and
    // returns at the first line.
    const std::string T = cls.name;
    std::ostringstream b;
    b << "    if (";
    for (size_t i = 0; i < fields.size(); ++i) {
        if (i > 0) b << " && ";
        b << "!" << fields[i];
    }
    b << ") return;\n";
    b << "    std::vector<std::shared_ptr<" << T << ">> pending;\n";
    for (const auto& f : fields) {
        b << "    if (" << f << ") pending.push_back(std::move(" << f << "));\n";
    }
    b << "    while (!pending.empty()) {\n";
    b << "        std::shared_ptr<" << T << "> node = std::move(pending.back());\n";
    b << "        pending.pop_back();\n";
    b << "        if (node.use_count() != 1) continue;\n";
    for (const auto& f : fields) {
        b << "        if (node->" << f << ") pending.push_back(std::move(node->" << f << "));\n";
    }
    b << "    }\n";
    return b.str();
}

void AstEmitter::emitClass(const ClassDeclaration& cls, std::ostringstream& out) {
    const std::string guard = perClassGuardName(cls.name);
    out << "#ifndef " << guard << "\n#define " << guard << "\n";
    out << "class " << cls.name;
    // enable_shared_from_this is a compiler-generated base, added when a method returns `this` as shared; it is comma-joined with the user's base.
    std::vector<std::string> bases;
    if (!cls.baseClass.empty()) bases.push_back("public " + cls.baseClass);
    if (analyzer_.classNeedsSharedFromThis(cls.name)) {
        bases.push_back("public std::enable_shared_from_this<" + cls.name + ">");
    }
    if (!bases.empty()) {
        out << " : ";
        for (size_t i = 0; i < bases.size(); ++i) {
            if (i > 0) out << ", ";
            out << bases[i];
        }
    }
    out << " {\npublic:\n";

    // A class used as a base gets a virtual destructor (deleting a Derived through unique_ptr<Base> is undefined otherwise) and a protected
    // no-arg constructor for derived constructors to chain through (any user constructor suppresses the implicit one). The constructor is
    // skipped when the user wrote a zero-argument one.
    if (baseClasses_.count(cls.name) > 0) {
        out << indentStr(1) << "virtual ~" << cls.name << "() = default;\n";
        if (!hasUserDefinedDefaultConstructor(cls)) {
            out << "protected:\n";
            out << indentStr(1) << cls.name << "() = default;\n";
            out << "public:\n";
        }
    }

    // Constructors and methods are declared here and defined out of line after the class, as in the paired header: an in-class definition
    // is inline and may be dropped from this object file, breaking other translation units at link time. The primary generic template
    // stays in-class because its definition must be visible at every instantiation.
    std::vector<const ConstructorDeclaration*> outOfLineCtors;
    std::vector<const MethodDeclaration*> outOfLineMethods;
    std::vector<const MethodDeclaration*> specializations;

    for (const auto& member : cls.members) {
        if (auto var = dynamic_cast<const VariableDeclaration*>(member.get())) {
            // Member field: a value-type class stays unwrapped, other object types become smart pointers.
            if (isValueType(var->type)) {
                out << indentStr(1) << var->type << " " << var->identifier << ";\n";
            } else if (isClassType(var->type)) {
                out << indentStr(1) << smartPointerType(var->type, var->identifier)
                    << "<" << var->type << "> " << var->identifier << ";\n";
            } else {
                // A number, bool or char field starts at zero (`{}`), as in Java.
                const bool zeroed = isPrimitiveType(var->type) && var->type != "string" && var->type != "std::string";
                out << indentStr(1) << cppTypeFor(var->type, var->identifier) << " "
                    << var->identifier << (zeroed ? "{}" : "") << ";\n";
            }
        } else if (auto ctor = dynamic_cast<const ConstructorDeclaration*>(member.get())) {
            varKinds_.clear(); varClass_.clear(); observerClass_.clear(); containerVarType_.clear(); viewVars_.clear();
            std::string sig = buildFunctionSignature("", ctor->name, ctor->parameters, cls.name,
                                                      false, false, false, "");
            out << indentStr(1) << sig << ";\n";
            outOfLineCtors.push_back(ctor);
        } else if (auto method = dynamic_cast<const MethodDeclaration*>(member.get())) {
            if (!method->specializationArg.empty()) {
                if (method->isExternalSpecialization) {
                    // An external specialization is emitted at its marker's position in the source (emitExternalSpecializationMarker),
                    // which is after every type it can reference.
                } else {
                    // An in-class specialization can only use types the class already needs.
                    specializations.push_back(method);
                }
                continue;
            }
            if (!method->typeParameter.empty()) {
                // Must stay visible at every instantiation point.
                out << indentStr(1) << "template<typename " << method->typeParameter << ">\n";
                emitFunction(method->returnType, method->name, method->parameters, method->body,
                             out, cls.name, 1, method->isConst, method->isVirtual, method->isOverride,
                             method->name);
                continue;
            }
            varKinds_.clear(); varClass_.clear(); observerClass_.clear(); containerVarType_.clear(); viewVars_.clear();
            std::string sig = buildFunctionSignature(method->returnType, method->name, method->parameters,
                                                      cls.name, method->isConst, method->isVirtual,
                                                      method->isOverride, method->name);
            out << indentStr(1) << sig << ";\n";
            outOfLineMethods.push_back(method);
        }
    }
    const std::vector<std::string> selfOwning = selfOwningFields(cls);
    if (!selfOwning.empty()) out << indentStr(1) << "~" << cls.name << "();\n";
    out << "};\n#endif // " << guard << "\n\n";

    // Out-of-line definitions: written here, or stashed for the second phase when deferClassBodies_ is set (see emit()).
    std::ostringstream bodies;
    for (const auto* ctor : outOfLineCtors) {
        emitFunction("", cls.name + "::" + ctor->name, ctor->parameters, ctor->body, bodies, cls.name, 0,
                     false, false, false, "", /*isConstructor=*/true);
    }
    for (const auto* method : outOfLineMethods) {
        // virtual and override are class-body-only specifiers; they are not repeated out of line.
        emitFunction(method->returnType, cls.name + "::" + method->name, method->parameters,
                     method->body, bodies, cls.name, 0, method->isConst, false, false, method->name);
    }
    if (!selfOwning.empty()) {
        bodies << cls.name << "::~" << cls.name << "() {\n" << selfOwningDestructorBody(cls, selfOwning) << "}\n\n";
    }
    // Specializations are defined here (one definition per program); the paired header only declares them.
    for (const auto* method : specializations) {
        bodies << "template<>\n";
        // The `<int>` in the name must use the C++ spelling (cppTypeFor), or the specialization does not bind to its call sites.
        emitFunction(method->returnType,
                     cls.name + "::" + method->name + "<" + cppTypeFor(method->specializationArg, "") + ">",
                     method->parameters, method->body, bodies, cls.name, 0, method->isConst);
    }
    if (deferClassBodies_) {
        deferredClassBodies_[cls.name] = bodies.str();
    } else {
        out << bodies.str();
    }
}

void AstEmitter::emitClassHeader(const ClassDeclaration& cls, std::ostringstream& out) {
    const std::string guard = perClassGuardName(cls.name);
    out << "#ifndef " << guard << "\n#define " << guard << "\n";
    out << "class " << cls.name;
    std::vector<std::string> bases;
    if (!cls.baseClass.empty()) bases.push_back("public " + cls.baseClass);
    if (analyzer_.classNeedsSharedFromThis(cls.name)) {
        bases.push_back("public std::enable_shared_from_this<" + cls.name + ">");
    }
    if (!bases.empty()) {
        out << " : ";
        for (size_t i = 0; i < bases.size(); ++i) {
            if (i > 0) out << ", ";
            out << bases[i];
        }
    }
    out << " {\npublic:\n";

    if (baseClasses_.count(cls.name) > 0) {
        out << indentStr(1) << "virtual ~" << cls.name << "() = default;\n";
        if (!hasUserDefinedDefaultConstructor(cls)) {
            out << "protected:\n";
            out << indentStr(1) << cls.name << "() = default;\n";
            out << "public:\n";
        }
    }

    // Specializations are declared here without a body; the one definition lives in the paired .cpp.
    std::vector<const MethodDeclaration*> specializations;

    for (const auto& member : cls.members) {
        if (auto var = dynamic_cast<const VariableDeclaration*>(member.get())) {
            if (isValueType(var->type)) {
                out << indentStr(1) << var->type << " " << var->identifier << ";\n";
            } else if (isClassType(var->type)) {
                out << indentStr(1) << smartPointerType(var->type, var->identifier)
                    << "<" << var->type << "> " << var->identifier << ";\n";
            } else {
                // A number, bool or char field starts at zero (`{}`), as in Java.
                const bool zeroed = isPrimitiveType(var->type) && var->type != "string" && var->type != "std::string";
                out << indentStr(1) << cppTypeFor(var->type, var->identifier) << " "
                    << var->identifier << (zeroed ? "{}" : "") << ";\n";
            }
        } else if (auto ctor = dynamic_cast<const ConstructorDeclaration*>(member.get())) {
            varKinds_.clear(); varClass_.clear(); observerClass_.clear(); containerVarType_.clear(); viewVars_.clear();
            std::string sig = buildFunctionSignature("", ctor->name, ctor->parameters, cls.name,
                                                      false, false, false, "");
            out << indentStr(1) << sig << ";\n";
        } else if (auto method = dynamic_cast<const MethodDeclaration*>(member.get())) {
            if (!method->specializationArg.empty()) {
                specializations.push_back(method);
                continue;
            }
            if (!method->typeParameter.empty()) {
                // The primary generic template's body stays visible in the header, as any function template's must.
                out << indentStr(1) << "template<typename " << method->typeParameter << ">\n";
                emitFunction(method->returnType, method->name, method->parameters, method->body,
                             out, cls.name, 1, method->isConst, method->isVirtual, method->isOverride,
                             method->name);
                continue;
            }
            varKinds_.clear(); varClass_.clear(); observerClass_.clear(); containerVarType_.clear(); viewVars_.clear();
            std::string sig = buildFunctionSignature(method->returnType, method->name, method->parameters,
                                                      cls.name, method->isConst, method->isVirtual,
                                                      method->isOverride, method->name);
            out << indentStr(1) << sig << ";\n";
        }
    }
    if (!selfOwningFields(cls).empty()) out << indentStr(1) << "~" << cls.name << "();\n";
    out << "};\n#endif // " << guard << "\n\n";

    for (const auto* method : specializations) {
        // Declaration only, at namespace scope with the qualified, specialized name.
        varKinds_.clear(); varClass_.clear(); observerClass_.clear(); containerVarType_.clear(); viewVars_.clear();
        std::string sig = buildFunctionSignature(
            method->returnType,
            cls.name + "::" + method->name + "<" + cppTypeFor(method->specializationArg, "") + ">",
            method->parameters, cls.name, method->isConst, false, false, "");
        out << "template<>\n" << sig << ";\n\n";
    }
}

void AstEmitter::emitTopLevelHeader(const Statement* stmt, std::ostringstream& out) {
    if (stmt->isExternalDeclaration) return;
    if (auto cls = dynamic_cast<const ClassDeclaration*>(stmt)) {
        emitClassHeader(*cls, out);
    } else if (auto enm = dynamic_cast<const EnumDeclaration*>(stmt)) {
        // Already a pure declaration.
        out << "enum " << enm->name << " { ";
        for (size_t i = 0; i < enm->enumerators.size(); ++i) {
            if (i > 0) out << ", ";
            out << enm->enumerators[i];
        }
        out << " };\n\n";
    } else if (auto fn = dynamic_cast<const FunctionDeclaration*>(stmt)) {
        varKinds_.clear(); varClass_.clear(); observerClass_.clear(); containerVarType_.clear(); viewVars_.clear();
        std::string sig = buildFunctionSignature(fn->returnType, fn->name, fn->parameters,
                                                  "", false, false, false, fn->name);
        out << sig << ";\n\n";
    } else if (auto extFn = dynamic_cast<const ExternFunctionDeclaration*>(stmt)) {
        // An ext_fn is already just a signature.
        varKinds_.clear(); varClass_.clear(); observerClass_.clear(); containerVarType_.clear(); viewVars_.clear();
        currentExternRefParams_ = extFn->refParams;
        std::string sig = buildFunctionSignature(extFn->returnType, extFn->name, extFn->parameters,
                                                  "", false, false, false, extFn->name);
        currentExternRefParams_.clear();
        out << sig << ";\n\n";
    } else if (dynamic_cast<const MainFunction*>(stmt)) {
        // A header never declares main.
    } else if (dynamic_cast<const IncludeStatement*>(stmt)) {
        // Collected once at the top of emitHeader().
    } else if (dynamic_cast<const CommentStatement*>(stmt)) {
        // Comments are dropped in the AST-driven output for now.
    }
}

std::string AstEmitter::buildFunctionSignature(const std::string& returnType, const std::string& name,
                                               const std::vector<std::pair<std::string, std::string>>& params,
                                               const std::string& ownerClass, bool isConst,
                                               bool isVirtual, bool isOverride, const std::string& methodName) {
    // A free function (ownerClass empty) is queried under the "" class key. The return is unique_ptr unless the inference classified it shared.
    const std::string savedScope = currentScope_;
    currentScope_ = scopeKeyFor(ownerClass, name, methodName);
    currentMethodReturnsShared_ = !methodName.empty() &&
        analyzer_.getMethodReturnPointerType(ownerClass, methodName) == PointerType::SHARED;
    currentMethodReturnsValueType_ = isValueType(returnType);

    // An object return type is a smart pointer: unique_ptr by default, shared_ptr when the method returns an alias. A value-type class is
    // returned bare (range-for needs begin()/end() by value).
    std::string sig;
    if (isVirtual) sig += "virtual ";
    if (!returnType.empty()) {
        if (isOstreamType(returnType)) {
            // std::ostream cannot be copied, so it is returned by reference. Checked before the value-type and class-type cases.
            sig += "std::ostream& ";
        } else if (currentMethodReturnsValueType_) {
            sig += returnType + " ";
        } else {
            sig += isClassType(returnType)
                ? (currentMethodReturnsShared_ ? "std::shared_ptr<" : "std::unique_ptr<") + returnType + "> "
                : cppTypeFor(returnType, "") + " ";
        }
    }
    sig += name + "(";
    size_t outParamIdx = params.size();  // sentinel: no out-parameter
    if (!methodName.empty()) {
        if (auto it = outParamIndex_.find(outParamKey(ownerClass, methodName));
            it != outParamIndex_.end()) {
            outParamIdx = it->second;
        }
    }
    for (size_t i = 0; i < params.size(); ++i) {
        if (i > 0) sig += ", ";
        const auto& [ptype, pname] = params[i];
        if (i == outParamIdx) {
            // Mutable out-parameter (see outParamIndex_): a raw non-owning pointer like a `ref` observer; the call site passes `.get()`.
            sig += ptype + "* " + pname;
            noteVar(pname, VarKind::Reference);
            observerClass_[pname] = ptype;
            continue;
        }
        if (isOstreamType(ptype)) {
            // std::ostream cannot be passed by value.
            sig += "std::ostream& " + pname;
            noteVar(pname, VarKind::Value);
            // Remember it is a stream, so `out << x` emits a stream chain, not a shift.
            ostreamVars_.insert(pname);
        } else if (isValueType(ptype)) {
            // Passed by value; value types are cheap to copy.
            sig += ptype + " " + pname;
            noteVar(pname, VarKind::Value);
            varClass_[pname] = ptype;
        } else if (isClassType(ptype)) {
            // A parameter's smart-pointer kind is derived like a local's, from its use in the body.
            sig += smartPointerType(ptype, pname) + "<" + ptype + "> " + pname;
            noteVar(pname, VarKind::SmartPtr);
            varClass_[pname] = ptype;
        } else if (currentExternRefParams_.count(i)) {
            // ext_fn `ref`: a container is a read-only `const&`, a primitive a mutable `&`. Other types were rejected at parse time.
            if (isVectorType(ptype)) {
                sig += "const " + cppTypeFor(ptype, pname) + "& " + pname;
                containerVarType_[pname] = ptype;
            } else {
                sig += cppTypeFor(ptype, pname) + "& " + pname;
            }
            noteVar(pname, VarKind::Value);
        } else if (isStringType(ptype) && analyzer_.isReadOnlyStringParam(ownerClass, ownerClass.empty() ? name : methodName, pname)) {
            // A string the body never mutates is passed by const reference.
            sig += "const std::string& " + pname;
            noteVar(pname, VarKind::Value);
        } else if (isVectorType(ptype) &&
                   (ownerClass.empty() ? analyzer_.isReadOnlyContainerParam(name, pname)
                                       : analyzer_.isReadOnlyMethodContainerParam(ownerClass, methodName, pname))) {
            // Inferred read-only container parameter: `const T&`. Whether a call site passes a live reference or an explicit copy depends
            // on the callee's purity, not on this declaration.
            sig += "const " + cppTypeFor(ptype, pname) + "& " + pname;
            noteVar(pname, VarKind::Value);
            containerVarType_[pname] = ptype;
        } else {
            sig += cppTypeFor(ptype, pname) + " " + pname;
            noteVar(pname, VarKind::Value);
            if (isVectorType(ptype) || isChannelType(ptype)) {
                containerVarType_[pname] = ptype;
            }
            if (isStringViewType(ptype)) viewVars_.insert(pname);
        }
    }
    sig += ")";
    if (isConst) sig += " const";
    if (isOverride) sig += " override";
    currentScope_ = savedScope;
    return sig;
}

void AstEmitter::emitFunction(const std::string& returnType, const std::string& name,
                              const std::vector<std::pair<std::string, std::string>>& params,
                              const std::vector<StatementPtr>& body, std::ostringstream& out,
                              const std::string& ownerClass, int baseIndent, bool isConst,
                              bool isVirtual, bool isOverride, const std::string& methodName,
                              bool isConstructor) {
    // Each function has its own local scope; `currentClass_` enables `this->` for fields.
    varKinds_.clear();
    varClass_.clear();
    observerClass_.clear();
    containerVarType_.clear(); viewVars_.clear();
    const std::string savedClass = currentClass_;
    currentClass_ = ownerClass;
    const bool savedReturnsShared = currentMethodReturnsShared_;
    const bool savedReturnsValueType = currentMethodReturnsValueType_;
    const std::string savedReturnType = currentReturnType_;
    currentReturnType_ = returnType;
    const std::string savedFunctionName = currentFunctionName_;
    const std::string savedBodyScope = currentScope_;
    currentScope_ = scopeKeyFor(ownerClass, name, methodName);
    const std::set<std::string> savedParamNames = currentParamNames_;
    currentFunctionName_ = methodName.empty() ? name : methodName;
    currentParamNames_.clear();
    for (const auto& [ptype, pname] : params) currentParamNames_.insert(pname);

    std::string sig = buildFunctionSignature(returnType, name, params, ownerClass, isConst,
                                             isVirtual, isOverride, methodName);

    // Constructors only: hoist the leading run of plain assignments to this class's own fields into the member-initializer list. Stop at
    // the first statement that is not such an assignment, assigns a field already hoisted, or is not an own field (an inherited field
    // cannot appear in this list). The rest stays in the body, in order.
    std::set<size_t> hoistedIndices;
    std::string initList;
    if (isConstructor) {
        // Own fields only: classFields_ includes inherited ones, which cannot be in this class's initializer list.
        std::set<std::string> ownFieldNames;
        std::map<std::string, size_t> declarationIndex;
        if (auto clsIt = classByName_.find(ownerClass); clsIt != classByName_.end()) {
            for (const auto& member : clsIt->second->members) {
                if (auto field = dynamic_cast<const VariableDeclaration*>(member.get())) {
                    ownFieldNames.insert(field->identifier);
                    declarationIndex.emplace(field->identifier, declarationIndex.size());
                }
            }
        }
        // a method of this class or of a class it inherits from (a bare call to one is a call on `this`)
        const auto isMethodName = [&](const std::string& called) {
            for (std::string cls = ownerClass; !cls.empty();) {
                auto it = classByName_.find(cls);
                if (it == classByName_.end()) break;
                for (const auto& member : it->second->members) {
                    if (auto method = dynamic_cast<const MethodDeclaration*>(member.get())) if (method->name == called) return true;
                }
                cls = it->second->baseClass;
            }
            return false;
        };
        std::set<std::string> hoistedFields;
        long lastDeclared = -1;
        for (size_t i = 0; i < body.size(); ++i) {
            auto assign = dynamic_cast<const AssignmentStatement*>(body[i].get());
            if (assign == nullptr || assign->target != nullptr) break;  // non-assignment, or member/subscript target
            if (ownFieldNames.count(assign->variable) == 0) break;      // not this class's own field
            if (hoistedFields.count(assign->variable) > 0) break;       // already hoisted once
            // C++ runs the list in declaration order: an entry written out of that order would change the order its expression runs in
            if (static_cast<long>(declarationIndex[assign->variable]) < lastDeclared) break;
            if (!initializerExprIsSafe(assign->expression.get(), ownFieldNames, hoistedFields, currentParamNames_, isMethodName)) break;
            lastDeclared = static_cast<long>(declarationIndex[assign->variable]);
            hoistedFields.insert(assign->variable);
            hoistedIndices.insert(i);
            const std::string rhs = selectAssignmentRhs(assign, assign->variable, false, false);
            if (!initList.empty()) initList += ", ";
            initList += assign->variable + "(" + rhs + ")";
        }
    }

    // Class members are indented one level; free functions sit at column 0.
    const std::string pad = indentStr(baseIndent);
    out << pad << sig;
    if (!initList.empty()) out << " : " << initList;
    out << " {\n";
    for (size_t i = 0; i < body.size(); ++i) {
        if (hoistedIndices.count(i) > 0) continue;
        emitStatement(body[i].get(), out, baseIndent + 1);
    }
    out << pad << "}\n";
    if (baseIndent == 0) out << "\n";  // blank line only between top-level definitions

    currentClass_ = savedClass;
    currentMethodReturnsShared_ = savedReturnsShared;
    currentMethodReturnsValueType_ = savedReturnsValueType;
    currentReturnType_ = savedReturnType;
    currentFunctionName_ = savedFunctionName;
    currentScope_ = savedBodyScope;
    currentParamNames_ = savedParamNames;
}

// One constructor argument of a spawn job: a class OBJECT is moved in (the checker marked the source moved-from),
// everything else -- primitives, strings, vectors, structs -- is a plain copy and the source stays usable.
std::string AstEmitter::emitSpawnJobArg(const Expression* arg) {
    if (auto id = dynamic_cast<const Identifier*>(arg)) {
        auto it = varClass_.find(id->name);
        if (it != varClass_.end() && !isValueType(it->second) && !isCurrentField(id->name)) return emitMovedArg(arg);
    }
    return emitExpression(arg);
}

std::string AstEmitter::emitMovedArg(const Expression* arg,
                                     const std::pair<std::string, std::string>& targetParam) {
    if (dynamic_cast<const ThisExpression*>(arg) != nullptr && !currentClass_.empty()) {
        // `this` as an argument or right-hand side shares the object, like `return this;`: shared_from_this() uses the existing control
        // block. The class derives from enable_shared_from_this (SmartPointerAnalyzer::methodPassesThisAsArgument); EscapeHatchChecker
        // rejects this in a constructor.
        return "shared_from_this()";
    }
    if (auto id = dynamic_cast<const Identifier*>(arg)) {
        // Object-typed local or parameter (smart pointer): transfer ownership.
        // Field access and references are not moved here.
        if (auto it = varClass_.find(id->name); it != varClass_.end() && !isCurrentField(id->name)) {
            // A shared variable is copied, not moved: moving would null the caller's local, which the checker proved survives the call.
            if (analyzer_.getPointerTypeInScope(currentScope_, it->second, id->name) == PointerType::SHARED) {
                return id->name;
            }
            return "std::move(" + id->name + ")";
        }
        // A std::vector local or parameter in a declaration or call-argument position is moved, because DanglingChecker marks it
        // moved-from there. A field-assignment right-hand side never reaches this (see selectAssignmentRhs), and read-only container
        // arguments are handled by the call-argument loop before it calls emitMovedArg.
        if (auto ctIt = containerVarType_.find(id->name);
            ctIt != containerVarType_.end() && !isCurrentField(id->name)) {
            // A vector handed to a `span` parameter is viewed, not given away (DanglingChecker::applyCallArgMoves does not mark it moved).
            if (isSpanType(targetParam.first)) return id->name;
            return "std::move(" + id->name + ")";
        }
    }
    if (!targetParam.first.empty()) {
        if (auto neww = dynamic_cast<const NewExpression*>(arg)) {
            // A bare `new T(...)` argument constructs the class it names but takes its pointer kind from the receiving parameter's declared type.
            return factoryFor(neww->className, targetParam.second, neww->arguments, targetParam.first);
        }
    }
    return emitExpression(arg);
}

// ---- statements ----------------------------------------------------------

// Shared by ordinary assignments and by hoisted member-initializer entries (emitFunction), so both lower `field = expr;` the same way.
// Hoisting also avoids default-constructing a value-type field that has no zero-argument constructor.
std::string AstEmitter::selectAssignmentRhs(const AssignmentStatement* assign, const std::string& ptrVar,
                                            bool memberTarget, bool subscriptTarget) {
    std::string rhs;
    const bool rhsIsNullptr = isNullptrLiteral(assign->expression.get());
    // a view stored into a `string` (a local or a field) is copied; into another view it is rebound as it is
    if (!subscriptTarget && isViewExpression(assign->expression.get()) && (memberTarget || viewVars_.count(assign->variable) == 0)) {
        return emitAsString(assign->expression.get());
    }
    if (subscriptTarget) {
        // `m[key] = value;` (map insert-or-update): the value is a primitive, copied.
        rhs = emitExpression(assign->expression.get());
    } else if (!memberTarget && kindOf(assign->variable) == VarKind::Reference) {
        // `obs = b;` rebinds the observer and does not move `b`.
        rhs = rhsIsNullptr ? "nullptr" : observerAddress(assign->expression.get());
    } else if (auto neww = dynamic_cast<const NewExpression*>(assign->expression.get())) {
        if (isValueType(neww->className)) {
            // Value-type class: a plain constructor call.
            rhs = neww->className + "(";
            for (size_t i = 0; i < neww->arguments.size(); ++i) {
                if (i > 0) rhs += ", ";
                rhs += emitExpression(neww->arguments[i].get());
            }
            rhs += ")";
        } else {
            // `target = new T(...)` -> make_*<T>(...) with the target field's ptr kind.
            rhs = factoryFor(neww->className, ptrVar, neww->arguments);
        }
    } else if (rhsIsNullptr && !ptrVar.empty() &&
               analyzer_.getPointerTypeForVariable(ptrVar) == PointerType::WEAK) {
        // weak_ptr has no operator=(nullptr_t); an empty one is `{}`.
        rhs = "{}";
    } else if ((memberTarget || isCurrentField(assign->variable)) &&
               dynamic_cast<const ThisExpression*>(assign->expression.get()) != nullptr &&
               !currentClass_.empty()) {
        // Storing `this` into a field is shared co-ownership, like passing it as an argument (see emitMovedArg).
        rhs = emitMovedArg(assign->expression.get());
    } else if (memberTarget || isCurrentField(assign->variable)) {
        // Storing into a field (`a.b = obj;` or a bare field name) links and does not consume `obj`: DanglingChecker keeps it live, so
        // this must be a plain copy. Exception: a std::vector<primitive> parameter that classifyLastUseFieldAssignedParams proved is
        // not used again after this assignment is moved.
        auto rhsId = dynamic_cast<const Identifier*>(assign->expression.get());
        // A spawn job's constructor stores each object capture into its (unique) field: a move.
        if (rhsId != nullptr && !currentClass_.empty() && currentParamNames_.count(rhsId->name) > 0) {
            auto jobIt = classByName_.find(currentClass_);
            if (jobIt != classByName_.end() && jobIt->second->isSpawnJob && varClass_.count(rhsId->name) > 0 &&
                !isValueType(varClass_.at(rhsId->name))) {
                return emitMovedArg(assign->expression.get());
            }
        }
        const bool rhsIsLastUseParam = rhsId != nullptr && currentParamNames_.count(rhsId->name) > 0 &&
            (currentClass_.empty() ? analyzer_.isLastUseFieldAssignedParam(currentFunctionName_, rhsId->name)
                                    : analyzer_.isLastUseMethodFieldAssignedParam(currentClass_, currentFunctionName_,
                                                                                   rhsId->name));
        rhs = rhsIsLastUseParam ? emitMovedArg(assign->expression.get())
                                  : emitExpression(assign->expression.get());
    } else if (!ptrVar.empty() && isValueType(varClass_.count(ptrVar) ? varClass_.at(ptrVar) : "")) {
        // A value-type class: a plain copy.
        rhs = emitExpression(assign->expression.get());
    } else {
        // Transfers ownership of an object local (a plain local or parameter, not a field).
        rhs = emitMovedArg(assign->expression.get());
    }
    return rhs;
}

// In debug mode (-g) a `#line` directive, at column 0 on a line of its own, tells the C++ compiler that what follows came from that line of the .clg file.
void AstEmitter::emitStatement(const Statement* stmt, std::ostringstream& out, int indent) {
    if (debugLines_ && stmt != nullptr && stmt->loc.line > 0) {
        out << "#line " << stmt->loc.line << " \"" << debugPath_ << "\"\n";
    }
    emitStatementImpl(stmt, out, indent);
}

void AstEmitter::emitStatementImpl(const Statement* stmt, std::ostringstream& out, int indent) {
    const std::string pad = indentStr(indent);

    if (auto decl = dynamic_cast<const VariableDeclaration*>(stmt)) {
        if (decl->isObserver) {
            // Observer `ref T b = a;` is a raw pointer `T* b = (a).get();` so it can be rebound (a `T&` cannot). The `.get()` is generated
            // code on a pointer the checker proved safe to observe; source-level `.get()` stays rejected.
            noteVar(decl->identifier, VarKind::Reference);
            observerClass_[decl->identifier] = decl->type;
            std::string rhs = isNullptrLiteral(decl->expression.get())
                ? "nullptr"
                : observerAddress(decl->expression.get());
            out << pad << decl->type << "* " << decl->identifier << " = " << rhs << ";\n";
        } else if (isValueType(decl->type)) {
            // Value-type class: plain, copied; `new T(...)` becomes a constructor call.
            noteVar(decl->identifier, VarKind::Value);
            varClass_[decl->identifier] = decl->type;
            std::string rhs;
            if (auto neww = dynamic_cast<const NewExpression*>(decl->expression.get())) {
                rhs = neww->className + "(";
                for (size_t i = 0; i < neww->arguments.size(); ++i) {
                    if (i > 0) rhs += ", ";
                    rhs += emitExpression(neww->arguments[i].get());
                }
                rhs += ")";
            } else {
                rhs = emitExpression(decl->expression.get());
            }
            out << pad << decl->type << " " << decl->identifier << " = " << rhs << ";\n";
        } else if (isClassType(decl->type) && isBareValueReturningSpecializationCall(decl->expression.get())) {
            // The call's C++ result is a bare value, so the local is declared bare. Checked before the smart-pointer branch.
            noteVar(decl->identifier, VarKind::Value);
            varClass_[decl->identifier] = decl->type;
            out << pad << decl->type << " " << decl->identifier << " = "
                << emitExpression(decl->expression.get()) << ";\n";
        } else if (isClassType(decl->type)) {
            // A local initialized from a call that returns shared must itself be a shared_ptr.
            const bool sharedFromCall = isSharedReturningCall(decl->expression.get());
            const std::string ptr = sharedFromCall ? "std::shared_ptr"
                                                    : smartPointerType(decl->type, decl->identifier);
            std::string rhs;
            if (auto neww = dynamic_cast<const NewExpression*>(decl->expression.get())) {
                // Construct the named class but pick the pointer kind from the declared type (they differ for `Animal a = new Dog();`).
                rhs = factoryFor(neww->className, decl->identifier, neww->arguments, decl->type);
            } else if (sharedFromCall) {
                // The result is an alias: copy the handle.
                rhs = emitExpression(decl->expression.get());
            } else {
                // `Node b = a;` transfers ownership: emit std::move(a).
                rhs = emitMovedArg(decl->expression.get());
            }
            // Register after the RHS so it can't accidentally reference itself.
            noteVar(decl->identifier, VarKind::SmartPtr);
            varClass_[decl->identifier] = decl->type;
            out << pad << ptr << "<" << decl->type << "> " << decl->identifier
                << " = " << rhs << ";\n";
        } else {
            noteVar(decl->identifier, VarKind::Value);
            // A vector right-hand side goes through emitMovedArg: DanglingChecker marks the source moved-from.
            const bool rhsIsVector = isVectorType(decl->type);
            if (rhsIsVector) {
                containerVarType_[decl->identifier] = decl->type;
            }
            if (isChannelType(decl->type)) {
                containerVarType_[decl->identifier] = decl->type;      // (for method receivers; the handle is copied, never moved)
            }
            if (isStringViewType(decl->type)) viewVars_.insert(decl->identifier);
            std::string rhs = rhsIsVector ? emitMovedArg(decl->expression.get())
                                          : (isStringType(decl->type) ? emitAsString(decl->expression.get()) : emitExpression(decl->expression.get()));
            out << pad << cppTypeFor(decl->type, decl->identifier) << " " << decl->identifier << " = "
                << rhs << ";\n";
        }
    } else if (auto assign = dynamic_cast<const AssignmentStatement*>(stmt)) {
        // Left-hand side: a member target (`a.b = ...`) is emitted via the member-access
        // path (so `.`/`->` and this-> resolve); a bare name gets this-> if it's a field.
        std::string lhs;
        std::string ptrVar = assign->variable;  // name used to pick the target's ptr kind
        const bool memberTarget = dynamic_cast<const MemberAccess*>(assign->target.get()) != nullptr;
        const bool subscriptTarget = dynamic_cast<const SubscriptExpression*>(assign->target.get()) != nullptr;
        if (auto m = dynamic_cast<const MemberAccess*>(assign->target.get())) {
            emittingAssignTarget_ = true;
            lhs = emitExpression(m);
            emittingAssignTarget_ = false;
            ptrVar = m->member;  // the field being assigned
        } else if (subscriptTarget) {
            lhs = emitExpression(assign->target.get());
        } else {
            lhs = isCurrentField(assign->variable) ? "this->" + assign->variable
                                                   : assign->variable;
        }
        // `n = value;` with a class-typed `n` and a `value` of another class means the user's `operator=`. A bare field store
        // (`child = nullptr;`) is excluded: it is an ordinary link.
        if (!memberTarget && !subscriptTarget && !isCurrentField(assign->variable)) {
            Identifier lhsId(assign->variable);
            const std::string lhsClass = exprObjectClass(&lhsId);
            const std::string rhsClass = exprObjectClass(assign->expression.get());
            // operatorAssignParamTypes_ is used because classMethodParams_ keeps only one of several arity-1 operator= overloads.
            if (auto opIt = operatorAssignParamTypes_.find(lhsClass); opIt != operatorAssignParamTypes_.end()) {
                // A same-class right-hand side (`n = otherNode;`) uses operator= only when the class declares an overload taking itself and
                // `n` is a read-only parameter (a handle meant to stay the same logical object, mutated in place). A local reassigned in a
                // loop (`cur = cur.next();`) has the same shape but needs a plain repoint. A right-hand side of another class uses
                // operator= whenever any overload exists.
                const bool lhsIsReadOnlyParam = currentParamNames_.count(assign->variable) > 0 &&
                    (analyzer_.isReadOnlyMethodParam(currentClass_, currentFunctionName_, assign->variable) ||
                     analyzer_.isReadOnlyFreeFunctionParam(currentFunctionName_, assign->variable));
                const bool hasMatchingOverload = (rhsClass == lhsClass)
                    ? (opIt->second.count(lhsClass) > 0 && lhsIsReadOnlyParam)
                    : !opIt->second.empty();
                if (hasMatchingOverload) {
                    out << pad << lhs << "->operator=(" << emitExpression(assign->expression.get())
                        << ");\n";
                    return;
                }
            }
        }
        std::string rhs = selectAssignmentRhs(assign, ptrVar, memberTarget, subscriptTarget);
        out << pad << lhs << " = " << rhs << ";\n";
    } else if (auto ret = dynamic_cast<const ReturnStatement*>(stmt)) {
        // A bare `return;` has no expression to lower.
        if (!ret->expression) {
            out << pad << "return;\n";
            return;
        }
        // Returning an object hands ownership to the caller: `new T(...)` becomes a factory and an owning local is moved out.
        std::string rhs;
        auto retIsThis = dynamic_cast<const ThisExpression*>(ret->expression.get()) != nullptr;
        if (currentMethodReturnsValueType_) {
            // Value-type return: plain, copied.
            if (auto neww = dynamic_cast<const NewExpression*>(ret->expression.get())) {
                rhs = neww->className + "(";
                for (size_t i = 0; i < neww->arguments.size(); ++i) {
                    if (i > 0) rhs += ", ";
                    rhs += emitExpression(neww->arguments[i].get());
                }
                rhs += ")";
            } else if (retIsThis) {
                // `this` is always a raw pointer; dereference it to copy the object.
                rhs = "*this";
            } else {
                rhs = emitExpression(ret->expression.get());
            }
        } else if (currentMethodReturnsShared_ && retIsThis) {
            // `return this;` from a shared-returning method: shared_from_this() uses the existing control block (`shared_ptr<T>(this)` would make a second).
            rhs = "shared_from_this()";
        } else if (currentMethodReturnsShared_) {
            // A shared-returning method returns an alias: copy the handle and leave the field live.
            rhs = emitExpression(ret->expression.get());
        } else if (auto neww = dynamic_cast<const NewExpression*>(ret->expression.get())) {
            // make_unique, to match the lowered unique_ptr<T> return type.
            rhs = "std::make_unique<" + neww->className + ">(";
            for (size_t i = 0; i < neww->arguments.size(); ++i) {
                if (i > 0) rhs += ", ";
                rhs += emitExpression(neww->arguments[i].get());
            }
            rhs += ")";
        } else if (isStringType(currentReturnType_) && isViewExpression(ret->expression.get())) {
            rhs = emitAsString(ret->expression.get());
        } else {
            rhs = emitMovedArg(ret->expression.get());
        }
        out << pad << "return " << rhs << ";\n";
    } else if (auto ifs = dynamic_cast<const IfStatement*>(stmt)) {
        out << pad << "if (" << emitExpression(ifs->condition.get()) << ") {\n";
        for (const auto& inner : ifs->body) {
            emitStatement(inner.get(), out, indent + 1);
        }
        out << pad << "}";
        if (!ifs->elseBody.empty()) {
            // `else if` is emitted inline (not in debug mode: the nested `if` needs its `#line` at the start of a line).
            if (!debugLines_ && ifs->elseBody.size() == 1 &&
                dynamic_cast<const IfStatement*>(ifs->elseBody[0].get())) {
                std::ostringstream nested;
                emitStatement(ifs->elseBody[0].get(), nested, indent);
                std::string s = nested.str();
                // Drop the leading indent of the nested if so it reads `else if (...)`.
                const std::string lead = pad;
                if (s.rfind(lead, 0) == 0) s = s.substr(lead.size());
                out << " else " << s;
                return;
            }
            out << " else {\n";
            for (const auto& inner : ifs->elseBody) {
                emitStatement(inner.get(), out, indent + 1);
            }
            out << pad << "}\n";
        } else {
            out << "\n";
        }
    } else if (auto whiles = dynamic_cast<const WhileStatement*>(stmt)) {
        out << pad << "while (" << emitExpression(whiles->condition.get()) << ") {\n";
        for (const auto& inner : whiles->body) {
            emitStatement(inner.get(), out, indent + 1);
        }
        out << pad << "}\n";
    } else if (auto cdecl = dynamic_cast<const ContainerDeclaration*>(stmt)) {
        // `std::vector<T> v;` default-constructs empty; a container is a plain value.
        noteVar(cdecl->identifier, VarKind::Value);
        containerVarType_[cdecl->identifier] = cdecl->type;
        out << pad << cppTypeFor(cdecl->type, cdecl->identifier) << " " << cdecl->identifier << ";\n";
    } else if (auto fors = dynamic_cast<const ForStatement*>(stmt)) {
        // A class instance with its own member begin()/end() is dereferenced in range-for (a smart pointer has none). A class without a
        // member begin() is iterated through free begin()/end() found by ADL, which take the smart pointer, so it is not dereferenced.
        const std::string containerClass = exprObjectClass(fors->container.get());
        const bool hasMemberBegin = [&] {
            auto it = classMethodParams_.find(containerClass);
            return it != classMethodParams_.end() && it->second.count("begin") > 0;
        }();
        const bool derefContainer = !containerClass.empty() && !isValueType(containerClass)
            && hasMemberBegin;

        // Range-for maps a class-typed element to its smart pointer type; a value-type element is stored bare and stays bare.
        // A non-class element keeps its CeeLing spelling only when it is also valid C++; `string` and
        // `byte`/`int64` are not (cppTypeFor maps them).
        std::string elemCppType = isClassType(fors->loopVarType) ? fors->loopVarType
                                                                 : cppTypeFor(fors->loopVarType, "");
        bool elementIsUnique = false;
        if (isClassType(fors->loopVarType) && !isValueType(fors->loopVarType)) {
            std::string containerName;
            if (auto id = dynamic_cast<const Identifier*>(fors->container.get())) {
                containerName = id->name;
            }
            // A vector or map container's element kind comes from containerElementIsShared. With a free ADL `begin()` (no member begin)
            // it comes from the return kind of `*begin()` instead; this assumes at most one free `begin` in the program.
            std::string beginReturnClass = (!hasMemberBegin && functionReturnType_.count("begin"))
                ? functionReturnType_.at("begin") : "";
            if (!beginReturnClass.empty() &&
                analyzer_.getMethodReturnPointerType(beginReturnClass, "operator*") == PointerType::SHARED) {
                elementIsUnique = false;
            } else {
                elementIsUnique = !analyzer_.containerElementIsShared(containerName);
            }
            elemCppType = (elementIsUnique ? "std::unique_ptr<" : "std::shared_ptr<") +
                          fors->loopVarType + ">";
        }
        // A unique_ptr element cannot be copied, so the loop variable binds by reference; shared_ptr elements are copied.
        out << pad << "for (" << elemCppType << (elementIsUnique ? "& " : " ")
            << fors->loopVarName << " : " << (derefContainer ? "*" : "")
            << emitExpression(fors->container.get()) << ") {\n";
        if (isClassType(fors->loopVarType) && !isValueType(fors->loopVarType)) {
            // Register the loop variable as a smart-pointer local so `.` becomes `->` in the body.
            noteVar(fors->loopVarName, VarKind::SmartPtr);
            varClass_[fors->loopVarName] = fors->loopVarType;
        } else {
            // Also covers a value-type element: no wrapping, `.` access.
            noteVar(fors->loopVarName, VarKind::Value);
        }
        for (const auto& inner : fors->body) {
            emitStatement(inner.get(), out, indent + 1);
        }
        out << pad << "}\n";
    } else if (auto exprStmt = dynamic_cast<const ExpressionStatement*>(stmt)) {
        out << pad << emitExpression(exprStmt->expression.get()) << ";\n";
    } else if (dynamic_cast<const BreakStatement*>(stmt) != nullptr) {
        out << pad << "break;\n";
    } else if (dynamic_cast<const ContinueStatement*>(stmt) != nullptr) {
        out << pad << "continue;\n";
    } else if (auto throwStmt = dynamic_cast<const ThrowStatement*>(stmt)) {
        // Only strings are thrown. A string literal is wrapped in std::string: `catch (const std::string&)` does not match a thrown `const char*`.
        const bool isLiteral = dynamic_cast<const StringLiteral*>(throwStmt->expression.get()) != nullptr;
        std::string operand = emitExpression(throwStmt->expression.get());
        if (isLiteral) operand = "std::string(" + operand + ")";
        out << pad << "throw " << operand << ";\n";
    } else if (auto tryStmt = dynamic_cast<const TryStatement*>(stmt)) {
        // `catch (string name)` lowers to catch-by-const-ref; string is the only catch type.
        out << pad << "try {\n";
        for (const auto& inner : tryStmt->body) {
            emitStatement(inner.get(), out, indent + 1);
        }
        out << pad << "} catch (const std::string& " << tryStmt->catchIdentifier << ") {\n";
        for (const auto& inner : tryStmt->catchBody) {
            emitStatement(inner.get(), out, indent + 1);
        }
        out << pad << "}\n";
    }
}

// ---- expressions ---------------------------------------------------------

std::string AstEmitter::observerAddress(const Expression* expr) {
    if (auto id = dynamic_cast<const Identifier*>(expr)) {
        if (observerClass_.count(id->name)) return id->name;
    }
    return "(" + emitExpression(expr) + ").get()";
}

bool AstEmitter::isWeakFieldRead(const Expression* expr) const {
    if (auto id = dynamic_cast<const Identifier*>(expr)) {
        return isCurrentField(id->name) && analyzer_.isWeakField(currentClass_, id->name);
    }
    if (auto member = dynamic_cast<const MemberAccess*>(expr)) {
        const std::string objClass = exprObjectClass(member->object.get());
        return !objClass.empty() && analyzer_.isWeakField(objClass, member->member);
    }
    return false;
}

std::string AstEmitter::emitExpression(const Expression* expr) {
    if (expr == nullptr) return "";

    if (auto id = dynamic_cast<const Identifier*>(expr)) {
        // A bare reference to a field of the current class -> this->field.
        if (isCurrentField(id->name)) {
            // A weak_ptr field can only be read through lock(); the result is a shared_ptr that
            // may be empty, so `!owner`, `owner == nullptr` and `if (owner)` all work on it.
            if (!emittingAssignTarget_ && analyzer_.isWeakField(currentClass_, id->name)) {
                return "this->" + id->name + ".lock()";
            }
            return "this->" + id->name;
        }
        return id->name;
    }
    if (auto num = dynamic_cast<const Number*>(expr)) {
        return num->original_text;
    }
    if (auto scoped = dynamic_cast<const ScopedIdentifier*>(expr)) {
        // `std::cout`, `std::endl`, etc. — emitted verbatim.
        return scoped->qualifiedName;
    }
    if (auto str = dynamic_cast<const StringLiteral*>(expr)) {
        return str->original_text;
    }
    if (auto bin = dynamic_cast<const BinaryExpression*>(expr)) {
        // Stream `<<` chains are emitted flat. Gated on isStreamChain, not on the operator: dropping the parentheses of an integer
        // shift changes its meaning (`(1 << 4) + 1`).
        if (bin->operator_str == "<<" && isStreamChain(bin)) {
            return emitExpression(bin->left.get()) + " << " + emitExpression(bin->right.get());
        }
        // `a == b` / `a != b` on two handles of one class that declares its own operator==/!=: call it (`a->operator==(b)`). Plain
        // `a == b` would compare shared_ptr identity, which differs from the class's equality once it has separate handle and payload.
        if (bin->operator_str == "==" || bin->operator_str == "!=") {
            const std::string leftClass = exprObjectClass(bin->left.get());
            const std::string rightClass = exprObjectClass(bin->right.get());
            // Not for a bare field of the current class (`current != other.current`): that compares handle identity, and the field may be
            // null, so calling a method on it would be undefined.
            const auto leftId = dynamic_cast<const Identifier*>(bin->left.get());
            const bool leftIsOwnField = leftId != nullptr && isCurrentField(leftId->name);
            if (!leftClass.empty() && leftClass == rightClass && !leftIsOwnField &&
                !analyzer_.isValueTypeClass(leftClass)) {
                // Method names are stored with the "operator" prefix.
                const std::string methodName = "operator" + bin->operator_str;
                auto clsIt = classMethodParams_.find(leftClass);
                if (clsIt != classMethodParams_.end() && clsIt->second.count(methodName) > 0) {
                    // The parameter is a smart pointer, so the right-hand side is passed as is, not dereferenced.
                    return emitExpression(bin->left.get()) + "->" + methodName + "("
                           + emitExpression(bin->right.get()) + ")";
                }
            }
        }
        return "(" + emitExpression(bin->left.get()) + " " + bin->operator_str + " "
               + emitExpression(bin->right.get()) + ")";
    }
    if (auto un = dynamic_cast<const UnaryExpression*>(expr)) {
        // Prefix operator. A value-type operand is never wrapped, so `*it` and `++it` reach the user's operator through C++ overload resolution.
        return un->operator_str + emitExpression(un->operand.get());
    }
    if (auto neww = dynamic_cast<const NewExpression*>(expr)) {
        // `new T(...)` outside a typed declaration: a plain constructor call for a value type, otherwise make_shared.
        if (isValueType(neww->className)) {
            std::string s = neww->className + "(";
            for (size_t i = 0; i < neww->arguments.size(); ++i) {
                if (i > 0) s += ", ";
                s += emitExpression(neww->arguments[i].get());
            }
            s += ")";
            return s;
        }
        // A spawn job's object captures are moved into it (see factoryFor).
        auto jobIt = classByName_.find(neww->className);
        const bool isSpawnJob = jobIt != classByName_.end() && jobIt->second->isSpawnJob;
        std::string argText;
        for (size_t i = 0; i < neww->arguments.size(); ++i) {
            if (i > 0) argText += ", ";
            argText += isSpawnJob ? emitSpawnJobArg(neww->arguments[i].get()) : emitExpression(neww->arguments[i].get());
        }
        return sharedConstruct(neww->className, argText);
    }
    if (auto call = dynamic_cast<const FunctionCall*>(expr)) {
        // A primitive conversion `int(x)` is a static_cast, since `byte` lowers to `unsigned char`, which has no functional spelling.
        if (call->callee == nullptr && call->arguments.size() == 1 && isPrimitiveConversionName(call->function_name)) {
            return "static_cast<" + cppTypeFor(call->function_name, "") + ">(" +
                   emitExpression(call->arguments[0].get()) + ")";
        }
        std::string prefix;
        std::string receiverContainerType;  // "" unless recv is a known container local
        std::string receiverName;
        std::string receiverClass;
        std::string methodName;
        std::string recvText;  // the receiver's emitted text; copyWithin below needs it on its own
        if (auto m = dynamic_cast<const MemberAccess*>(call->callee.get())) {
            // Method call `recv.method(args)` or `recv.method<T>(args)`.
            const std::string recv = emitExpression(m->object.get());
            recvText = recv;
            const std::string mClass = exprObjectClass(m->object.get());
            const std::string op = (mClass.empty() || isValueType(mClass)) ? "." : "->";
            prefix = (op == "->" ? checkedDeref(m->object.get(), recv, m->loc.line) : recv) + op + m->member;
            if (!call->templateArg.empty()) {
                prefix += "<" + cppTypeFor(call->templateArg, "") + ">";
            }
            receiverContainerType = containerVarType(m->object.get());
            if (auto id = dynamic_cast<const Identifier*>(m->object.get())) {
                receiverName = id->name;
            } else if (auto field = dynamic_cast<const MemberAccess*>(m->object.get())) {
                receiverName = field->member;   // `obj.items.push_back(x)`: containers are keyed by the field's name
            }
            receiverClass = mClass;
            methodName = m->member;
        } else {
            // A bare call to a free function that shares its name with a method of the current class needs `::`, or C++ picks the member.
            // Only on a real collision: a namespaced function called bare inside its namespace must not be forced to the global one.
            bool collidesWithOwnMethod = !currentClass_.empty() &&
                call->function_name.find("::") == std::string::npos &&
                functionParams_.count(call->function_name) > 0 &&
                classMethodParams_.count(currentClass_) > 0 &&
                classMethodParams_.at(currentClass_).count(call->function_name) > 0;
            prefix = (collidesWithOwnMethod ? "::" : "") + call->function_name;
            if (!call->templateArg.empty()) {
                prefix += "<" + cppTypeFor(call->templateArg, "") + ">";
            }
            // An implicit-`this` call is a call of the current class's method, so outParamIndex_ is looked up under currentClass_.
            // Not when it collides with a free function: that call has no out-parameter.
            if (!collidesWithOwnMethod) {
                receiverClass = currentClass_;
                methodName = call->function_name;
            }
        }
        const bool receiverIsVector = isVectorType(receiverContainerType);
        // v.copyWithin(dst, src, len) has no std::vector counterpart: it lowers to the cling::copyWithin helper (memmove semantics; a
        // self-overlapping back-reference copy is not supported, see isKnownVectorMethod). Primitive elements only, checked again here
        // because the helper computes a byte count.
        if (receiverIsVector && methodName == "copyWithin" && call->arguments.size() == 3 &&
            isPrimitiveType(vectorElementType(receiverContainerType))) {
            const std::string dst = emitExpression(call->arguments[0].get());
            const std::string src = emitExpression(call->arguments[1].get());
            const std::string len = emitExpression(call->arguments[2].get());
            return "cling::copyWithin(" + recvText + ", " + dst + ", " + src + ", " + len + ")";
        }
        if (receiverIsVector && isVectorAlgorithm(methodName) && call->arguments.size() == vectorAlgorithmArity(methodName) &&
            isPrimitiveType(vectorElementType(receiverContainerType))) {
            std::string helper = "cling::v" + methodName + "(" + recvText;
            if (!call->arguments.empty()) helper += ", " + emitExpression(call->arguments[0].get());
            return helper + ")";
        }
        const bool elementIsShared = receiverIsVector && analyzer_.containerElementIsShared(receiverName);
        size_t argOutParamIdx = call->arguments.size();  // sentinel: no out-parameter
        if (!receiverClass.empty()) {
            if (auto it = outParamIndex_.find(outParamKey(receiverClass, methodName));
                it != outParamIndex_.end()) {
                argOutParamIdx = it->second;
            }
        }
        std::string s = prefix + "(";
        for (size_t i = 0; i < call->arguments.size(); ++i) {
            if (isChannelType(receiverContainerType)) {
                // Channel::send copies its argument (see DanglingChecker::applyCallArgMoves): the sender keeps using what it sent.
                if (i > 0) s += ", ";
                s += emitExpression(call->arguments[i].get());
                continue;
            }
            if (i > 0) s += ", ";
            if (i == argOutParamIdx) {
                // An out-parameter is passed as `.get()` and not moved (see buildFunctionSignature and DanglingChecker::applyCallArgMoves).
                // A plain-value argument has no `.get()`, so its address is taken.
                if (auto argId = dynamic_cast<const Identifier*>(call->arguments[i].get());
                    argId != nullptr && kindOf(argId->name) == VarKind::Value) {
                    s += "&" + argId->name;
                } else {
                    s += "(" + emitExpression(call->arguments[i].get()) + ").get()";
                }
                continue;
            }
            if (receiverIsVector && vectorMethodSharesArg(methodName, i)) {
                // A value-type element is stored bare, so no pointer logic applies. Checked first.
                const std::string vectorElemType = vectorElementType(receiverContainerType);
                if (!vectorElemType.empty() && analyzer_.isValueTypeClass(vectorElemType)) {
                    if (auto neww = dynamic_cast<const NewExpression*>(call->arguments[i].get())) {
                        // `push_back(new Vertex(...))` becomes a plain constructor call.
                        std::string arg = neww->className + "(";
                        for (size_t j = 0; j < neww->arguments.size(); ++j) {
                            if (j > 0) arg += ", ";
                            arg += emitExpression(neww->arguments[j].get());
                        }
                        arg += ")";
                        s += arg;
                    } else {
                        // An existing variable is copied: it has no ownership to transfer.
                        s += emitExpression(call->arguments[i].get());
                    }
                    continue;
                }
                // The element pointer kind is decided once per container (matching DanglingChecker::applyCallArgMoves). A push shares
                // (copies) only when the element kind is shared and the argument is already a shared_ptr; otherwise it moves, wrapped in
                // a shared_ptr when the element kind is shared.
                if (auto neww = dynamic_cast<const NewExpression*>(call->arguments[i].get())) {
                    // A bare `new T(...)`: use the container's element pointer kind.
                    std::string argText;
                    for (size_t j = 0; j < neww->arguments.size(); ++j) {
                        if (j > 0) argText += ", ";
                        argText += emitExpression(neww->arguments[j].get());
                    }
                    s += elementIsShared ? sharedConstruct(neww->className, argText)
                                         : "std::make_unique<" + neww->className + ">(" + argText + ")";
                    continue;
                }
                auto argId = dynamic_cast<const Identifier*>(call->arguments[i].get());
                const std::string argClass = argId ? exprObjectClass(argId) : std::string{};
                const bool argIsShared = !argClass.empty() &&
                    analyzer_.getPointerTypeInScope(currentScope_, argClass, argId->name) == PointerType::SHARED;
                if (elementIsShared && argIsShared) {
                    s += emitExpression(call->arguments[i].get());  // share: plain copy
                } else if (elementIsShared) {
                    // element=shared, arg=unique -> move into a shared_ptr wrapper.
                    s += "std::shared_ptr<" + argClass + ">(" +
                         emitMovedArg(call->arguments[i].get()) + ")";
                } else {
                    s += emitMovedArg(call->arguments[i].get(), calleeParam(*call, i));
                }
                continue;
            }
            if (const std::string argContainerType =
                    argNeedsExplicitCopyForReadOnlyContainerParam(*call, i, receiverClass, methodName);
                !argContainerType.empty()) {
                // A field argument bound to a read-only container parameter of a callee not proven heap-pure is passed as an explicit
                // copy, which keeps by-value semantics.
                s += cppTypeFor(argContainerType, "") + "(" + emitExpression(call->arguments[i].get()) + ")";
                continue;
            }
            if (isViewExpression(call->arguments[i].get()) && isStringType(calleeParamType(*call, i))) {
                s += emitAsString(call->arguments[i].get());      // a view handed to a `string` parameter: an explicit copy
                continue;
            }
            s += emitMovedArg(call->arguments[i].get(), calleeParam(*call, i));
        }
        s += ")";
        return s;
    }
    if (auto member = dynamic_cast<const MemberAccess*>(expr)) {
        // `->` for a smart-pointer object (including nested access and this-> fields); `.` for value types and plain values.
        // The assignment-target flag applies only to the outermost field written: in `a.owner.next = x`, `a.owner` is a read and is locked.
        const bool targetHere = emittingAssignTarget_;
        emittingAssignTarget_ = false;
        std::string objStr = emitExpression(member->object.get());
        emittingAssignTarget_ = targetHere;
        const std::string memberClass = exprObjectClass(member->object.get());
        const std::string op = (memberClass.empty() || isValueType(memberClass)) ? "." : "->";
        // Dereferencing THROUGH a weak field: lock() may yield an empty pointer, so check it.
        if (op == "->") {
            // every dereference of an object handle is null-checked (a weak read yields a handle
            // that may be empty; any other handle may simply be nullptr)
            objStr = checkedDeref(member->object.get(), objStr, member->loc.line);
        }
        // A weak field that is itself being READ as a value (not written) is locked.
        if (!targetHere && isWeakFieldRead(member)) {
            return objStr + op + member->member + ".lock()";
        }
        return objStr + op + member->member;
    }
    if (auto sub = dynamic_cast<const SubscriptExpression*>(expr)) {
        // `n[key]` on a class-typed receiver is the user's `operator[]` (smart pointers have no usable one): `n->operator[](key)`.
        const std::string objClass = exprObjectClass(sub->object.get());
        if (!objClass.empty()) {
            return checkedDeref(sub->object.get(), emitExpression(sub->object.get()), sub->loc.line) + "->operator[](" +
                   emitExpression(sub->index.get()) + ")";
        }
        // `v[i]` / `s[i]` goes through cling::at, for reads and stores, which aborts with the .clg line on an out-of-range index. It leaves map receivers alone.
        if (boundsChecks_) {
            return "cling::at(" + emitExpression(sub->object.get()) + ", " + emitExpression(sub->index.get()) +
                   ", " + std::to_string(sub->loc.line) + ")";
        }
        return emitExpression(sub->object.get()) + "[" + emitExpression(sub->index.get()) + "]";
    }
    // Unknown node: best effort.
    return expr->toString();
}

} // namespace cling
