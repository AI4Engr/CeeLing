// AstEmitter: expressions
#include "emit/ast_emitter.h"
#include <functional>
#include "syntax/class_hierarchy.h"
#include "facts/library_summary.h"
#include "emit/runtime_helpers.h"
#include "passes/channel_check.h"

#include "emit/ast_emitter_util.h"

namespace cling {

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
            if (call->callee == nullptr) {
                if (auto refs = externRefParamsByName_.find(call->function_name);
                    refs != externRefParamsByName_.end() && refs->second.count(i) > 0) {
                    s += emitExpression(call->arguments[i].get());      // an ext_fn `ref` parameter borrows: no std::move
                    continue;
                }
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
