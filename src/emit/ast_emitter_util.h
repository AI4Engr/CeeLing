#pragma once
// Helpers shared by the AstEmitter translation units.

#include <functional>
#include <set>
#include <string>

#include "syntax/ast.h"

namespace cling {
inline bool isNullptrLiteral(const Expression* expr) {
    auto id = dynamic_cast<const Identifier*>(expr);
    return id != nullptr && id->name == "nullptr";
}

// An initializer list runs in declaration order, so an expression may move into it only if it reads parameters, names that are not
// own fields, or own fields already hoisted. `this` and calls of the class's own methods are never safe. Otherwise hoisting stops
// there and the statement stays in the body.
inline bool initializerExprIsSafe(const Expression* expr, const std::set<std::string>& ownFields, const std::set<std::string>& hoisted,
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

} // namespace cling
