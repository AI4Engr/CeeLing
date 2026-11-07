// AstEmitter: classes, headers, constructors and function bodies
#include "emit/ast_emitter.h"
#include <functional>
#include "syntax/class_hierarchy.h"
#include "facts/library_summary.h"
#include "emit/runtime_helpers.h"
#include "passes/channel_check.h"

#include "emit/ast_emitter_util.h"

namespace cling {

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

} // namespace cling
