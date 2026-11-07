// AstEmitter: whole programs and headers, program scans, type queries
#include "emit/ast_emitter.h"
#include <functional>
#include "syntax/class_hierarchy.h"
#include "facts/library_summary.h"
#include "emit/runtime_helpers.h"
#include "passes/channel_check.h"

#include "emit/ast_emitter_util.h"

namespace cling {

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

} // namespace cling
