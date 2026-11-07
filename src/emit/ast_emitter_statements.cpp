// AstEmitter: moved arguments, assignments and statements
#include "emit/ast_emitter.h"
#include <functional>
#include "syntax/class_hierarchy.h"
#include "facts/library_summary.h"
#include "emit/runtime_helpers.h"
#include "passes/channel_check.h"

#include "emit/ast_emitter_util.h"

namespace cling {

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

} // namespace cling
