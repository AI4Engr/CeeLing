#include "ast_emitter.h"

namespace cling {

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
                            "#include <iostream>"}) {
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
        if (auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
            classNames_.insert(cls->name);
            auto& fields = classFields_[cls->name];
            for (const auto& member : cls->members) {
                if (auto var = dynamic_cast<const VariableDeclaration*>(member.get())) {
                    fields[var->identifier] = var->type;
                } else if (auto method = dynamic_cast<const MethodDeclaration*>(member.get())) {
                    classMethodParams_[cls->name][method->name] = method->parameters;
                }
            }
        } else if (auto fn = dynamic_cast<const FunctionDeclaration*>(stmt.get())) {
            functionParams_[fn->name] = fn->parameters;
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
        auto it = varClass_.find(id->name);
        return it == varClass_.end() ? "" : it->second;
    }
    if (auto member = dynamic_cast<const MemberAccess*>(expr)) {
        // The class of `a.b` is the declared type of field b in a's class.
        const std::string objClass = exprObjectClass(member->object.get());
        if (objClass.empty()) return "";
        const std::string ft = fieldType(objClass, member->member);
        return isClassType(ft) ? ft : "";
    }
    return "";
}

bool AstEmitter::isClassType(const std::string& type) const {
    return classNames_.find(type) != classNames_.end();
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
                                   const std::vector<ExpressionPtr>& args) {
    const std::string ptr = smartPointerType(className, varName);
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
    out << "class " << cls.name << " {\npublic:\n";
    for (const auto& member : cls.members) {
        if (auto var = dynamic_cast<const VariableDeclaration*>(member.get())) {
            // Member field. Object-typed fields become smart pointers.
            if (isClassType(var->type)) {
                out << indentStr(1) << smartPointerType(var->type, var->identifier)
                    << "<" << var->type << "> " << var->identifier << ";\n";
            } else {
                out << indentStr(1) << var->type << " " << var->identifier << ";\n";
            }
        } else if (auto ctor = dynamic_cast<const ConstructorDeclaration*>(member.get())) {
            emitFunction("", ctor->name, ctor->parameters, ctor->body, out, cls.name, 1);
        } else if (auto method = dynamic_cast<const MethodDeclaration*>(member.get())) {
            emitFunction(method->returnType, method->name, method->parameters, method->body,
                         out, cls.name, 1, method->isConst);
        }
    }
    out << "};\n\n";
}

void AstEmitter::emitFunction(const std::string& returnType, const std::string& name,
                              const std::vector<std::pair<std::string, std::string>>& params,
                              const std::vector<StatementPtr>& body, std::ostringstream& out,
                              const std::string& ownerClass, int baseIndent, bool isConst) {
    // Each function has its own local variable scope; `currentClass_` enables `this->`
    // for field access inside a method/constructor.
    varKinds_.clear();
    varClass_.clear();
    const std::string savedClass = currentClass_;
    currentClass_ = ownerClass;

    // Signature. An object return type is lowered to a smart pointer so the body's
    // owning local (a smart pointer) can be returned directly (unique_ptr by default:
    // the callee constructs the object and hands sole ownership back to the caller).
    std::string sig;
    if (!returnType.empty()) {
        sig += isClassType(returnType) ? "std::unique_ptr<" + returnType + "> "
                                       : returnType + " ";
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
            sig += ptype + " " + pname;
            noteVar(pname, VarKind::Value);
        }
    }
    sig += ")";
    if (isConst) sig += " const";
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
        if (varClass_.find(id->name) != varClass_.end() && !isCurrentField(id->name)) {
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
            // Observer: `ref T b = a;` -> `T& b = *a;` (native reference to the object).
            noteVar(decl->identifier, VarKind::Reference);
            out << pad << decl->type << "& " << decl->identifier << " = "
                << "*(" << emitExpression(decl->expression.get()) << ");\n";
        } else if (isClassType(decl->type)) {
            const std::string ptr = smartPointerType(decl->type, decl->identifier);
            std::string rhs;
            if (auto neww = dynamic_cast<const NewExpression*>(decl->expression.get())) {
                rhs = factoryFor(neww->className, decl->identifier, neww->arguments);
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
            out << pad << decl->type << " " << decl->identifier << " = "
                << emitExpression(decl->expression.get()) << ";\n";
        }
    } else if (auto assign = dynamic_cast<const AssignmentStatement*>(stmt)) {
        // Left-hand side: a member target (`a.b = ...`) is emitted via the member-access
        // path (so `.`/`->` and this-> resolve); a bare name gets this-> if it's a field.
        std::string lhs;
        std::string ptrVar = assign->variable;  // name used to pick the target's ptr kind
        const bool memberTarget = dynamic_cast<const MemberAccess*>(assign->target.get()) != nullptr;
        if (auto m = dynamic_cast<const MemberAccess*>(assign->target.get())) {
            lhs = emitExpression(m);
            ptrVar = m->member;  // the field being assigned
        } else {
            lhs = isCurrentField(assign->variable) ? "this->" + assign->variable
                                                   : assign->variable;
        }
        std::string rhs;
        const bool rhsIsNullptr = [&] {
            auto id = dynamic_cast<const Identifier*>(assign->expression.get());
            return id != nullptr && id->name == "nullptr";
        }();
        if (auto neww = dynamic_cast<const NewExpression*>(assign->expression.get())) {
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
    } else if (auto exprStmt = dynamic_cast<const ExpressionStatement*>(stmt)) {
        out << pad << emitExpression(exprStmt->expression.get()) << ";\n";
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
        if (auto m = dynamic_cast<const MemberAccess*>(call->callee.get())) {
            // Method call `recv.method(args)` -> `recv (-> or .) method(args)`.
            const std::string recv = emitExpression(m->object.get());
            const std::string op = exprObjectClass(m->object.get()).empty() ? "." : "->";
            prefix = recv + op + m->member;
        } else {
            prefix = call->function_name;
        }
        std::string s = prefix + "(";
        for (size_t i = 0; i < call->arguments.size(); ++i) {
            if (i > 0) s += ", ";
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
    // Unknown node: best effort.
    return expr->toString();
}

} // namespace cling
