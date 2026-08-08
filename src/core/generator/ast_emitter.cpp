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
    collectClassNames(program);

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
    for (const auto& stmt : program.statements) {
        if (auto inc = dynamic_cast<const IncludeStatement*>(stmt.get())) {
            if (emittedIncludes.insert(inc->includeFile).second) {
                out << inc->includeFile << "\n";
            }
        }
    }
    out << "\n";

    for (const auto& stmt : program.statements) {
        emitTopLevel(stmt.get(), out);
    }
    return out.str();
}

void AstEmitter::collectClassNames(const Program& program) {
    for (const auto& stmt : program.statements) {
        if (auto fn = dynamic_cast<const FunctionDeclaration*>(stmt.get())) {
            functionParams_[fn->name] = fn->parameters;
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
        auto& fields = classFields_[cls->name];
        auto& methodParams = classMethodParams_[cls->name];
        if (!cls->baseClass.empty()) {
            baseClasses_.insert(cls->baseClass);
            if (auto baseIt = classFields_.find(cls->baseClass); baseIt != classFields_.end()) {
                fields = baseIt->second;
            }
            if (auto baseIt = classMethodParams_.find(cls->baseClass);
                baseIt != classMethodParams_.end()) {
                methodParams = baseIt->second;
            }
        }
        for (const auto& member : cls->members) {
            if (auto var = dynamic_cast<const VariableDeclaration*>(member.get())) {
                fields[var->identifier] = var->type;
            } else if (auto method = dynamic_cast<const MethodDeclaration*>(member.get())) {
                methodParams[method->name] = method->parameters;
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
        auto paramIt = clsIt->second.find(member->member);
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

std::string AstEmitter::exprObjectClass(const Expression* expr) const {
    if (auto id = dynamic_cast<const Identifier*>(expr)) {
        // A field of the current class (accessed via this->) that is object-typed.
        if (isCurrentField(id->name)) {
            const std::string ft = fieldType(currentClass_, id->name);
            return isClassType(ft) ? ft : "";
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
    return "";
}

bool AstEmitter::isClassType(const std::string& type) const {
    return classNames_.find(type) != classNames_.end();
}

std::string AstEmitter::containerVarType(const Expression* expr) const {
    auto id = dynamic_cast<const Identifier*>(expr);
    if (id == nullptr) return "";
    auto it = containerVarType_.find(id->name);
    return it == containerVarType_.end() ? "" : it->second;
}

std::string AstEmitter::cppTypeFor(const std::string& ceelingType, const std::string& varName) const {
    if (isStringType(ceelingType)) return "std::string";
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
    std::string elemCpp = elem;
    if (isClassType(elem)) {
        const std::string ptr = analyzer_.containerElementIsShared(varName)
            ? "std::shared_ptr<" : "std::unique_ptr<";
        elemCpp = ptr + elem + ">";
    }
    return "std::vector<" + elemCpp + ">";
}

AstEmitter::VarKind AstEmitter::kindOf(const std::string& name) const {
    auto it = varKinds_.find(name);
    return it == varKinds_.end() ? VarKind::Value : it->second;
}

std::string AstEmitter::smartPointerType(const std::string& className,
                                         const std::string& varName) const {
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
    if (auto cls = dynamic_cast<const ClassDeclaration*>(stmt)) {
        emitClass(*cls, out);
    } else if (auto fn = dynamic_cast<const FunctionDeclaration*>(stmt)) {
        emitFunction(fn->returnType, fn->name, fn->parameters, fn->body, out);
    } else if (auto mn = dynamic_cast<const MainFunction*>(stmt)) {
        emitFunction(mn->returnType, "main", {}, mn->body, out);
    } else if (dynamic_cast<const IncludeStatement*>(stmt)) {
        // All includes (required std headers + any the source declared) are collected
        // and emitted once, de-duplicated, at the top of emit(). Nothing to do here.
    } else if (dynamic_cast<const CommentStatement*>(stmt)) {
        // Comments are dropped in the AST-driven output for now.
    }
}

void AstEmitter::emitClass(const ClassDeclaration& cls, std::ostringstream& out) {
    out << "class " << cls.name;
    if (!cls.baseClass.empty()) {
        out << " : public " << cls.baseClass;
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
    if (baseClasses_.count(cls.name) > 0) {
        out << indentStr(1) << "virtual ~" << cls.name << "() = default;\n";
        out << "protected:\n";
        out << indentStr(1) << cls.name << "() = default;\n";
        out << "public:\n";
    }

    for (const auto& member : cls.members) {
        if (auto var = dynamic_cast<const VariableDeclaration*>(member.get())) {
            // Member field. Object-typed fields become smart pointers.
            if (isClassType(var->type)) {
                out << indentStr(1) << smartPointerType(var->type, var->identifier)
                    << "<" << var->type << "> " << var->identifier << ";\n";
            } else {
                out << indentStr(1) << cppTypeFor(var->type, var->identifier) << " "
                    << var->identifier << ";\n";
            }
        } else if (auto ctor = dynamic_cast<const ConstructorDeclaration*>(member.get())) {
            emitFunction("", ctor->name, ctor->parameters, ctor->body, out, cls.name, 1);
        } else if (auto method = dynamic_cast<const MethodDeclaration*>(member.get())) {
            emitFunction(method->returnType, method->name, method->parameters, method->body,
                         out, cls.name, 1, method->isConst, method->isVirtual, method->isOverride);
        }
    }
    out << "};\n\n";
}

void AstEmitter::emitFunction(const std::string& returnType, const std::string& name,
                              const std::vector<std::pair<std::string, std::string>>& params,
                              const std::vector<StatementPtr>& body, std::ostringstream& out,
                              const std::string& ownerClass, int baseIndent, bool isConst,
                              bool isVirtual, bool isOverride) {
    // Each function has its own local variable scope; `currentClass_` enables `this->`
    // for field access inside a method/constructor.
    varKinds_.clear();
    varClass_.clear();
    observerClass_.clear();
    containerVarType_.clear();
    const std::string savedClass = currentClass_;
    currentClass_ = ownerClass;

    // Signature. An object return type is lowered to a smart pointer so the body's
    // owning local (a smart pointer) can be returned directly (unique_ptr by default:
    // the callee constructs the object and hands sole ownership back to the caller).
    std::string sig;
    if (isVirtual) sig += "virtual ";
    if (!returnType.empty()) {
        sig += isClassType(returnType) ? "std::unique_ptr<" + returnType + "> "
                                       : cppTypeFor(returnType, "") + " ";
    }
    sig += name + "(";
    for (size_t i = 0; i < params.size(); ++i) {
        if (i > 0) sig += ", ";
        const auto& [ptype, pname] = params[i];
        if (isClassType(ptype)) {
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
    // Class members are indented one level; free functions sit at column 0.
    const std::string pad = indentStr(baseIndent);
    out << pad << sig << " {\n";
    for (const auto& stmt : body) {
        emitStatement(stmt.get(), out, baseIndent + 1);
    }
    out << pad << "}\n";
    if (baseIndent == 0) out << "\n";  // blank line only between top-level definitions

    currentClass_ = savedClass;
}

std::string AstEmitter::emitMovedArg(const Expression* arg,
                                     const std::pair<std::string, std::string>& targetParam) {
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
    }
    if (!targetParam.first.empty()) {
        if (auto neww = dynamic_cast<const NewExpression*>(arg)) {
            // A bare `new T(...)` argument has no variable of its own to key its
            // pointer type off (unlike `Node x = new Node(1);`, which uses x's
            // analyzer decision) -- use the receiving parameter's own (class, name)
            // instead of emitExpression's context-free make_shared default, so the
            // constructed object matches what the callee's signature expects.
            return factoryFor(targetParam.first, targetParam.second, neww->arguments);
        }
    }
    return emitExpression(arg);
}

// ---- statements ----------------------------------------------------------

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
        } else if (isClassType(decl->type)) {
            const std::string ptr = smartPointerType(decl->type, decl->identifier);
            std::string rhs;
            if (auto neww = dynamic_cast<const NewExpression*>(decl->expression.get())) {
                // Construct `neww->className`, but pick the pointer kind from
                // `decl->type` -- these differ for `Animal a = new Dog();`.
                rhs = factoryFor(neww->className, decl->identifier, neww->arguments, decl->type);
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
            if (isVectorType(decl->type)) {
                containerVarType_[decl->identifier] = decl->type;
            }
            out << pad << cppTypeFor(decl->type, decl->identifier) << " " << decl->identifier << " = "
                << emitExpression(decl->expression.get()) << ";\n";
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
            // `target = new T(...)` -> make_*<T>(...) with the target field's ptr kind.
            rhs = factoryFor(neww->className, ptrVar, neww->arguments);
        } else if (rhsIsNullptr && !ptrVar.empty() &&
                   analyzer_.getPointerTypeForVariable(ptrVar) == PointerType::WEAK) {
            // weak_ptr has no operator=(nullptr_t); default-construct instead (an empty
            // weak_ptr, the same meaning as "nullptr" for the other smart-pointer kinds).
            rhs = "{}";
        } else if (memberTarget) {
            // `a.b = obj;` links/shares into a field — it does not consume `obj`.
            rhs = emitExpression(assign->expression.get());
        } else {
            // `x = a;` transfers ownership of an object local.
            rhs = emitMovedArg(assign->expression.get());
        }
        out << pad << lhs << " = " << rhs << ";\n";
    } else if (auto ret = dynamic_cast<const ReturnStatement*>(stmt)) {
        // Returning an object hands ownership back to the caller. A `new T(...)`
        // becomes a factory; an owning local is std::move'd out (a smart pointer is
        // move-only, so this is required, and the dangling checker has proven the
        // local is not used after the return). Non-object returns (`return 0;`,
        // `return a.value;`) fall through unchanged.
        std::string rhs;
        if (auto neww = dynamic_cast<const NewExpression*>(ret->expression.get())) {
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
        // Range-for matches native C++ syntax 1:1 -- no translation needed beyond
        // the class-typed-element type mapping (Node -> shared_ptr<Node> or
        // unique_ptr<Node>, matching the CONTAINER's own decided element kind --
        // see cppTypeFor/SmartPointerAnalyzer::containerElementIsShared -- so the
        // loop variable's type actually matches what the container yields).
        std::string elemCppType = fors->loopVarType;
        bool elementIsUnique = false;
        if (isClassType(fors->loopVarType)) {
            std::string containerName;
            if (auto id = dynamic_cast<const Identifier*>(fors->container.get())) {
                containerName = id->name;
            }
            elementIsUnique = !analyzer_.containerElementIsShared(containerName);
            elemCppType = (elementIsUnique ? "std::unique_ptr<" : "std::shared_ptr<") +
                          fors->loopVarType + ">";
        }
        // unique_ptr elements can't be copied, so the loop variable must bind by
        // reference (`T&`); shared_ptr elements copy cheaply (refcount bump) and
        // stay by-value, matching how CeeLing's model treats a shared_ptr local
        // elsewhere.
        out << pad << "for (" << elemCppType << (elementIsUnique ? "& " : " ")
            << fors->loopVarName << " : " << emitExpression(fors->container.get()) << ") {\n";
        if (isClassType(fors->loopVarType)) {
            // The loop variable is a shared_ptr<T> per iteration -- register it like
            // any other smart-pointer local so member access inside the body lowers
            // `.` to `->` via the existing exprObjectClass machinery.
            noteVar(fors->loopVarName, VarKind::SmartPtr);
            varClass_[fors->loopVarName] = fors->loopVarType;
        } else {
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
        if (bin->operator_str == "<<") {
            return emitExpression(bin->left.get()) + " << " + emitExpression(bin->right.get());
        }
        return "(" + emitExpression(bin->left.get()) + " " + bin->operator_str + " "
               + emitExpression(bin->right.get()) + ")";
    }
    if (auto un = dynamic_cast<const UnaryExpression*>(expr)) {
        // Prefix operator: `!x`, `-x`.
        return un->operator_str + emitExpression(un->operand.get());
    }
    if (auto neww = dynamic_cast<const NewExpression*>(expr)) {
        // `new T(...)` outside a typed declaration: default to make_shared.
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
        std::string methodName;
        if (auto m = dynamic_cast<const MemberAccess*>(call->callee.get())) {
            // Method call `recv.method(args)` -> `recv (-> or .) method(args)`.
            const std::string recv = emitExpression(m->object.get());
            const std::string op = exprObjectClass(m->object.get()).empty() ? "." : "->";
            prefix = recv + op + m->member;
            receiverContainerType = containerVarType(m->object.get());
            if (auto id = dynamic_cast<const Identifier*>(m->object.get())) {
                receiverName = id->name;
            }
            methodName = m->member;
        } else {
            prefix = call->function_name;
        }
        const bool receiverIsVector = isVectorType(receiverContainerType);
        const bool elementIsShared = receiverIsVector && analyzer_.containerElementIsShared(receiverName);
        std::string s = prefix + "(";
        for (size_t i = 0; i < call->arguments.size(); ++i) {
            if (i > 0) s += ", ";
            if (receiverIsVector && vectorMethodSharesArg(methodName, i)) {
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
            s += emitMovedArg(call->arguments[i].get(), calleeParam(*call, i));
        }
        s += ")";
        return s;
    }
    if (auto member = dynamic_cast<const MemberAccess*>(expr)) {
        // `->` if the object is a smart-pointer-to-object (including nested access and
        // this-> fields); `.` for references and plain values.
        const std::string objStr = emitExpression(member->object.get());
        const std::string op = exprObjectClass(member->object.get()).empty() ? "." : "->";
        return objStr + op + member->member;
    }
    if (auto sub = dynamic_cast<const SubscriptExpression*>(expr)) {
        // `v[i]` lowers 1:1 to C++ `v[i]` -- std::vector's operator[] is exactly
        // what CeeLing's subscript syntax means, no translation needed.
        return emitExpression(sub->object.get()) + "[" + emitExpression(sub->index.get()) + "]";
    }
    // Unknown node: best effort.
    return expr->toString();
}

} // namespace cling
