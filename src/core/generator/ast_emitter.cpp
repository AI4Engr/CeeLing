#include "ast_emitter.h"
#include "class_hierarchy.h"
#include "library_summary.h"

namespace cling {

namespace {
bool isNullptrLiteral(const Expression* expr) {
    auto id = dynamic_cast<const Identifier*>(expr);
    return id != nullptr && id->name == "nullptr";
}
} // namespace

std::string AstEmitter::emit(const Program& program) {
    classNames_.clear();
    varKinds_.clear();
    usedRuntimeHelpers_.clear();
    usesCopyWithin_ = false;
    collectClassNames(program);
    collectUsedRuntimeHelpers(program);

    std::ostringstream out;
    // Headers the lowered constructs always need (smart pointers, std::move, cout),
    // plus any #include the source itself declared — emitted once, de-duplicated so a
    // source `#include <memory>` doesn't collide with the one we require.
    std::set<std::string> emittedIncludes;
    for (const char* hdr : {"#include <memory>", "#include <utility>",
                            "#include <iostream>", "#include <vector>",
                            "#include <string>", "#include <map>"}) {
        if (emittedIncludes.insert(hdr).second) out << hdr << "\n";
    }
    if (!usedRuntimeHelpers_.empty()) {
        for (const char* hdr : {"#include <fstream>", "#include <sstream>"}) {
            if (emittedIncludes.insert(hdr).second) out << hdr << "\n";
        }
    }
    if (usesCopyWithin_) {
        // cling::copyWithin's std::memmove fallback (copyWithinHelperDefinition).
        if (emittedIncludes.insert("#include <cstring>").second) out << "#include <cstring>\n";
    }
    for (const auto& stmt : program.statements) {
        // Only THIS file's own #include lines -- an external declaration's includes
        // are that included file's own concern (its .cpp/.clh already carries them).
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

    // Group top-level declarations by namespaceName and wrap each contiguous run in
    // `namespace X { ... }`. Desugaring (ASTBuilder::buildNamespaceDeclaration)
    // preserves each namespace's members as a contiguous block in program order and
    // never interleaves two namespaces, so a simple "did the namespace change since
    // the last statement" check is sufficient -- no separate grouping/sorting pass
    // needed. A statement with an empty namespaceName (the common case, and every
    // statement type that doesn't have the field at all) emits exactly as before,
    // so a namespace-free program's output is byte-identical to pre-namespace output.
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
    varKinds_.clear(); varClass_.clear(); observerClass_.clear(); containerVarType_.clear();
    // When this specialization's return type IS its own concrete
    // specialized type (`Point Node::as<Point>()`, as opposed to e.g. `bool
    // Node::decodeInto<Point>(Point out)`, whose return type is `bool`, not
    // `Point`), real C++ requires the return -- and every local of that
    // same type in the body -- to be bare/unwrapped, not smart-pointer
    // wrapped (confirmed via probe: an explicit specialization's return
    // must match the primary's T-substituted type exactly). See
    // isValueType's comment for how this is scoped to ONLY this function's
    // emission.
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
                            "#include <string>", "#include <map>"}) {
        if (emittedIncludes.insert(hdr).second) out << hdr << "\n";
    }
    for (const auto& stmt : program.statements) {
        if (stmt->isExternalDeclaration) continue;
        if (auto inc = dynamic_cast<const IncludeStatement*>(stmt.get())) {
            if (emittedIncludes.insert(inc->includeFile).second) {
                out << inc->includeFile << "\n";
            }
        }
    }
    // Forward-declare (`class X;`) every EXTERNAL class this header's own
    // declarations reference, INDEPENDENT of whatever #include lines the
    // source .clg wrote (emitted right after them, before this header's own
    // declarations begin) -- makes this header's use of `std::shared_ptr<X>`
    // etc. correct regardless of whether X's own real header actually
    // manages to fully expand at this point in the include graph. Found as
    // a real bug via imgui-cling's Context gaining a method that calls a
    // free function from another file (button.clg): button.h's own
    // declarations need `Context` (context.clg) visible, and context.clg's
    // own `#include "button.clh"` (needed only by its .cpp body, not by
    // context.h's own declarations) created a genuine two-header cycle --
    // whichever header's #include-guard fired SECOND, transitively, saw the
    // other's class body already skipped by that header's OWN #ifndef guard
    // and never re-run, so the type was never declared at all by the point
    // it was needed. A forward declaration sidesteps the whole ordering
    // question: `std::shared_ptr<X>` only needs X to be a known INCOMPLETE
    // type at the declaration (not full-definition) sites a header ever
    // has -- CeeLing headers contain ONLY signatures, never bodies that
    // would dot/arrow-access X's members, so a forward declaration is
    // always sufficient here. Never applies to a value-type/struct (e.g.
    // Response embedded/returned BY VALUE) -- those need their real size at
    // declaration time too, so they keep requiring the real #include instead.
    for (const auto& className : collectForwardDeclarableClasses(program)) {
        out << "class " << className << ";\n";
    }
    out << "\n";

    // Same namespace-grouping traversal as emit() (see its comment) -- a header's
    // declarations must nest in the same namespaces the definitions in the paired
    // .cpp do, or the two translation units declare/define different entities.
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
            // Never emits anything in header mode (see emitTopLevelHeader) --
            // stay in whatever namespace is already open instead of
            // needlessly closing/reopening it around a no-op.
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
            // Dual-register under the qualified name (`YAML::Load`) -- a call
            // site outside the namespace spells it qualified, and both
            // calleeParam and exprObjectClass's plain-call branch look these
            // maps up keyed by exactly that spelling.
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
    // Base-before-derived flattening. This is the highest-stakes of the four: unlike
    // the checkers (whose exact-match misses fail SAFE, rejecting valid code),
    // AstEmitter::fieldType returning "" for an inherited field makes
    // exprObjectClass conclude "not a class type", so an inherited smart-pointer
    // field would be emitted with `.` instead of `->` -- silently wrong codegen, not
    // a rejection. Flattening here is what keeps that from happening.
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
        // Count bare-name occurrences first: only a class that genuinely OVERLOADS
        // a method (declares more than one with the same name, e.g. yaml-cpp-style
        // `operator[](string)` + `operator[](int)`) needs the arity-qualified key
        // below -- every other, non-overloaded method keeps the plain bare-name
        // key it always had, so this can't perturb any existing single-method
        // lookup's key shape.
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
                // A specialization registers under its synthetic key ("as<int>"),
                // matching EscapeHatchChecker/DanglingChecker's identical
                // reasoning -- calleeParam below resolves a `.as<int>()` call to
                // this same key via call.templateArg. An OVERLOADED (same-name,
                // different-arity) ordinary method instead keys by
                // overloadKey(name, paramCount), so a second declaration doesn't
                // silently overwrite the first's parameter info (see
                // class_hierarchy.h's overloadKey for the exact scope/limits).
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
                // Every operator= overload's parameter type, tracked
                // independently of the collapsed classMethodParams_ map --
                // see the field's own header comment for why.
                if (method->name == "operator=" && method->parameters.size() == 1) {
                    operatorAssignParamTypes_[cls->name].insert(method->parameters[0].first);
                }
                // The PRIMARY generic (typeParameter set) with T used as a
                // parameter type, and ONLY when T is NOT ALSO the return
                // type -- see outParamIndex_'s comment and DanglingChecker's
                // identical guard for the full rationale (`<T> T as(T
                // fallback)`'s fallback must stay an ordinary by-value
                // parameter, not become a raw-pointer out-parameter).
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

        // Dual-register under the QUALIFIED name too (`YAML::Node` alongside
        // `Node`), so `YAML::Node` used as a TYPE from outside the namespace (a
        // field/parameter/variable declaration, or `new YAML::Node(...)`) resolves
        // through the exact same classNames_/classFields_/classMethodParams_ lookups
        // every other check already uses -- no new lookup call sites needed,
        // matching how base-class member flattening above avoided touching call
        // sites too. Done as a plain copy (not a shared reference) since the two
        // keys never need to diverge after this point.
        if (!cls->namespaceName.empty()) {
            const std::string qualified = cls->namespaceName + "::" + cls->name;
            classNames_.insert(qualified);
            classFields_[qualified] = fields;
            classMethodParams_[qualified] = methodParams;
            classMethodReturnType_[qualified] = methodReturnTypes;
            if (auto opIt = operatorAssignParamTypes_.find(cls->name); opIt != operatorAssignParamTypes_.end()) {
                operatorAssignParamTypes_[qualified] = opIt->second;
            }
            // Same reasoning, for outParamIndex_ -- see DanglingChecker's
            // identical dual-registration for the full rationale (a call
            // through a namespaced receiver, e.g. `YAML::Node n; ...
            // n.decodeInto(p);`, must resolve outParamIndex_ under the
            // QUALIFIED key, since that's what receiverClass/localType_
            // actually hold for such a receiver).
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
    // Classes THIS file declares itself (non-external ClassDeclarations) get
    // their own real, full declaration emitted below -- never forward-declare
    // one of those, that would conflict with (or pointlessly precede) the
    // real thing.
    std::set<std::string> ownClasses;
    for (const auto& stmt : program.statements) {
        if (stmt->isExternalDeclaration) continue;
        if (auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
            ownClasses.insert(cls->name);
        }
    }

    // A candidate type qualifies for forward declaration only if it's a
    // KNOWN CLASS (classNames_, populated by collectClassNames from every
    // merged declaration, own and external alike), not namespace-qualified
    // (`class YAML::Node;` is not valid C++ -- would need `namespace YAML {
    // class Node; }` instead; conservatively skipped rather than handled,
    // since no current consumer needs a namespaced class here), not one of
    // THIS file's own classes, and not a value-type/struct (which needs its
    // real size at declaration time, so it keeps requiring the actual
    // #include instead of a forward declaration).
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
                // v.copyWithin(...) anywhere in the program means the
                // std::memmove-based codegen (emitExpression's FunctionCall
                // branch) will be emitted somewhere, so <cstring> is needed.
                // Not gated on the receiver actually being a vector here --
                // that's checked again at the real emission site, and a
                // false positive here only costs one unused #include, never
                // a missing one.
                if (auto member = dynamic_cast<const MemberAccess*>(call->callee.get());
                    member != nullptr && member->member == "copyWithin") {
                    usesCopyWithin_ = true;
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
            // The bare name may have been registered under an overload key
            // instead (collectClassNames only does this for a genuinely
            // overloaded method name) -- retry with the call's own argument
            // count, which is the same key shape the overloaded method's real
            // arity was registered under.
            methodKey = overloadKey(member->member, call.arguments.size());
            paramIt = clsIt->second.find(methodKey);
        }
        if (paramIt == clsIt->second.end()) return {};
        params = &paramIt->second;
    } else {
        // Plain call `f(args)`.
        auto paramIt = functionParams_.find(call.function_name);
        if (paramIt == functionParams_.end()) return {};
        params = &paramIt->second;
    }
    if (argIndex >= params->size()) return {};
    const auto& [ptype, pname] = (*params)[argIndex];
    return isClassType(ptype) ? std::make_pair(ptype, pname) : std::pair<std::string, std::string>{};
}

std::string AstEmitter::argNeedsExplicitCopyForReadOnlyContainerParam(const FunctionCall& call, size_t argIndex,
                                                                       const std::string& receiverClass,
                                                                       const std::string& methodName) const {
    // Resolve the target parameter's own declared type at argIndex, mirroring
    // calleeParam's own lookup (kept separate rather than widening
    // calleeParam's contract, which is documented class-type-only).
    const std::vector<std::pair<std::string, std::string>>* params = nullptr;
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
    if (!isVectorType(ptype)) return "";
    const bool isReadOnlyContainer = receiverClass.empty()
        ? analyzer_.isReadOnlyContainerParam(call.function_name, pname)
        : analyzer_.isReadOnlyMethodContainerParam(receiverClass, methodName, pname);
    if (!isReadOnlyContainer) return "";

    // Is the argument a caller LOCAL or by-value PARAMETER (never a field
    // access)? Those can never be reached mid-call by the callee (no
    // references to locals outside ext_fn, closures capture by value), so a
    // live reference is always safe regardless of the callee's purity.
    const Expression* arg = call.arguments[argIndex].get();
    if (auto id = dynamic_cast<const Identifier*>(arg)) {
        if (!isCurrentField(id->name)) return "";  // local/param -- reference is safe
    } else if (dynamic_cast<const ThisExpression*>(arg) == nullptr &&
               dynamic_cast<const MemberAccess*>(arg) == nullptr) {
        // Anything else that is not a field/this/member chain (a nested call
        // result, a literal, ...) is not a live alias to begin with -- no
        // copy needed either way; emitMovedArg's existing fallthrough
        // handles it.
        return "";
    }

    // Argument is a field access (or `this` itself, though a container is
    // never bound to `this` directly). Safe to pass by reference only if the
    // callee is proven heap-pure.
    const bool calleeIsPure = receiverClass.empty() ? analyzer_.isHeapPureFreeFunction(call.function_name)
                                                     : analyzer_.isHeapPureMethod(receiverClass, methodName);
    if (calleeIsPure) return "";

    return ptype;  // needs an explicit copy -- return the type to construct it from
}

bool AstEmitter::isCurrentField(const std::string& name) const {
    if (currentClass_.empty()) return false;
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
    // Walk to the chain's leftmost leaf through nested `<<` nodes: `a << b << c`
    // parses left-associatively as `((a << b) << c)`, so the stream (if any) is
    // at the bottom-left. Only `<<` is followed -- a `>>` node is never stream
    // insertion in CeeLing (there is no `istream` type), and stopping at any
    // other operator is what keeps `(x + y) << 2` a shift.
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
    // An `ostream`-typed local/param, e.g. the `out` in a user-written free
    // `ostream operator<<(ostream out, Foo f) { out << ...; }`.
    if (auto id = dynamic_cast<const Identifier*>(leaf)) {
        return ostreamVars_.count(id->name) > 0;
    }
    return false;
}

std::string AstEmitter::exprObjectClass(const Expression* expr) const {
    if (dynamic_cast<const ThisExpression*>(expr) != nullptr) {
        // `this` used as an explicit expression (`this.field`, `this[key]`,
        // `Node self = this;`) resolves to the enclosing class -- `this` is real
        // C++ syntax inside a method body, always a pointer to the current
        // object, so it needs the SAME `->` access every other smart-pointer/
        // observer expression gets. Checked before the Identifier branch below
        // since `this` is now its own AST node type (grammar.js's reserved
        // `this_expression`), never an Identifier at all. Found as a real gap
        // via yaml-cling's convert<T> customization point: a user-written
        // decodeInto<T> specialization calling `this["key"]` or storing
        // `this` into a local both need this to resolve correctly.
        return currentClass_.empty() ? "" : currentClass_;
    }
    if (auto id = dynamic_cast<const Identifier*>(expr)) {
        // A field of the current class (accessed via this->) that is object-typed.
        if (isCurrentField(id->name)) {
            const std::string ft = fieldType(currentClass_, id->name);
            return isClassType(ft) ? ft : "";
        }
        // A local/parameter explicitly classified Value (an ordinary
        // globally-value-typed class like Iterator, OR a local declared
        // from a bare-value-returning specialization call like `Point p =
        // doc.as<Point>();` -- see isBareValueReturningSpecializationCall)
        // is never smart-pointer/`->`-accessed, regardless of what
        // varClass_ separately records for it (varClass_ only tracks the
        // DECLARED TYPE NAME for field-type lookups on `.field` chains, not
        // pointer-vs-value-ness). Checked before the varClass_ branch below
        // so it takes priority.
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
        // `v[i]` yields a container element -- if the container holds class-typed
        // elements, `v[i]` is itself a smart pointer (shared_ptr or unique_ptr, per
        // SmartPointerAnalyzer::containerElementIsShared), so `v[i].field` needs
        // `->` exactly like any other smart-pointer expression.
        const std::string containerType = containerVarType(sub->object.get());
        if (containerType.empty()) return "";
        const std::string elem = vectorElementType(containerType);
        return isClassType(elem) ? elem : "";
    }
    if (auto call = dynamic_cast<const FunctionCall*>(expr)) {
        // `someCall().field` -- the object being accessed is a CALL RESULT, not
        // a variable, so `.`/`->` must be decided from the call's own declared
        // RETURN type, not a varClass_/classFields_ lookup (there is none).
        // Found as a real gap while writing the yaml-cling Parser: member_access
        // gained function_call as a valid left operand (real yaml-cpp-shaped
        // code chains `current().kind`-style access constantly), but nothing
        // told exprObjectClass how to resolve the resulting object's class.
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
        // Bare call, no explicit receiver -- either a free function, or (inside
        // a method body) an implicit-`this` call to a same-class method. Check
        // both the same way isKnownCallable/resolveCallReturnAlias's identical
        // plain-call branches already do elsewhere in this codebase.
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
    // currentSpecializationValueType_ is set only while emitting an external
    // specialization's own signature+body whose return type is its own
    // concrete specialized type (see emitExternalSpecializationMarker) --
    // real C++ requires that specific function's return AND every local of
    // that same type inside its body to be bare/unwrapped (an explicit
    // specialization's return must match the primary's T-substituted type
    // exactly, never a smart-pointer wrapping of it). This does NOT
    // reclassify the type as a value type anywhere else -- ordinary code
    // elsewhere (including this same class used as a field/parameter type
    // in other functions) is completely unaffected, since the flag is
    // cleared the moment this one function's emission finishes.
    if (!currentSpecializationValueType_.empty() && type == currentSpecializationValueType_) {
        return true;
    }
    return analyzer_.isValueTypeClass(type);
}

// A class used as a base needs a callable no-arg constructor for a derived
// class's implicit base-init to find (CeeLing has no base-initializer-list
// syntax -- see the base-injection block in emitClass/emitClassHeader). If the
// user already wrote one (`Animal() { ... }` or a defaulted-by-having-zero-
// params ctor), synthesizing ANOTHER `Animal() = default;` is a duplicate
// declaration -- a real g++ error ("cannot be overloaded with"), confirmed via
// probe: any base class with an explicit `T() { }` constructor failed to
// compile before this check existed. Only synthesize when the user declared
// NO zero-arg constructor of their own.
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
        // containerVarType_ only ever registers LOCALS/PARAMS (see
        // collectClassNames/emitStatement's VariableDeclaration/ContainerDeclaration
        // branches) -- a container-typed class FIELD (`std::vector<Token> out;`,
        // accessed bare via implicit `this` inside a method) was never checked here
        // at all. That silently broke `out.push_back(new Token(...))`: with no
        // container type resolved, receiverIsVector was false, so the
        // element-kind-aware factory path never ran and the generic NewExpression
        // fallback (emitExpression's unconditional make_shared default) fired
        // instead -- producing `std::vector<std::unique_ptr<Token>> out;` declared
        // as unique_ptr but every push_back site emitting make_shared, a real
        // compile error. Fixed by also checking the current class's own fields,
        // the same way isCurrentField/fieldType already do for exprObjectClass.
        if (isCurrentField(id->name)) {
            const std::string ft = fieldType(currentClass_, id->name);
            if (isVectorType(ft)) return ft;
        }
        return "";
    }
    if (auto member = dynamic_cast<const MemberAccess*>(expr)) {
        // `scanner.out[pos]` -- a container FIELD accessed through an
        // EXPLICIT class-typed receiver (`scanner`), not implicit `this`.
        // Found as a real gap while writing yaml-cling's Parser (owns its own
        // Scanner as a field, reads `scanner.out[pos]` throughout): without
        // this, `scanner->out[pos].kind` emitted `.kind` instead of `->kind`,
        // since a std::vector<unique_ptr<Token>>'s operator[] returns a
        // reference to the smart pointer itself, not the pointee -- the same
        // reasoning the bare-field case above already established, extended
        // to an explicit receiver.
        const std::string objClass = exprObjectClass(member->object.get());
        if (objClass.empty()) return "";
        const std::string ft = fieldType(objClass, member->member);
        if (isVectorType(ft)) return ft;
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
        // Plain call, no receiver -- a free function (queried under the ""
        // class, matching emitFunction's own free-function query) or an
        // implicit-`this` call to a same-class method. Found as a real gap
        // while writing yaml-cling's Parser: `Node root = YAML::Load(yaml);`
        // kept declaring `root` as unique_ptr even after Load itself was
        // correctly inferred/emitted as shared_ptr-returning, since this
        // function unconditionally bailed out for any callee-less call.
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
        // `n[key]` on a class-typed receiver desugars to `n.operator[](key)` for
        // emission purposes (see the SubscriptExpression branch in
        // emitExpression) -- the SAME shared-return check applies, just keyed
        // by the synthetic method name "operator[]" instead of a real
        // MemberAccess's member name.
        const std::string objClass = exprObjectClass(sub->object.get());
        if (objClass.empty()) return false;
        return analyzer_.getMethodReturnPointerType(objClass, "operator[]") == PointerType::SHARED;
    }
    return false;
}

// Is `expr` a call to a method specialization (`recv.method<Type>(...)`)
// whose declared return type IS its own concrete specialized type -- e.g.
// `doc.as<Point>()`, where the specialization is `Point Node::as<Point>()`
// (as opposed to `bool`-returning decodeInto<T>)? Such a call's C++ result
// is a bare value (see emitExternalSpecializationMarker/
// currentSpecializationValueType_), never smart-pointer wrapped, so a local
// declared from it (`Point p = doc.as<Point>();`) must be emitted bare too
// -- the ordinary "class-typed method return -> smart pointer" rule does
// not apply to this one, narrow case.
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
    // `byte`/`int64` are the two primitives whose CeeLing spelling is NOT
    // identical to their C++ spelling (every other primitive --
    // int/float/double/bool/char -- happens to share a name with its C++
    // counterpart, which is why none of them need an entry here; see
    // grammar.js's own comments). Checked before isMapType/vectorElementType
    // so `std::vector<byte>`/`std::vector<int64>`'s element still gets
    // lowered correctly via the recursive cppTypeFor(elem, "") call a few
    // lines below.
    if (ceelingType == "byte") return "unsigned char";
    if (ceelingType == "int64") return "long long";
    if (isMapType(ceelingType)) {
        // Map keys/values are restricted to primitives this slice (enforced at parse
        // time in ast_builder.cpp), so neither side ever needs the class-type
        // smart-pointer-wrapping cppTypeFor does for vector elements below -- just
        // recurse so e.g. the value's own `string` -> `std::string` mapping applies.
        return "std::map<" + cppTypeFor(mapKeyType(ceelingType), "")
             + ", " + cppTypeFor(mapValueType(ceelingType), "") + ">";
    }
    const std::string elem = vectorElementType(ceelingType);
    if (elem.empty()) return ceelingType;
    // A `struct`-declared (or Iterator-shaped) value-type element is stored
    // BARE -- `std::vector<Vertex>`, not `std::vector<unique_ptr<Vertex>>` --
    // matching how a value-type LOCAL/field is already unwrapped elsewhere in
    // this file. Checked BEFORE the ordinary class-type smart-pointer wrap
    // below, which would otherwise apply unconditionally to any class-typed
    // element regardless of value-type-ness (confirmed as a real bug via
    // probe: a plain-data Vertex struct was forced into
    // vector<unique_ptr<Vertex>>, scattered heap allocations physically
    // unusable as a contiguous vertex/index buffer).
    if (analyzer_.isValueTypeClass(elem)) {
        return "std::vector<" + elem + ">";
    }
    if (isClassType(elem)) {
        const std::string ptr = analyzer_.containerElementIsShared(varName)
            ? "std::shared_ptr<" : "std::unique_ptr<";
        return "std::vector<" + ptr + elem + ">>";
    }
    // Non-class element (string, a primitive, or a nested map/vector type) --
    // recurse so e.g. `vector<string>` normalizes its element to `std::string`
    // the same way the map branch above already normalizes key/value types.
    // Confirmed as a real bug via probe: `std::vector<string>` (element left
    // as the BARE CeeLing spelling) doesn't compile -- `string` unqualified
    // is not `std::string` outside a `using namespace std;`, which this
    // generator's output never has.
    return "std::vector<" + cppTypeFor(elem, "") + ">";
}

AstEmitter::VarKind AstEmitter::kindOf(const std::string& name) const {
    auto it = varKinds_.find(name);
    return it == varKinds_.end() ? VarKind::Value : it->second;
}

std::string AstEmitter::smartPointerType(const std::string& className,
                                         const std::string& varName) const {
    // `className` is passed through VERBATIM to SmartPointerAnalyzer -- do not
    // strip a namespace qualifier here. SmartPointerAnalyzer::analyzeClassDeclaration
    // dual-registers a namespaced class's NAME (in `classNames`), but a specific
    // local/parameter/field's OWN pointer-kind decision is registered under
    // whichever spelling its declared-type STRING actually used (decl.type /
    // ptype) -- "YAML::Node" for a `YAML::Node a;` local. Stripping here would look
    // up a DIFFERENT (nonexistent) registration and silently fall through to
    // getPointerType's raw SHARED default -- confirmed as a real bug this way (a
    // never-shared namespaced local got shared_ptr instead of unique_ptr).
    switch (analyzer_.getPointerType(className, varName)) {
        case PointerType::UNIQUE: return "std::unique_ptr";
        case PointerType::WEAK:   return "std::weak_ptr";
        default:                  return "std::shared_ptr";
    }
}

std::string AstEmitter::factoryFor(const std::string& className, const std::string& varName,
                                   const std::vector<ExpressionPtr>& args,
                                   const std::string& declaredType) {
    // With inheritance, the CONSTRUCTED type and the DECLARED type differ:
    // `Animal a = new Dog();` must build a `Dog` (make_*<Dog>) but choose its smart-
    // pointer KIND from `a`'s own declared type `Animal` -- asking the analyzer about
    // `Dog` would consult a different variable's ownership decisions entirely, and
    // mismatching the kind produces `unique_ptr<Animal> = make_shared<Dog>(...)`,
    // which does not compile. Without a base clause these are the same string, so
    // non-inheriting programs are unaffected.
    const std::string kindType = declaredType.empty() ? className : declaredType;
    const std::string ptr = smartPointerType(kindType, varName);
    const std::string factory = (ptr == "std::unique_ptr") ? "std::make_unique" : "std::make_shared";
    std::string s = factory + "<" + className + ">(";
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) s += ", ";
        s += emitExpression(args[i].get());
    }
    s += ")";
    return s;
}

// ---- top level -----------------------------------------------------------

void AstEmitter::emitTopLevel(const Statement* stmt, std::ostringstream& out) {
    // Declarations pulled in from an #include'd sibling .clg (for semantic analysis
    // only) are never re-emitted here -- their real definition already lives in that
    // sibling's own .cpp/.clh. See Statement::isExternalDeclaration.
    if (stmt->isExternalDeclaration) return;
    if (auto marker = dynamic_cast<const ExternalSpecializationMarker*>(stmt)) {
        if (auto it = classByName_.find(marker->targetClassName); it != classByName_.end()) {
            emitExternalSpecializationMarker(*marker, *it->second, out);
        }
        // An unresolvable target here would already have been rejected as
        // error[semantic/unknown-specialization-target] during AST building
        // (see ASTBuilder::buildProgram's splice pass) -- this branch cannot
        // be reached with a dangling targetClassName in practice.
        return;
    }
    if (auto cls = dynamic_cast<const ClassDeclaration*>(stmt)) {
        emitClass(*cls, out);
    } else if (auto enm = dynamic_cast<const EnumDeclaration*>(stmt)) {
        // Unscoped enum -- see EnumDeclaration's comment for why (implicit
        // enumerator->int conversion, matching how a CeeLing enum-typed value is
        // stored/passed as a plain int).
        out << "enum " << enm->name << " { ";
        for (size_t i = 0; i < enm->enumerators.size(); ++i) {
            if (i > 0) out << ", ";
            out << enm->enumerators[i];
        }
        out << " };\n\n";
    } else if (auto fn = dynamic_cast<const FunctionDeclaration*>(stmt)) {
        // methodName must be passed explicitly (defaults to "" otherwise) --
        // it's what currentMethodReturnsShared_ actually queries
        // getMethodReturnPointerType with (ownerClass="", methodName=fn->name
        // for a free function). Without this, a genuinely alias-returning
        // free function like yaml-cling's `Node Load(string) { ... return
        // p->parseDocument(); }` silently stayed unique_ptr -- confirmed via
        // debug instrumentation that inferAliasReturningFreeFunctions
        // correctly classified it SHARED, but emitFunction's own shared-check
        // never saw that classification because methodName was never wired
        // through from this, the ONLY free-function call site.
        emitFunction(fn->returnType, fn->name, fn->parameters, fn->body, out,
                     "", 0, false, false, false, fn->name);
    } else if (auto extFn = dynamic_cast<const ExternFunctionDeclaration*>(stmt)) {
        // ext_fn emits ONLY a forward declaration -- never a body. The real
        // implementation is hand-written C++ the user links in separately
        // (reached via an ordinary #include, exactly like imgui-cling's
        // gfx_shim.h pattern -- see library_summary.cpp's
        // isKnownQualifiedFreeFunction doc comment). CeeLing emits the trust
        // boundary, never fabricates a body for it.
        varKinds_.clear(); varClass_.clear(); observerClass_.clear(); containerVarType_.clear();
        currentExternRefParams_ = extFn->refParams;
        std::string sig = buildFunctionSignature(extFn->returnType, extFn->name,
                                                  extFn->parameters, "", false, false, false, extFn->name);
        currentExternRefParams_.clear();
        out << sig << ";\n\n";
    } else if (auto mn = dynamic_cast<const MainFunction*>(stmt)) {
        emitFunction(mn->returnType, "main", {}, mn->body, out);
    } else if (dynamic_cast<const IncludeStatement*>(stmt)) {
        // All includes (required std headers + any the source declared) are collected
        // and emitted once, de-duplicated, at the top of emit(). Nothing to do here.
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

void AstEmitter::emitClass(const ClassDeclaration& cls, std::ostringstream& out) {
    const std::string guard = perClassGuardName(cls.name);
    out << "#ifndef " << guard << "\n#define " << guard << "\n";
    out << "class " << cls.name;
    // `std::enable_shared_from_this<T>` is a SEPARATE, compiler-generated base from
    // the user's own `cls.baseClass` (single inheritance) -- auto-derived whenever
    // some method's `@ptr shared` return contains `return this;` (see
    // SmartPointerAnalyzer::classNeedsSharedFromThis), never user-annotated. Comma-
    // joins with an ordinary base clause if both happen to apply (real C++ allows
    // multiple base specifiers here; this combination just isn't exercised by any
    // current fixture).
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

    // Two things the compiler generates for any class used as a base, both invisible
    // to the user (CeeLing has no destructor or initializer-list syntax, so neither
    // can conflict with something the user wrote):
    //
    //  * `virtual ~Base() = default;` -- destroying a Derived through a
    //    `unique_ptr<Base>` is undefined behavior without it. A real memory-safety
    //    hole, closed by construction rather than by a rule users must remember.
    //  * a protected no-arg constructor -- a derived constructor implicitly calls the
    //    base's no-arg constructor, but declaring ANY constructor (e.g. `Animal(int)`)
    //    suppresses C++'s implicit default one. CeeLing has no base-initializer-list
    //    syntax (`Dog(int a) : Animal(a)`), and deliberately does not add one: a
    //    derived constructor just assigns the inherited fields directly in its own
    //    body (`age = a;` already lowers to `this->age = a;` thanks to member
    //    flattening). `protected` keeps it an implementation detail -- it exists for
    //    derived classes to chain through, not for users to call.
    //    Only synthesized when the user did NOT already write their own zero-arg
    //    constructor -- see hasUserDefinedDefaultConstructor's own comment for the
    //    real "cannot be overloaded with" compile error this guards against.
    if (baseClasses_.count(cls.name) > 0) {
        out << indentStr(1) << "virtual ~" << cls.name << "() = default;\n";
        if (!hasUserDefinedDefaultConstructor(cls)) {
            out << "protected:\n";
            out << indentStr(1) << cls.name << "() = default;\n";
            out << "public:\n";
        }
    }

    // Every constructor/method/specialization is DECLARED here (signature only) and
    // DEFINED out-of-line (`ReturnType ClassName::method(...) {...}`) after the
    // closing `};` -- the same shape the paired header (emitClassHeader) already
    // declares, so the two stay structurally in sync. This matters beyond style: an
    // in-class-body definition is implicitly `inline`, and the compiler is free to
    // drop an unused inline function's code from this TU's object file entirely --
    // which silently breaks a SEPARATE translation unit (one that only sees the
    // header's signature-only declaration) at link time with "undefined reference",
    // since it expected an out-of-line definition to exist somewhere. Out-of-line
    // definitions here are ordinary (non-inline) external-linkage functions, always
    // emitted, exactly what a translation-unit-per-.clg-file model needs. The primary
    // generic template (`typeParameter` non-empty) is the one exception, kept
    // in-class: an ordinary function template's definition must be visible at every
    // instantiation point, the same requirement that makes emitClassHeader keep it
    // in the header instead of declaration-only.
    std::vector<const ConstructorDeclaration*> outOfLineCtors;
    std::vector<const MethodDeclaration*> outOfLineMethods;
    std::vector<const MethodDeclaration*> specializations;

    for (const auto& member : cls.members) {
        if (auto var = dynamic_cast<const VariableDeclaration*>(member.get())) {
            // Member field. Object-typed fields become smart pointers, EXCEPT a
            // value-type class field (Iterator etc.), which stays a plain,
            // unwrapped member -- consistent with every other value-type site.
            if (isValueType(var->type)) {
                out << indentStr(1) << var->type << " " << var->identifier << ";\n";
            } else if (isClassType(var->type)) {
                out << indentStr(1) << smartPointerType(var->type, var->identifier)
                    << "<" << var->type << "> " << var->identifier << ";\n";
            } else {
                out << indentStr(1) << cppTypeFor(var->type, var->identifier) << " "
                    << var->identifier << ";\n";
            }
        } else if (auto ctor = dynamic_cast<const ConstructorDeclaration*>(member.get())) {
            varKinds_.clear(); varClass_.clear(); observerClass_.clear(); containerVarType_.clear();
            std::string sig = buildFunctionSignature("", ctor->name, ctor->parameters, cls.name,
                                                      false, false, false, "");
            out << indentStr(1) << sig << ";\n";
            outOfLineCtors.push_back(ctor);
        } else if (auto method = dynamic_cast<const MethodDeclaration*>(member.get())) {
            if (!method->specializationArg.empty()) {
                if (method->isExternalSpecialization) {
                    // An EXTERNAL specialization (yaml-cpp's convert<T>
                    // shape, `ReturnType ClassName::method<Type>(...) {...}`
                    // declared OUTSIDE the class) may reference a type
                    // declared LATER in this program relative to the target
                    // class (e.g. a consumer's own struct, defined after
                    // #include "yaml_node.clh" but specializing Node::as<T>
                    // for it -- and in the #include case, the target class
                    // itself is merged in FIRST, before any of the
                    // consumer's own statements, so "after the target
                    // class's own body" is never a safe emission point).
                    // Nothing is emitted here at all: both the declaration
                    // and the definition are emitted by
                    // flushExternalSpecializationMarker, at the
                    // ExternalSpecializationMarker's OWN top-level source
                    // position (see ast.h) -- the one position ordinary
                    // source-order rules guarantee is after every type this
                    // specialization's signature/body can reference, since
                    // CeeLing has no forward declarations and the consumer
                    // could not have named an undeclared type in their own
                    // source.
                } else {
                    // An ORDINARY (in-class) specialization has no such
                    // problem -- it can only ever have been written after
                    // its own class's other members lexically, so nothing
                    // later in program order could be needed by it that
                    // isn't already needed by the class itself.
                    specializations.push_back(method);
                }
                continue;
            }
            if (!method->typeParameter.empty()) {
                // Must stay visible at every instantiation point -- see comment above.
                out << indentStr(1) << "template<typename " << method->typeParameter << ">\n";
                emitFunction(method->returnType, method->name, method->parameters, method->body,
                             out, cls.name, 1, method->isConst, method->isVirtual, method->isOverride,
                             method->name);
                continue;
            }
            varKinds_.clear(); varClass_.clear(); observerClass_.clear(); containerVarType_.clear();
            std::string sig = buildFunctionSignature(method->returnType, method->name, method->parameters,
                                                      cls.name, method->isConst, method->isVirtual,
                                                      method->isOverride, method->name);
            out << indentStr(1) << sig << ";\n";
            outOfLineMethods.push_back(method);
        }
    }
    out << "};\n#endif // " << guard << "\n\n";

    for (const auto* ctor : outOfLineCtors) {
        emitFunction("", cls.name + "::" + ctor->name, ctor->parameters, ctor->body, out, cls.name, 0,
                     false, false, false, "", /*isConstructor=*/true);
    }
    for (const auto* method : outOfLineMethods) {
        // `isVirtual`/`isOverride` are NOT repeated out-of-line -- both are class-body-
        // only specifiers in real C++ (a redefinition would be a compile error), so
        // buildFunctionSignature's virtual/override text must not appear here; only
        // the in-class declaration above carries them.
        emitFunction(method->returnType, cls.name + "::" + method->name, method->parameters,
                     method->body, out, cls.name, 0, method->isConst, false, false, method->name);
    }
    // Specializations (`int as<int>() {...}`) DEFINE here, same as any other
    // out-of-line method -- exactly ONE .cpp may define a given explicit
    // specialization (ODR), so this is their one true home. The paired header
    // (emitClassHeader) only DECLARES each specialization (`template<> T
    // Node::as<T>();`, no body) -- a specialization declaration (unlike an ordinary
    // template) does not need to be visible at every instantiation point, only
    // resolvable at link time, so declaration-only in the header + one definition
    // here is both sufficient and ODR-safe. A single-file build with no paired
    // header (plain `emit()`, no `.h` ever generated) still works unchanged: the
    // specialization is simply defined and used within the same translation unit.
    for (const auto* method : specializations) {
        out << "template<>\n";
        // The `<int>`/`<std::string>` in the qualified name must match the emitted
        // C++ type spelling (cppTypeFor's string->std::string mapping etc.), not the
        // raw CeeLing source token, or the specialization silently fails to bind to
        // its call sites (a second, distinct overload instead of a specialization).
        emitFunction(method->returnType,
                     cls.name + "::" + method->name + "<" + cppTypeFor(method->specializationArg, "") + ">",
                     method->parameters, method->body, out, cls.name, 0, method->isConst);
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

    // Specializations (`int as<int>() {...}`), like in emitClass, are listed OUTSIDE
    // the class body -- but here as DECLARATIONS only (`template<> T Node::as<T>();`,
    // no body). Unlike an ordinary template, whose definition must be visible at
    // every instantiation point, an explicit specialization only needs to be
    // DECLARED before use; its one definition lives in the paired .cpp (emitClass),
    // resolved at link time like any other out-of-line method. This is what lets a
    // consumer that only sees this header (never yaml_node.cpp) still call
    // `node.as<int>()` and link successfully against the separately compiled
    // yaml_node.o.
    std::vector<const MethodDeclaration*> specializations;

    for (const auto& member : cls.members) {
        if (auto var = dynamic_cast<const VariableDeclaration*>(member.get())) {
            if (isValueType(var->type)) {
                out << indentStr(1) << var->type << " " << var->identifier << ";\n";
            } else if (isClassType(var->type)) {
                out << indentStr(1) << smartPointerType(var->type, var->identifier)
                    << "<" << var->type << "> " << var->identifier << ";\n";
            } else {
                out << indentStr(1) << cppTypeFor(var->type, var->identifier) << " "
                    << var->identifier << ";\n";
            }
        } else if (auto ctor = dynamic_cast<const ConstructorDeclaration*>(member.get())) {
            varKinds_.clear(); varClass_.clear(); observerClass_.clear(); containerVarType_.clear();
            std::string sig = buildFunctionSignature("", ctor->name, ctor->parameters, cls.name,
                                                      false, false, false, "");
            out << indentStr(1) << sig << ";\n";
        } else if (auto method = dynamic_cast<const MethodDeclaration*>(member.get())) {
            if (!method->specializationArg.empty()) {
                specializations.push_back(method);
                continue;
            }
            if (!method->typeParameter.empty()) {
                // The primary generic template's body must stay visible at every
                // instantiation point -- same requirement as any C++ function
                // template -- so it is emitted in full here, not declaration-only.
                out << indentStr(1) << "template<typename " << method->typeParameter << ">\n";
                emitFunction(method->returnType, method->name, method->parameters, method->body,
                             out, cls.name, 1, method->isConst, method->isVirtual, method->isOverride,
                             method->name);
                continue;
            }
            varKinds_.clear(); varClass_.clear(); observerClass_.clear(); containerVarType_.clear();
            std::string sig = buildFunctionSignature(method->returnType, method->name, method->parameters,
                                                      cls.name, method->isConst, method->isVirtual,
                                                      method->isOverride, method->name);
            out << indentStr(1) << sig << ";\n";
        }
    }
    out << "};\n#endif // " << guard << "\n\n";

    for (const auto* method : specializations) {
        // Declaration only: `template<> ReturnType Node::as<int>(params);` -- build
        // the signature the same way the in-class declarations above do, but at
        // namespace scope with the qualified/specialized name, and no body.
        varKinds_.clear(); varClass_.clear(); observerClass_.clear(); containerVarType_.clear();
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
        // Already a pure declaration -- emitted identically to emitTopLevel.
        out << "enum " << enm->name << " { ";
        for (size_t i = 0; i < enm->enumerators.size(); ++i) {
            if (i > 0) out << ", ";
            out << enm->enumerators[i];
        }
        out << " };\n\n";
    } else if (auto fn = dynamic_cast<const FunctionDeclaration*>(stmt)) {
        varKinds_.clear(); varClass_.clear(); observerClass_.clear(); containerVarType_.clear();
        std::string sig = buildFunctionSignature(fn->returnType, fn->name, fn->parameters,
                                                  "", false, false, false, fn->name);
        out << sig << ";\n\n";
    } else if (auto extFn = dynamic_cast<const ExternFunctionDeclaration*>(stmt)) {
        // Identical shape to an ordinary FunctionDeclaration's header
        // declaration above -- an ext_fn already IS just a signature, so
        // header emission needs no special-casing beyond reusing the same
        // buildFunctionSignature call.
        varKinds_.clear(); varClass_.clear(); observerClass_.clear(); containerVarType_.clear();
        currentExternRefParams_ = extFn->refParams;
        std::string sig = buildFunctionSignature(extFn->returnType, extFn->name, extFn->parameters,
                                                  "", false, false, false, extFn->name);
        currentExternRefParams_.clear();
        out << sig << ";\n\n";
    } else if (dynamic_cast<const MainFunction*>(stmt)) {
        // A header never declares main -- the definition lives only in the one .cpp
        // that has one (a program may have at most one main across all linked TUs).
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
    // getMethodReturnPointerType defaults to UNIQUE unless the alias-return
    // inference actually classified this (class, method) pair SHARED, so an
    // ordinary object-returning method/function's behavior is unaffected
    // unless its body genuinely matches the inference rule. A free function
    // (ownerClass empty) queries under the SAME "" class key
    // inferAliasReturningMethods/promoteReturnsOfSharedLocals register free
    // functions under -- needed for `Node Load(string) { ... return
    // p->parseDocument(); }`-shaped code (yaml-cling's YAML::Load), which is
    // exactly as alias-returning as any class method but has no owner class
    // to key by.
    currentMethodReturnsShared_ = !methodName.empty() &&
        analyzer_.getMethodReturnPointerType(ownerClass, methodName) == PointerType::SHARED;
    currentMethodReturnsValueType_ = isValueType(returnType);

    // An object return type is lowered to a smart pointer so the body's owning local
    // (a smart pointer) can be returned directly (unique_ptr by default: the callee
    // constructs the object and hands sole ownership back to the caller; shared_ptr
    // only when an explicit @ptr hint says this method's return aliases rather than
    // transfers -- see the ReturnStatement branch in emitStatement for the matching
    // copy-vs-move choice). A value-type class (Iterator etc.) return is a THIRD
    // case, checked first: plain, unwrapped -- required for the C++ range-for
    // protocol, which needs begin()/end() to return the iterator by bare value, not
    // wrapped in a smart pointer (std::shared_ptr/unique_ptr have no operator++).
    std::string sig;
    if (isVirtual) sig += "virtual ";
    if (!returnType.empty()) {
        if (isOstreamType(returnType)) {
            // std::ostream's copy constructor is deleted -- by-value is not just
            // suboptimal here the way it is for a value-type class, it does not
            // compile at all. Checked FIRST (before value-type/class-type), since
            // "ostream" would otherwise fall into the generic non-class-type
            // cppTypeFor(...) branch below, which emits the bare, unqualified,
            // by-value type name -- invalid C++ for this specific type.
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
            // Mutable out-parameter (see outParamIndex_): lowered exactly
            // like a `ref` observer local (VarKind::Reference) -- a raw,
            // non-owning pointer, `->` access in the body, the call site
            // passes `.get()` instead of moving. Applies identically
            // whether ptype is the primary's own literal `T` (emits `T*
            // pname`, valid template-parameter syntax) or a specialization's
            // concrete type (`Point* pname`) -- cppTypeFor returns a type
            // string verbatim for either, so no isClassType branch is
            // needed here.
            sig += ptype + "* " + pname;
            noteVar(pname, VarKind::Reference);
            observerClass_[pname] = ptype;
            continue;
        }
        if (isOstreamType(ptype)) {
            // Same reasoning as the return-type branch above -- must be a
            // reference, std::ostream cannot be passed by value at all.
            sig += "std::ostream& " + pname;
            noteVar(pname, VarKind::Value);
            // Remember it IS a stream, so `out << x` inside the body is emitted
            // as a stream chain rather than as an integer shift (both are the
            // same `<<` BinaryExpression now -- see isStreamChain).
            ostreamVars_.insert(pname);
        } else if (isValueType(ptype)) {
            // Passed by value, like an ordinary primitive -- value types are meant
            // to be cheap to copy (e.g. an iterator wrapping a single Node handle).
            sig += ptype + " " + pname;
            noteVar(pname, VarKind::Value);
            varClass_[pname] = ptype;
        } else if (isClassType(ptype)) {
            // A parameter's smart-pointer type is derived by SmartPointerAnalyzer
            // exactly like a local's (it registers parameters in its first pass now
            // -- see analyzeStatement), so it reflects real usage inside the body
            // (e.g. stored into a shared field vs. exclusively owned). A fixed
            // choice here regardless of usage/call-site type produced a mismatch in
            // one direction or the other (shared_ptr broke unique_ptr-passing call
            // sites; unique_ptr broke bodies that store the parameter into a shared
            // field).
            sig += smartPointerType(ptype, pname) + "<" + ptype + "> " + pname;
            noteVar(pname, VarKind::SmartPtr);
            varClass_[pname] = ptype;
        } else if (currentExternRefParams_.count(i)) {
            // ext_fn `ref` parameter (see ExternFunctionDeclaration::refParams
            // and currentExternRefParams_'s comment in the header): a
            // container is a read-only borrow (const&, cannot reallocate --
            // no iterator-invalidation hazard), a primitive is a mutable
            // out-param (&, an int has no provenance so writing it back can't
            // dangle). ast_builder.cpp already rejected every other type at
            // parse time, so only these two shapes reach here.
            if (isVectorType(ptype)) {
                sig += "const " + cppTypeFor(ptype, pname) + "& " + pname;
                containerVarType_[pname] = ptype;
            } else {
                sig += cppTypeFor(ptype, pname) + "& " + pname;
            }
            noteVar(pname, VarKind::Value);
        } else if (isVectorType(ptype) &&
                   (ownerClass.empty() ? analyzer_.isReadOnlyContainerParam(name, pname)
                                       : analyzer_.isReadOnlyMethodContainerParam(ownerClass, methodName, pname))) {
            // Inferred read-only container parameter (see
            // SmartPointerAnalyzer::classifyReadOnlyContainerParams) -- the
            // language-level fix for the by-value-container-copy cost the
            // ext_fn `ref` mechanism above only fixes for hand-declared
            // externs. Lowered exactly like an ext_fn `ref` container:
            // `const T&`, never a mutable `T&` (a mutable container
            // reference stays forbidden everywhere -- reallocation would
            // invalidate every existing observer, CLAUDE.md threat #4).
            // Call-site emission (emitMovedArg) must NOT move this argument
            // even when the callee is "not heap-pure" for a NON-local
            // argument -- that impurity gate controls whether the CALL SITE
            // passes a live reference or an explicit copy, not whether the
            // CALLEE's own parameter type here is a reference; the
            // parameter is always `const&` once classified read-only.
            sig += "const " + cppTypeFor(ptype, pname) + "& " + pname;
            noteVar(pname, VarKind::Value);
            containerVarType_[pname] = ptype;
        } else {
            sig += cppTypeFor(ptype, pname) + " " + pname;
            noteVar(pname, VarKind::Value);
            if (isVectorType(ptype)) {
                containerVarType_[pname] = ptype;
            }
        }
    }
    sig += ")";
    if (isConst) sig += " const";
    if (isOverride) sig += " override";
    return sig;
}

void AstEmitter::emitFunction(const std::string& returnType, const std::string& name,
                              const std::vector<std::pair<std::string, std::string>>& params,
                              const std::vector<StatementPtr>& body, std::ostringstream& out,
                              const std::string& ownerClass, int baseIndent, bool isConst,
                              bool isVirtual, bool isOverride, const std::string& methodName,
                              bool isConstructor) {
    // Each function has its own local variable scope; `currentClass_` enables `this->`
    // for field access inside a method/constructor.
    varKinds_.clear();
    varClass_.clear();
    observerClass_.clear();
    containerVarType_.clear();
    const std::string savedClass = currentClass_;
    currentClass_ = ownerClass;
    const bool savedReturnsShared = currentMethodReturnsShared_;
    const bool savedReturnsValueType = currentMethodReturnsValueType_;
    const std::string savedFunctionName = currentFunctionName_;
    const std::set<std::string> savedParamNames = currentParamNames_;
    currentFunctionName_ = methodName.empty() ? name : methodName;
    currentParamNames_.clear();
    for (const auto& [ptype, pname] : params) currentParamNames_.insert(pname);

    std::string sig = buildFunctionSignature(returnType, name, params, ownerClass, isConst,
                                             isVirtual, isOverride, methodName);

    // Member-initializer-list hoisting (constructors only -- see this function's
    // header comment and selectAssignmentRhs's for the full rationale). Walk the
    // body from the start, hoisting a LEADING RUN of plain assignments to this
    // class's own fields; stop at the first statement that doesn't qualify (a
    // non-assignment, a member/subscript-target assignment, an assignment to a
    // field this constructor already hoisted, or a field NOT declared directly
    // in this class -- e.g. an inherited field, which cannot legally appear in
    // this class's own initializer list). Everything from that point on stays an
    // ordinary body statement, in original order, exactly as before this change.
    std::set<size_t> hoistedIndices;
    std::string initList;
    if (isConstructor) {
        // Own-field names only (NOT classFields_/isCurrentField, which are
        // flattened with inherited fields merged in -- using them here would
        // wrongly try to hoist an inherited field's assignment into a
        // derived-class initializer list, which does not compile). Walking
        // cls->members directly mirrors the exact pattern emitClass/
        // emitClassHeader already use for field declarations.
        std::set<std::string> ownFieldNames;
        if (auto clsIt = classByName_.find(ownerClass); clsIt != classByName_.end()) {
            for (const auto& member : clsIt->second->members) {
                if (auto field = dynamic_cast<const VariableDeclaration*>(member.get())) {
                    ownFieldNames.insert(field->identifier);
                }
            }
        }
        std::set<std::string> hoistedFields;
        for (size_t i = 0; i < body.size(); ++i) {
            auto assign = dynamic_cast<const AssignmentStatement*>(body[i].get());
            if (assign == nullptr || assign->target != nullptr) break;  // non-assignment, or member/subscript target
            if (ownFieldNames.count(assign->variable) == 0) break;      // not this class's own field
            if (hoistedFields.count(assign->variable) > 0) break;       // already hoisted once
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
    currentFunctionName_ = savedFunctionName;
    currentParamNames_ = savedParamNames;
}

std::string AstEmitter::emitMovedArg(const Expression* arg,
                                     const std::pair<std::string, std::string>& targetParam) {
    if (dynamic_cast<const ThisExpression*>(arg) != nullptr && !currentClass_.empty()) {
        // `this` passed as a call argument, a VariableDeclaration RHS, or an
        // AssignmentStatement RHS (this function is the shared lowering point
        // for all three -- see its own call sites) shares ownership of the
        // current object, exactly like `return this;` already does (Chapter
        // 18 Step 37): shared_from_this() aliases the object's OWN existing
        // shared_ptr control block, never a second independent one. Requires
        // the enclosing class to inherit std::enable_shared_from_this and
        // always be constructed via make_shared -- both forced by
        // SmartPointerAnalyzer::methodPassesThisAsArgument's detection scan,
        // the argument-position counterpart of the return-position
        // anyReturnsThis scan that already backs `return this;`.
        // EscapeHatchChecker separately hard-rejects this shape from inside a
        // constructor, where shared_from_this() would throw std::bad_weak_ptr
        // unconditionally (the control block does not exist yet) -- by the
        // time this runs, that case has already failed to compile.
        return "shared_from_this()";
    }
    if (auto id = dynamic_cast<const Identifier*>(arg)) {
        // Object-typed local or parameter (smart pointer): transfer ownership.
        // Field access and references are not moved here.
        if (auto it = varClass_.find(id->name); it != varClass_.end() && !isCurrentField(id->name)) {
            // A shared_ptr is always safely copyable -- once SmartPointerAnalyzer's
            // parameter-storage promotion (Chapter 18) has decided this variable is
            // SHARED, moving it here would leave the CALLER's own local null, which
            // defeats the entire point of the promotion (the checker proved this
            // argument survives the call; std::move-ing it anyway would silently
            // violate that). Emit a plain copy instead -- cheap (a refcount bump),
            // and the only way "b stays usable after a.attach(b)" is actually true
            // in the generated code, not just in DanglingChecker's model.
            if (analyzer_.getPointerType(it->second, id->name) == PointerType::SHARED) {
                return id->name;
            }
            return "std::move(" + id->name + ")";
        }
        // std::vector<T>-typed local or parameter (containerVarType_ is only
        // ever populated for isVectorType -- see its five populating sites --
        // so this branch never fires for a map or any other type), in a
        // DECLARATION or CALL-ARGUMENT position (never an assignment target
        // -- selectAssignmentRhs decides that separately, see its own
        // comment on why a field-assignment RHS must NOT reach this branch):
        // DanglingChecker treats a vector identifier exactly like an object
        // local in both of those positions -- isObjectType
        // (dangling_checker.cpp) includes isVectorType, and
        // applyCallArgMoves' final `if (std::string src =
        // asObjectLocalName(...)) movedFrom_.insert(src);` fallthrough (and
        // the matching VariableDeclaration branch) mark the source
        // unconditionally moved-from once it reaches either position -- the
        // same guarantee the varClass_ branch above already relies on. This
        // branch was missing entirely, so `Node b = f(bytes);`-style
        // declarations and `f(bytes)`-style call arguments (`bytes` a
        // vector) silently emitted a COPY of the whole container instead of
        // a move.
        //
        // A read-only-container-classified argument at a CALL site never
        // reaches this branch: the call-argument loop's
        // argNeedsExplicitCopyForReadOnlyContainerParam check and the
        // read-only continue in applyCallArgMoves are keyed on the CALLEE's
        // parameter classification for THIS call, so those call sites
        // `continue`/emit their own copy before ever calling emitMovedArg.
        if (auto ctIt = containerVarType_.find(id->name);
            ctIt != containerVarType_.end() && !isCurrentField(id->name)) {
            return "std::move(" + id->name + ")";
        }
    }
    if (!targetParam.first.empty()) {
        if (auto neww = dynamic_cast<const NewExpression*>(arg)) {
            // A bare `new T(...)` argument has no variable of its own to key its
            // pointer type off (unlike `Node x = new Node(1);`, which uses x's
            // analyzer decision) -- use the receiving parameter's own (class, name)
            // to decide the smart-pointer KIND, exactly like factoryFor's own
            // declared-vs-constructed split (see its doc comment). Bug found via
            // probe: this used to pass targetParam.first as factoryFor's
            // CONSTRUCTED-type argument instead of its declared-type argument,
            // so `useIt(new Dog())` into `void useIt(Animal a)` silently built
            // `std::make_shared<Animal>(...)` -- constructing the WRONG CLASS
            // entirely (compiles, links, runs, and silently does something other
            // than what the source says -- the same class of bug
            // inherit_virtual_dispatch_accepted.clg's own reason block warns
            // about). neww->className (the type actually being constructed) must
            // stay the factory's className; targetParam.first (the callee's
            // DECLARED parameter type) is the declaredType that picks the
            // pointer KIND, matching exactly how `Animal a = new Dog();` already
            // works for a named local.
            return factoryFor(neww->className, targetParam.second, neww->arguments, targetParam.first);
        }
    }
    return emitExpression(arg);
}

// ---- statements ----------------------------------------------------------

// Extracted from emitStatement's own AssignmentStatement branch (which used to
// hand-inline this whole decision tree) so a hoisted member-initializer-list
// entry (emitFunction, constructor-only) computes its RHS text through the
// EXACT same logic an ordinary body assignment does -- the two paths must
// never independently decide "what does `field = expr;` lower to," or a
// future bugfix to one would silently miss the other. See emitFunction's own
// comment for what motivated hoisting in the first place: a value-type field
// (`isValueType`) assigned via ordinary body assignment still requires C++ to
// default-construct it first (before the constructor body runs), which fails
// outright for any value type with no zero-arg constructor -- confirmed via a
// real g++ error building imgui-cling's `Color` (nested inside
// `ColorEditResult`'s own field). A member-initializer entry (`field(rhs)`)
// constructs the field FROM rhs directly and never default-constructs it,
// making that failure mode structurally impossible regardless of the field
// type's own constructors.
std::string AstEmitter::selectAssignmentRhs(const AssignmentStatement* assign, const std::string& ptrVar,
                                            bool memberTarget, bool subscriptTarget) {
    std::string rhs;
    const bool rhsIsNullptr = isNullptrLiteral(assign->expression.get());
    if (subscriptTarget) {
        // `m[key] = value;` (std::map insert-or-update, Phase 2.6 map slice): the
        // value is always a primitive this slice (enforced at parse time), so it's
        // a plain copy -- no move/factory/weak_ptr logic applies, the same reason
        // a member-target store doesn't consume its RHS.
        rhs = emitExpression(assign->expression.get());
    } else if (!memberTarget && kindOf(assign->variable) == VarKind::Reference) {
        // `obs = b;` REBINDS the observer to a different object -- it does not
        // move/consume `b` (matches DanglingChecker's model: rebinding an
        // observer never marks the RHS moved-from). Lowered the same way as the
        // declaration: `.get()` extracts the new address, or `nullptr` directly.
        rhs = rhsIsNullptr ? "nullptr" : "(" + emitExpression(assign->expression.get()) + ").get()";
    } else if (auto neww = dynamic_cast<const NewExpression*>(assign->expression.get())) {
        if (isValueType(neww->className)) {
            // `target = new T(...)` for a value-type class: plain constructor
            // call, matching every other value-type construction site.
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
        // weak_ptr has no operator=(nullptr_t); default-construct instead (an empty
        // weak_ptr, the same meaning as "nullptr" for the other smart-pointer kinds).
        rhs = "{}";
    } else if (memberTarget || isCurrentField(assign->variable)) {
        // `a.b = obj;` OR a bare identifier naming a field with implicit `this`
        // (`data = bytes;`) links/shares into a field -- it does not consume
        // `obj`/`bytes` (DanglingChecker's own AssignmentStatement branch
        // gives a bare field store this exact "link, don't move" semantics --
        // see its `isBareFieldStore` handling -- precisely so a linked-list
        // node can be attached to a field and used again right after; a
        // container-typed RHS is no different in kind from an object-typed
        // one here). This must stay emitExpression (a plain copy), never
        // emitMovedArg -- calling emitMovedArg here would std::move a
        // container the checker has explicitly promised stays live after
        // this statement, silently breaking that promise (found via a direct
        // probe while implementing the containerVarType_ branch in
        // emitMovedArg below: `data = bytes; std::cout << bytes.size();`
        // compiled and ran with bytes already moved-from, printing 0 instead
        // of the real size, with no rejection from the checker -- exactly
        // the "checker allows it, emitter breaks it" failure mode, not a
        // move-then-use gap the checker could ever have caught after the
        // fact).
        //
        // EXCEPT: if the RHS is a bare identifier naming one of THIS
        // function/method's own std::vector<primitive> parameters, and
        // SmartPointerAnalyzer::classifyLastUseFieldAssignedParams has
        // proven this exact assignment is that parameter's provable last
        // use in the body (isLastUseFieldAssignedParam/
        // isLastUseMethodFieldAssignedParam), moving it here is safe even
        // though the checker's own general field-store model doesn't need
        // it to be -- this is a STRICTLY NARROWER, additional guarantee on
        // top of the checker's, not a relaxation of it (the checker's
        // "stays live" promise is about what CAN happen; this classification
        // separately proves nothing DOES happen afterward in THIS specific
        // body). Motivating case: zlib-cling's `pushAll(std::vector<byte>
        // bytes) { input = bytes; }`, whose whole body is this one
        // statement -- `bytes` provably has no use after it, so copying the
        // whole buffer here was always wasted work.
        auto rhsId = dynamic_cast<const Identifier*>(assign->expression.get());
        const bool rhsIsLastUseParam = rhsId != nullptr && currentParamNames_.count(rhsId->name) > 0 &&
            (currentClass_.empty() ? analyzer_.isLastUseFieldAssignedParam(currentFunctionName_, rhsId->name)
                                    : analyzer_.isLastUseMethodFieldAssignedParam(currentClass_, currentFunctionName_,
                                                                                   rhsId->name));
        rhs = rhsIsLastUseParam ? emitMovedArg(assign->expression.get())
                                  : emitExpression(assign->expression.get());
    } else if (!ptrVar.empty() && isValueType(varClass_.count(ptrVar) ? varClass_.at(ptrVar) : "")) {
        // `x = a;` where `x` is a value-type class: a plain copy, like any
        // other value-type site -- never moved (value types aren't smart
        // pointers, there's nothing to move).
        rhs = emitExpression(assign->expression.get());
    } else {
        // `x = a;` transfers ownership of an object local (x is a plain
        // local/parameter, NOT a field -- excluded above).
        rhs = emitMovedArg(assign->expression.get());
    }
    return rhs;
}

void AstEmitter::emitStatement(const Statement* stmt, std::ostringstream& out, int indent) {
    const std::string pad = indentStr(indent);

    if (auto decl = dynamic_cast<const VariableDeclaration*>(stmt)) {
        if (decl->isObserver) {
            // Observer: `ref T b = a;` -> `T* b = (a).get();`. A raw pointer (not a
            // native `T&`) so it can be REBOUND later (`b = c;`, see the
            // AssignmentStatement branch below) -- a `T&` cannot be rebound once
            // initialized, which is exactly the gap this lowering closes. `.get()`
            // here is compiler-generated code extracting the address from a smart
            // pointer the dangling checker has already proven safe to observe, not a
            // user-authored escape hatch (EscapeHatchChecker still rejects source-
            // level `.get()` -- see Chapter 17's note on this distinction).
            noteVar(decl->identifier, VarKind::Reference);
            observerClass_[decl->identifier] = decl->type;
            std::string rhs = isNullptrLiteral(decl->expression.get())
                ? "nullptr"
                : "(" + emitExpression(decl->expression.get()) + ").get()";
            out << pad << decl->type << "* " << decl->identifier << " = " << rhs << ";\n";
        } else if (isValueType(decl->type)) {
            // Value-type class (Iterator etc.): plain, unwrapped declaration, copied
            // (not moved) -- matches an ordinary primitive local's own emission shape
            // just below, except the declared type is a class name instead of a
            // primitive. `new ClassName(...)` on the RHS becomes a plain constructor
            // call (no make_unique/make_shared factory), since there's no smart
            // pointer to construct into.
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
            // `Point p = doc.as<Point>();` -- the call's C++ result is a bare
            // value (see isBareValueReturningSpecializationCall), not a
            // smart pointer, so `p` must be declared bare too. Checked
            // BEFORE the ordinary isClassType smart-pointer branch below,
            // which would otherwise unconditionally wrap `p` regardless of
            // what the call actually returns.
            noteVar(decl->identifier, VarKind::Value);
            varClass_[decl->identifier] = decl->type;
            out << pad << decl->type << " " << decl->identifier << " = "
                << emitExpression(decl->expression.get()) << ";\n";
        } else if (isClassType(decl->type)) {
            // A local initialized from a @ptr-shared-hinted method's return MUST
            // itself be shared_ptr -- the C++ initializer expression is already a
            // shared_ptr (see emitFunction's return-type lowering), so declaring
            // the local unique_ptr does not compile. Bypasses the ordinary
            // smartPointerType field/local lookup entirely for this one case
            // (confirmed as a real bug via a direct probe: `Node c = a.getChild();`
            // previously emitted `unique_ptr<Node> c = a->getChild();`, a hard
            // compile error, since getChild()'s own return is shared_ptr<Node>).
            const bool sharedFromCall = isSharedReturningCall(decl->expression.get());
            const std::string ptr = sharedFromCall ? "std::shared_ptr"
                                                    : smartPointerType(decl->type, decl->identifier);
            std::string rhs;
            if (auto neww = dynamic_cast<const NewExpression*>(decl->expression.get())) {
                // Construct `neww->className`, but pick the pointer kind from
                // `decl->type` -- these differ for `Animal a = new Dog();`.
                rhs = factoryFor(neww->className, decl->identifier, neww->arguments, decl->type);
            } else if (sharedFromCall) {
                // The call's result is an ALIAS (see emitFunction/ReturnStatement),
                // not something to move -- a plain copy of the shared_ptr handle.
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
            // A std::vector<T> RHS goes through emitMovedArg (its
            // containerVarType_ branch), same as every other declaration
            // shape above -- `std::vector<int> b = a;` moves `a` exactly
            // like `Node b = a;` moves `a` above (DanglingChecker's
            // VariableDeclaration branch marks a vector-typed RHS identifier
            // moved-from unconditionally, isObjectType includes
            // isVectorType). Scoped to isVectorType only, matching
            // emitMovedArg's own containerVarType_ branch and this
            // session's other vector-only changes (reserve/copyWithin) --
            // isMapType is a separate, unaudited type this fix does not
            // touch. A non-vector RHS (a primitive, a literal, a call, a
            // subscript, ...) is unaffected: emitMovedArg falls through to a
            // plain emitExpression for anything that isn't a bare identifier
            // matching containerVarType_/varClass_.
            const bool rhsIsVector = isVectorType(decl->type);
            if (rhsIsVector) {
                containerVarType_[decl->identifier] = decl->type;
            }
            std::string rhs = rhsIsVector ? emitMovedArg(decl->expression.get())
                                          : emitExpression(decl->expression.get());
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
            lhs = emitExpression(m);
            ptrVar = m->member;  // the field being assigned
        } else if (subscriptTarget) {
            lhs = emitExpression(assign->target.get());
        } else {
            lhs = isCurrentField(assign->variable) ? "this->" + assign->variable
                                                   : assign->variable;
        }
        // `n = value;` where `n` is class-typed and `value`'s type does NOT match
        // that class (e.g. `n = "text";`) means the user's own `operator=`, not
        // ordinary owner-reassignment -- `n->operator=(value);`, a mutating call on
        // the still-alive object, not a move/rebind. An ordinary same-class
        // reassignment (`Node b = a;`-shaped `n = a;`) is unaffected: its RHS IS
        // class-typed and falls through to the existing move path below.
        //
        // EXCLUDES a bare-identifier FIELD store (`child = nullptr;`, implicit
        // `this` -- isCurrentField true): that's an ordinary field link/store (same
        // category as `a.b = x;`), never an operator= mutation, even though the
        // field's declared type is class-typed and nullptr/`new Node(...)` aren't
        // "the same class" by this check's own RHS test. Without this exclusion,
        // resetting an object-typed field to nullptr or a fresh `new` was wrongly
        // routed through operator= -- found by re-running the real yaml_node.clg
        // (`this->child->operator=(nullptr);`, which doesn't compile).
        if (!memberTarget && !subscriptTarget && !isCurrentField(assign->variable)) {
            Identifier lhsId(assign->variable);
            const std::string lhsClass = exprObjectClass(&lhsId);
            const std::string rhsClass = exprObjectClass(assign->expression.get());
            // Looked up via operatorAssignParamTypes_ (every operator=
            // overload's OWN parameter type, per class), not
            // classMethodParams_["operator="] -- a class declaring both
            // operator=(string) and operator=(SameClass) (RB18's yaml-cling
            // Node) has two arity-1 overloads that collide under
            // classMethodParams_'s arity-only overloadKey, so that map alone
            // cannot tell whether EITHER overload exists once there's more
            // than one (see operatorAssignParamTypes_'s header comment).
            if (auto opIt = operatorAssignParamTypes_.find(lhsClass); opIt != operatorAssignParamTypes_.end()) {
                // Same-class RHS (`n = otherNode;`) only routes through
                // operator= when the class declares a HANDLE-REBIND overload
                // whose OWN parameter type is the class itself (yaml-cpp's
                // Node::operator=(const Node&) shape: mutate `n`'s pointee in
                // place so every alias of `n` observes the change) AND `n`
                // is itself a PARAMETER already classified read-only (mutated
                // in place across calls, never stored/returned) -- otherwise
                // it falls through to ordinary owner-reassignment below
                // (ownership move, plain shared_ptr/unique_ptr repoint).
                //
                // This second condition is load-bearing, not a nicety: an
                // ORDINARY LOCAL reassigned in a loop (`cur =
                // cur.nextSibling();`, yaml_emitter.clg's tree walk) has the
                // exact same "same-class RHS" AST shape as `parseValueInto`'s
                // `target` handle-rebind, but needs the OPPOSITE semantics --
                // a plain repoint, not an in-place payload mutation (routing
                // it through operator= caused a real, confirmed segfault:
                // `cur->operator=(cur->nextSibling())` mutates `cur`'s OWN
                // payload using a value computed from `cur` itself, and
                // breaks entirely once nextSibling() returns null at the end
                // of a chain). CeeLing has no syntax to distinguish "rebind
                // this shared alias" from "advance this loop variable" --
                // both are spelled `x = y;` identically -- so the read-only-
                // parameter classification (built for exactly this need,
                // Step 29/33) is reused as the disambiguating signal: a
                // parameter that's read-only is, by construction, meant to
                // keep referring to "the same logical Node" across the
                // method's whole body, which is exactly the handle-rebind
                // contract; an ordinary local has no such guarantee and
                // defaults to the ordinary move/repoint every other object
                // local already gets. A non-same-class RHS (e.g. a string)
                // still routes through operator= whenever ANY overload
                // exists, unaffected by this gate (matches the pre-existing
                // `operator=(string)` behavior, never ambiguous with a loop
                // variable since a string can't need handle-rebind semantics
                // at all).
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
        // A bare `return;` (void early-exit, ret->expression null -- see
        // grammar.js's `optional($.expression)` on return_statement) needs
        // none of the value-lowering logic below, which all assume a real
        // expression to inspect.
        if (!ret->expression) {
            out << pad << "return;\n";
            return;
        }
        // Returning an object hands ownership back to the caller. A `new T(...)`
        // becomes a factory; an owning local is std::move'd out (a smart pointer is
        // move-only, so this is required, and the dangling checker has proven the
        // local is not used after the return). Non-object returns (`return 0;`,
        // `return a.value;`) fall through unchanged.
        std::string rhs;
        auto retIsThis = dynamic_cast<const ThisExpression*>(ret->expression.get()) != nullptr;
        if (currentMethodReturnsValueType_) {
            // Value-type return: plain, unwrapped, copied (never moved) -- matches
            // the value-type VariableDeclaration branch's own RHS handling above.
            if (auto neww = dynamic_cast<const NewExpression*>(ret->expression.get())) {
                rhs = neww->className + "(";
                for (size_t i = 0; i < neww->arguments.size(); ++i) {
                    if (i > 0) rhs += ", ";
                    rhs += emitExpression(neww->arguments[i].get());
                }
                rhs += ")";
            } else if (retIsThis) {
                // `this` is ALWAYS a raw pointer in C++ (Type*), regardless of
                // value-type-ness -- `return this;` with a plain (non-pointer)
                // return type does not compile (confirmed via direct probe:
                // "invalid conversion from 'Foo*' to 'int'" against Foo's own
                // constructor, since the compiler tries to construct a NEW Foo
                // from the pointer). Dereference to get the actual object, which
                // is then copy-constructed into the return value, same as any
                // other value-type return.
                rhs = "*this";
            } else {
                rhs = emitExpression(ret->expression.get());
            }
        } else if (currentMethodReturnsShared_ && retIsThis) {
            // `return this;` from a @ptr-shared-hinted method: shared_from_this()
            // aliases the object's OWN existing shared_ptr control block (set up by
            // enable_shared_from_this, wired in by emitClass/classNeedsSharedFromThis
            // whenever this pattern is detected) -- NOT `shared_ptr<T>(this)`, which
            // would create a second, independent control block and double-free.
            rhs = "shared_from_this()";
        } else if (currentMethodReturnsShared_) {
            // A @ptr-shared-hinted method's return is an ALIAS, never a transfer:
            // `return child;`/`return this->child;` must copy the shared_ptr handle
            // (refcount bump), leaving the field itself live and usable afterward --
            // the same "copy, don't move" choice a shared field-store assignment
            // (`a.b = obj;`) already makes, applied here to `return` instead.
            rhs = emitExpression(ret->expression.get());
        } else if (auto neww = dynamic_cast<const NewExpression*>(ret->expression.get())) {
            // Force make_unique to match the lowered return type (unique_ptr<T>),
            // rather than asking the analyzer for a named-variable decision.
            rhs = "std::make_unique<" + neww->className + ">(";
            for (size_t i = 0; i < neww->arguments.size(); ++i) {
                if (i > 0) rhs += ", ";
                rhs += emitExpression(neww->arguments[i].get());
            }
            rhs += ")";
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
            // `else if` is a single nested IfStatement; emit it inline after `else `.
            if (ifs->elseBody.size() == 1 &&
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
        // `std::vector<T> v;` -- no initializer needed; default-constructs empty.
        // The container itself is a plain value (never smart-pointer-wrapped, see
        // Chapter 18) -- VarKind::Value, same as any other plain-value local.
        noteVar(cdecl->identifier, VarKind::Value);
        containerVarType_[cdecl->identifier] = cdecl->type;
        out << pad << cppTypeFor(cdecl->type, cdecl->identifier) << " " << cdecl->identifier << ";\n";
    } else if (auto fors = dynamic_cast<const ForStatement*>(stmt)) {
        // The CONTAINER expression itself needs a `*` dereference when it's a
        // smart-pointer-wrapped CLASS instance with its own MEMBER begin()/end()
        // (e.g. `for (Node x : range)` where `range` is shared_ptr<Range> and
        // Range::begin()/end() are member methods) -- a smart pointer has no
        // begin()/end() of its own, so real C++ range-for on the bare pointer
        // fails to compile (confirmed via direct probe: "no matching function for
        // call to 'begin(std::unique_ptr<Range>&)'"). std::vector<T>/
        // std::map<K,V> containers are NEVER pointer-wrapped (ordinary
        // value-typed locals), so this only ever fires for a CeeLing-class
        // container -- and never for a value-type class (Iterator etc.), which
        // is already unwrapped and needs no dereference either.
        //
        // Do NOT dereference when the container class has NO member begin() --
        // this means iteration relies on ADL-found FREE functions instead (the
        // standard C++ idiom for adding iteration to a type without adding
        // member methods to it, used here specifically to break a circular
        // class-ordering dependency CeeLing's lack of forward-declaration syntax
        // can't otherwise resolve -- e.g. Node/Iterator, where Node can't
        // reference Iterator as a member return type before Iterator exists, and
        // Iterator can't reference Node before Node exists). CeeLing always
        // generates a class-typed free-function PARAMETER as smart-pointer-
        // wrapped, matching the UNDEREFERENCED container variable's own type --
        // dereferencing here would break ADL lookup (confirmed via direct probe:
        // dereferencing produced `begin(Node&)`, found no candidates, since the
        // only declared `begin` took `shared_ptr<Node>`).
        const std::string containerClass = exprObjectClass(fors->container.get());
        const bool hasMemberBegin = [&] {
            auto it = classMethodParams_.find(containerClass);
            return it != classMethodParams_.end() && it->second.count("begin") > 0;
        }();
        const bool derefContainer = !containerClass.empty() && !isValueType(containerClass)
            && hasMemberBegin;

        // Range-for matches native C++ syntax 1:1 -- no translation needed beyond
        // the class-typed-element type mapping (Node -> shared_ptr<Node> or
        // unique_ptr<Node>, so the loop variable's type actually matches what
        // the container yields). A value-type element (`struct`/Iterator-
        // shaped) is excluded from this smart-pointer mapping -- it's ALREADY
        // stored bare in the container (see cppTypeFor's matching guard), so
        // the loop variable stays the bare type too, falling through to the
        // `else` branch below (VarKind::Value, `.` access) exactly like an
        // ordinary value-type local elsewhere in this file.
        std::string elemCppType = fors->loopVarType;
        bool elementIsUnique = false;
        if (isClassType(fors->loopVarType) && !isValueType(fors->loopVarType)) {
            std::string containerName;
            if (auto id = dynamic_cast<const Identifier*>(fors->container.get())) {
                containerName = id->name;
            }
            // Two genuinely different sources for the element's pointer kind,
            // depending on iteration style: a std::vector<T>/std::map<K,V>
            // container's element kind comes from SmartPointerAnalyzer::
            // containerElementIsShared (decided across its own push_back call
            // sites). An ADL-found free `begin()`'s element kind (only relevant
            // when there's no MEMBER begin, i.e. the same condition
            // derefContainer/hasMemberBegin already gates) instead comes from
            // `*begin()`'s OWN declared return pointer kind (Iterator::
            // operator*() -- confirmed via direct probe that these are
            // independently decided and can disagree: containerElementIsShared
            // has no signal at all for a non-vector container, silently
            // defaulting to unique, while operator*() may have been inferred
            // shared, producing a loop variable type that doesn't match what
            // operator*() actually returns). Assumes at most one free `begin`
            // in the program (narrowly scoped to yaml-cling's single-iterable-
            // type use case, not a general multi-iterable-type lookup).
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
        // unique_ptr elements can't be copied, so the loop variable must bind by
        // reference (`T&`); shared_ptr elements copy cheaply (refcount bump) and
        // stay by-value, matching how CeeLing's model treats a shared_ptr local
        // elsewhere.
        out << pad << "for (" << elemCppType << (elementIsUnique ? "& " : " ")
            << fors->loopVarName << " : " << (derefContainer ? "*" : "")
            << emitExpression(fors->container.get()) << ") {\n";
        if (isClassType(fors->loopVarType) && !isValueType(fors->loopVarType)) {
            // The loop variable is a shared_ptr<T> per iteration -- register it like
            // any other smart-pointer local so member access inside the body lowers
            // `.` to `->` via the existing exprObjectClass machinery.
            noteVar(fors->loopVarName, VarKind::SmartPtr);
            varClass_[fors->loopVarName] = fors->loopVarType;
        } else {
            // Also covers a value-type element (`struct`/Iterator-shaped): no
            // smart-pointer wrapping, `.` access via VarKind::Value, matching
            // an ordinary value-type local elsewhere in this file.
            noteVar(fors->loopVarName, VarKind::Value);
        }
        for (const auto& inner : fors->body) {
            emitStatement(inner.get(), out, indent + 1);
        }
        out << pad << "}\n";
    } else if (auto exprStmt = dynamic_cast<const ExpressionStatement*>(stmt)) {
        out << pad << emitExpression(exprStmt->expression.get()) << ";\n";
    } else if (auto throwStmt = dynamic_cast<const ThrowStatement*>(stmt)) {
        // `throw <expr>;` -- restricted to string operands (DanglingChecker has
        // already proven this). A string LITERAL is C++ `const char*`, not
        // `std::string` -- `catch (const std::string&)` does NOT match a thrown
        // `const char*` (C++ exception matching is exact-type, no implicit
        // conversion), which would silently std::terminate at runtime instead of
        // being caught. Force-wrap literals in std::string(...) so the thrown type
        // always matches the catch clause's type; an already-`std::string`-typed
        // identifier needs no wrapping.
        const bool isLiteral = dynamic_cast<const StringLiteral*>(throwStmt->expression.get()) != nullptr;
        std::string operand = emitExpression(throwStmt->expression.get());
        if (isLiteral) operand = "std::string(" + operand + ")";
        out << pad << "throw " << operand << ";\n";
    } else if (auto tryStmt = dynamic_cast<const TryStatement*>(stmt)) {
        // `catch (string name)` lowers to idiomatic C++ catch-by-const-ref. This is a
        // fixed emitter convention, not derived from cppTypeFor -- `string` is the
        // only catch type this slice supports (DanglingChecker enforces that), so
        // there's no other case to generalize to yet.
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

std::string AstEmitter::emitExpression(const Expression* expr) {
    if (expr == nullptr) return "";

    if (auto id = dynamic_cast<const Identifier*>(expr)) {
        // A bare reference to a field of the current class -> this->field.
        if (isCurrentField(id->name)) {
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
        // Stream `<<` chains are left-associative and need no grouping parens — emit them
        // flat (`std::cout << a << b`). Other operators keep parens to preserve grouping.
        //
        // Gated on isStreamChain, NOT on the operator text alone: `<<` is also
        // integer shift now, and dropping parens from a shift silently changes
        // what the arithmetic means. `(1 << 4) + 1` emitted `(1 << 4 + 1)` under
        // the old text-only check, which C++ regroups as `1 << (4+1)` -- the
        // program printed 32 where the source says 17, with no diagnostic. A
        // shift falls through to the generic parenthesized form below instead.
        if (bin->operator_str == "<<" && isStreamChain(bin)) {
            return emitExpression(bin->left.get()) + " << " + emitExpression(bin->right.get());
        }
        // `a == b`/`a != b` where BOTH operands are the SAME smart-pointer-
        // backed class AND that class declares its own operator==/!=: route
        // through the user's overload explicitly (`a->operator==(*b)`), since
        // `a`/`b` are shared_ptr<T> VARIABLES at the generated-C++ level, not
        // T objects directly -- plain `a == b` would compare shared_ptr
        // IDENTITY (which pointer object), never reaching T::operator== at
        // all. This is different from the pre-existing "== already works
        // with zero changes" case (RB16): that relied on shared_ptr's OWN
        // built-in identity comparison matching yaml-cpp's is() semantics
        // exactly, which was true ONLY because Node WAS its own payload one
        // handle == one node. Once a class introduces its own handle/payload
        // split (RB18's Node/NodeData: two DIFFERENT Node handles can wrap
        // the SAME payload), shared_ptr identity stops matching "same node"
        // and the class needs its own operator== reaching through to the
        // payload -- confirmed as a real regression via yaml-cling's own
        // `root["name"] == root["name"]` (two separate operator[] calls,
        // same underlying entry) wrongly comparing false after RB18 without
        // this routing. A value-type class (Iterator) is UNAFFECTED --
        // its operands are never smart-pointer-wrapped, so plain `a == b`
        // already resolves to its own operator== via ordinary C++ overload
        // resolution, matching operator!='s existing (also un-routed)
        // behavior for value types.
        if (bin->operator_str == "==" || bin->operator_str == "!=") {
            const std::string leftClass = exprObjectClass(bin->left.get());
            const std::string rightClass = exprObjectClass(bin->right.get());
            // Excludes comparing a bare FIELD of the CURRENT class (e.g.
            // Iterator's own `current != other.current`, where `current`
            // is `this->current`, a Node-typed field of Iterator itself):
            // that comparison is checking HANDLE identity for iteration
            // termination (needs to work when `current` is a genuinely null
            // shared_ptr, i.e. the end() sentinel), not payload identity --
            // routing it through the user's operator==/!= would call the
            // method ON a possibly-null receiver (`this->current->operator!=
            // (...)` when `current` is null), which is undefined behavior
            // (confirmed as a real segfault via the range-for termination
            // check crashing at the last element). A genuine EXTERNAL
            // comparison (`root == other` in `main()`) is unaffected: `root`
            // is not a field of any enclosing class there.
            const auto leftId = dynamic_cast<const Identifier*>(bin->left.get());
            const bool leftIsOwnField = leftId != nullptr && isCurrentField(leftId->name);
            if (!leftClass.empty() && leftClass == rightClass && !leftIsOwnField &&
                !analyzer_.isValueTypeClass(leftClass)) {
                // Method names are stored WITH the "operator" prefix
                // (classMethodParams_ keys by method->name verbatim, e.g.
                // "operator==", never the bare symbol) -- confirmed via
                // direct probe after this check first looked up the bare
                // `bin->operator_str` ("==") and silently never matched.
                const std::string methodName = "operator" + bin->operator_str;
                auto clsIt = classMethodParams_.find(leftClass);
                if (clsIt != classMethodParams_.end() && clsIt->second.count(methodName) > 0) {
                    // The method parameter is `Node` (by value, like every
                    // other CeeLing object parameter -- lowered to
                    // shared_ptr<Node>), so the RHS is passed AS-IS (the
                    // shared_ptr itself), not dereferenced -- `*b` would
                    // yield `Node&`, not `shared_ptr<Node>`, a type mismatch.
                    return emitExpression(bin->left.get()) + "->" + methodName + "("
                           + emitExpression(bin->right.get()) + ")";
                }
            }
        }
        return "(" + emitExpression(bin->left.get()) + " " + bin->operator_str + " "
               + emitExpression(bin->right.get()) + ")";
    }
    if (auto un = dynamic_cast<const UnaryExpression*>(expr)) {
        // Prefix operator: `!x`, `-x`, `*x`, `++x`. No rewriting needed for the
        // latter two even on a value-type class operand (Iterator etc.): unlike
        // operator[]/operator=, a value-type instance is never smart-pointer-
        // wrapped, so literal `*it`/`++it` in the generated C++ already resolves
        // to the user's own operator*/operator++ via ordinary C++ overload
        // resolution -- nothing here needs to know or care that `it` has a
        // user-defined operator at all.
        return un->operator_str + emitExpression(un->operand.get());
    }
    if (auto neww = dynamic_cast<const NewExpression*>(expr)) {
        // `new T(...)` outside a typed declaration. A value-type class (Iterator
        // etc.) becomes a plain constructor call, matching every other value-type
        // construction site; everything else defaults to make_shared as before.
        if (isValueType(neww->className)) {
            std::string s = neww->className + "(";
            for (size_t i = 0; i < neww->arguments.size(); ++i) {
                if (i > 0) s += ", ";
                s += emitExpression(neww->arguments[i].get());
            }
            s += ")";
            return s;
        }
        std::string s = "std::make_shared<" + neww->className + ">(";
        for (size_t i = 0; i < neww->arguments.size(); ++i) {
            if (i > 0) s += ", ";
            s += emitExpression(neww->arguments[i].get());
        }
        s += ")";
        return s;
    }
    if (auto call = dynamic_cast<const FunctionCall*>(expr)) {
        std::string prefix;
        std::string receiverContainerType;  // "" unless recv is a known container local
        std::string receiverName;
        std::string receiverClass;
        std::string methodName;
        std::string recvText;  // receiver's own emitted text, kept outside the
                                // MemberAccess branch's scope -- copyWithin's
                                // codegen below needs it standalone (not
                                // fused into `prefix`, which is
                                // "recvText.copyWithin" by the time this runs).
        if (auto m = dynamic_cast<const MemberAccess*>(call->callee.get())) {
            // Method call `recv.method(args)` -> `recv (-> or .) method(args)`, or
            // `recv (-> or .) method<T>(args)` when the call carries a type argument
            // (`n.as<int>()`) -- real C++ template resolution picks the specialization
            // at compile time; CeeLing's own specializationKey lookups (calleeParam,
            // isKnownCallable, resolveCallReturnAlias) are a separate, analysis-only
            // mechanism and must agree in spirit but not literally emit anything here.
            const std::string recv = emitExpression(m->object.get());
            recvText = recv;
            const std::string mClass = exprObjectClass(m->object.get());
            const std::string op = (mClass.empty() || isValueType(mClass)) ? "." : "->";
            prefix = recv + op + m->member;
            if (!call->templateArg.empty()) {
                prefix += "<" + cppTypeFor(call->templateArg, "") + ">";
            }
            receiverContainerType = containerVarType(m->object.get());
            if (auto id = dynamic_cast<const Identifier*>(m->object.get())) {
                receiverName = id->name;
            }
            receiverClass = mClass;
            methodName = m->member;
        } else {
            // A bare call whose name is BOTH a known free function AND a
            // method of the CURRENT class needs `::` qualification in the
            // EMITTED C++, or real C++'s member-name-hides-outer-name rule
            // silently resolves it to the (arity-mismatched) member instead
            // of the free function CeeLing's own resolution (isKnownCallable
            // checks free functions before same-class methods) already
            // committed to -- confirmed via a minimal, CeeLing-independent
            // probe (`struct S { int foo(int); }; ... s.foo() calls
            // ::foo(1,2,3) from inside S::foo`, a hard compile error: "no
            // matching function", NOT a fallback to the free function).
            // Found via imgui-cling's Context gaining a `button()` method
            // that forwards to the pre-existing free function of the exact
            // same name (`return button(this, ...);`) -- the first consumer
            // to intentionally reuse a free function's name as a method
            // name. Only qualifies on an ACTUAL collision (currentClass_
            // has its own same-named method) -- unconditionally prefixing
            // every bare free-function call would wrongly force GLOBAL
            // namespace lookup for one truly different case: a namespaced
            // free function called bare from WITHIN its own namespace.
            bool collidesWithOwnMethod = !currentClass_.empty() &&
                call->function_name.find("::") == std::string::npos &&
                functionParams_.count(call->function_name) > 0 &&
                classMethodParams_.count(currentClass_) > 0 &&
                classMethodParams_.at(currentClass_).count(call->function_name) > 0;
            prefix = (collidesWithOwnMethod ? "::" : "") + call->function_name;
            if (!call->templateArg.empty()) {
                prefix += "<" + cppTypeFor(call->templateArg, "") + ">";
            }
            // Implicit-`this` call (bare `decodeInto(out)` inside a method
            // body, no explicit receiver) -- the callee is still a method of
            // the CURRENT class, so outParamIndex_ must be looked up under
            // currentClass_ too, exactly like the explicit-receiver branch
            // above does under mClass. Without this, an out-parameter call
            // made via implicit `this` (as opposed to `this.decodeInto(out)`
            // or `n.decodeInto(out)`) skipped the .get()/no-move handling
            // entirely -- confirmed via probe (yaml-cling's own as<Point>()
            // specialization calling decodeInto(out) with implicit this).
            // NOT reassigned when collidesWithOwnMethod -- the call resolves
            // to the FREE function in that case, not currentClass_'s own
            // same-named method, so there is no out-parameter to look up
            // under currentClass_'s method map for it.
            if (!collidesWithOwnMethod) {
                receiverClass = currentClass_;
                methodName = call->function_name;
            }
        }
        const bool receiverIsVector = isVectorType(receiverContainerType);
        // v.copyWithin(dstStart, srcStart, len): std::vector has no member
        // of this shape, so (unlike every other vector method on this page,
        // which is a plain pass-through) this lowers to the cling::copyWithin
        // runtime helper (copyWithinHelperDefinition, library_summary.h:
        // memmove semantics, inline 16-byte chunks instead of a library
        // call). Exactly Rust slice::copy_within's semantics, not a
        // self-repeating/run-length primitive -- see isKnownVectorMethod's
        // own doc comment in library_summary.h for why a self-overlapping
        // DEFLATE-style back-reference (match distance shorter than match
        // length) is OUT OF SCOPE for this call and must be handled by the
        // caller before ever calling copyWithin. Guarded on
        // isPrimitiveType(element) the same way resize/reserve are (see
        // EscapeHatchChecker::isKnownCallable) -- checked again here, not
        // just trusted from the escape-hatch pass, since the helper computes
        // a byte count from the element type.
        if (receiverIsVector && methodName == "copyWithin" && call->arguments.size() == 3 &&
            isPrimitiveType(vectorElementType(receiverContainerType))) {
            const std::string dst = emitExpression(call->arguments[0].get());
            const std::string src = emitExpression(call->arguments[1].get());
            const std::string len = emitExpression(call->arguments[2].get());
            return "cling::copyWithin(" + recvText + ", " + dst + ", " + src + ", " + len + ")";
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
            if (i > 0) s += ", ";
            if (i == argOutParamIdx) {
                // Mirrors the VariableDeclaration `ref` observer lowering
                // (`(a).get()`) -- see buildFunctionSignature's matching
                // outParamIdx branch. The argument is NOT moved: the
                // dangling checker (DanglingChecker::applyCallArgMoves) has
                // already been taught this same index is a mutable
                // out-parameter, so the caller's own local/param stays alive
                // for use after this call.
                // A plain-value argument (e.g. `Point out` inside an
                // as<Point>() wrapper forced bare by
                // currentSpecializationValueType_ -- see its own comment)
                // has no `.get()` to call; take its address directly. An
                // ordinary smart-pointer argument still uses `.get()`
                // exactly as before.
                if (auto argId = dynamic_cast<const Identifier*>(call->arguments[i].get());
                    argId != nullptr && kindOf(argId->name) == VarKind::Value) {
                    s += "&" + argId->name;
                } else {
                    s += "(" + emitExpression(call->arguments[i].get()) + ").get()";
                }
                continue;
            }
            if (receiverIsVector && vectorMethodSharesArg(methodName, i)) {
                // A value-type element (`struct`/Iterator-shaped) is stored
                // BARE in the container (see cppTypeFor's matching guard) --
                // no smart-pointer construction/move/share logic applies at
                // all, matching how a bare value-type local is already
                // constructed/copied elsewhere in this file. Checked FIRST,
                // before any of the make_shared/make_unique/move/share
                // branches below, all of which assume a class-typed element
                // needs SOME pointer kind.
                const std::string vectorElemType = vectorElementType(receiverContainerType);
                if (!vectorElemType.empty() && analyzer_.isValueTypeClass(vectorElemType)) {
                    if (auto neww = dynamic_cast<const NewExpression*>(call->arguments[i].get())) {
                        // `push_back(new Vertex(1.0, 2.0))` -> plain constructor
                        // call, mirroring how a bare `new T(...)` outside a
                        // container already becomes a plain constructor call
                        // for a value-type class (see the NewExpression
                        // handling elsewhere in this file).
                        std::string arg = neww->className + "(";
                        for (size_t j = 0; j < neww->arguments.size(); ++j) {
                            if (j > 0) arg += ", ";
                            arg += emitExpression(neww->arguments[j].get());
                        }
                        arg += ")";
                        s += arg;
                    } else {
                        // An existing-variable argument: plain copy, never
                        // moved -- a small POD struct copies cheaply, and it
                        // has no ownership to transfer in the first place.
                        s += emitExpression(call->arguments[i].get());
                    }
                    continue;
                }
                // A container's element pointer kind is decided once across every
                // push_back call site targeting it (see DanglingChecker::
                // applyCallArgMoves, which this must produce matching results
                // for). This specific push shares (copies, argument survives)
                // only when the element kind is shared AND the argument is
                // already a shared_ptr; otherwise it's a move (consuming the
                // argument), wrapped in a shared_ptr construction if the element
                // kind is shared but the argument itself is a unique_ptr.
                if (auto neww = dynamic_cast<const NewExpression*>(call->arguments[i].get())) {
                    // A bare `new T(...)` argument has no variable of its own --
                    // use the container's decided element pointer kind directly.
                    const std::string factory = elementIsShared ? "std::make_shared<" : "std::make_unique<";
                    std::string arg = factory + neww->className + ">(";
                    for (size_t j = 0; j < neww->arguments.size(); ++j) {
                        if (j > 0) arg += ", ";
                        arg += emitExpression(neww->arguments[j].get());
                    }
                    arg += ")";
                    s += arg;
                    continue;
                }
                auto argId = dynamic_cast<const Identifier*>(call->arguments[i].get());
                const std::string argClass = argId ? exprObjectClass(argId) : std::string{};
                const bool argIsShared = !argClass.empty() &&
                    analyzer_.getPointerType(argClass, argId->name) == PointerType::SHARED;
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
                // See the function's own doc comment: a NON-local argument
                // (field access) bound to a read-only container parameter of
                // a callee that is not proven heap-pure gets an explicit
                // copy instead of the plain identifier/field-access
                // reference emitMovedArg would otherwise emit -- preserving
                // by-value semantics exactly (the callee cannot observe
                // anything the (possibly impure) call does to the field
                // afterward, matching what by-value copying already
                // guaranteed before this parameter was ever classified
                // read-only).
                s += cppTypeFor(argContainerType, "") + "(" + emitExpression(call->arguments[i].get()) + ")";
                continue;
            }
            s += emitMovedArg(call->arguments[i].get(), calleeParam(*call, i));
        }
        s += ")";
        return s;
    }
    if (auto member = dynamic_cast<const MemberAccess*>(expr)) {
        // `->` if the object is a smart-pointer-to-object (including nested access and
        // this-> fields); `.` for references, value types, and plain values.
        const std::string objStr = emitExpression(member->object.get());
        const std::string memberClass = exprObjectClass(member->object.get());
        const std::string op = (memberClass.empty() || isValueType(memberClass)) ? "." : "->";
        return objStr + op + member->member;
    }
    if (auto sub = dynamic_cast<const SubscriptExpression*>(expr)) {
        // A class-typed receiver's `n[key]` means the user's own `operator[]`
        // method, not container indexing -- `n` is a smart pointer, and
        // `unique_ptr<T>`/`shared_ptr<T>` have no operator[] of their own (or, for
        // shared_ptr, a different one entirely), so this MUST route through an
        // ordinary method call (`n->operator[](key)`), never literal `n[key]`.
        const std::string objClass = exprObjectClass(sub->object.get());
        if (!objClass.empty()) {
            return emitExpression(sub->object.get()) + "->operator[](" +
                   emitExpression(sub->index.get()) + ")";
        }
        // `v[i]` lowers 1:1 to C++ `v[i]` -- std::vector's operator[] is exactly
        // what CeeLing's subscript syntax means, no translation needed.
        return emitExpression(sub->object.get()) + "[" + emitExpression(sub->index.get()) + "]";
    }
    // Unknown node: best effort.
    return expr->toString();
}

} // namespace cling
