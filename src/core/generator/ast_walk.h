#pragma once
// A small recursive walk over every statement and expression of a program (including the bodies of functions, methods,
// constructors, closures and the members of classes). Shared by the passes that look for particular constructs.
#include <functional>

#include "../cst_to_ast/ast.h"

namespace cling {
namespace astwalk {

using NodeFn = std::function<void(ASTNode&)>;

inline void walkStatement(Statement* stmt, const NodeFn& fn);

inline void walkStatements(std::vector<StatementPtr>& body, const NodeFn& fn) {
    for (auto& s : body) walkStatement(s.get(), fn);
}

inline void walkExpression(Expression* expr, const NodeFn& fn) {
    if (expr == nullptr) return;
    fn(*expr);
    if (auto call = dynamic_cast<FunctionCall*>(expr)) {
        walkExpression(call->callee.get(), fn);
        for (auto& a : call->arguments) walkExpression(a.get(), fn);
    } else if (auto bin = dynamic_cast<BinaryExpression*>(expr)) {
        walkExpression(bin->left.get(), fn);
        walkExpression(bin->right.get(), fn);
    } else if (auto un = dynamic_cast<UnaryExpression*>(expr)) {
        walkExpression(un->operand.get(), fn);
    } else if (auto mem = dynamic_cast<MemberAccess*>(expr)) {
        walkExpression(mem->object.get(), fn);
    } else if (auto sub = dynamic_cast<SubscriptExpression*>(expr)) {
        walkExpression(sub->object.get(), fn);
        walkExpression(sub->index.get(), fn);
    } else if (auto nw = dynamic_cast<NewExpression*>(expr)) {
        for (auto& a : nw->arguments) walkExpression(a.get(), fn);
    } else if (auto closure = dynamic_cast<ClosureExpression*>(expr)) {
        walkStatements(closure->body, fn);
    }
}

// Visits every statement and expression below `stmt` (including `stmt` itself).
inline void walkStatement(Statement* stmt, const NodeFn& fn) {
    if (stmt == nullptr) return;
    fn(*stmt);
    if (auto d = dynamic_cast<VariableDeclaration*>(stmt)) {
        walkExpression(d->expression.get(), fn);
    } else if (auto a = dynamic_cast<AssignmentStatement*>(stmt)) {
        walkExpression(a->target.get(), fn);
        walkExpression(a->expression.get(), fn);
    } else if (auto e = dynamic_cast<ExpressionStatement*>(stmt)) {
        walkExpression(e->expression.get(), fn);
    } else if (auto r = dynamic_cast<ReturnStatement*>(stmt)) {
        walkExpression(r->expression.get(), fn);
    } else if (auto t = dynamic_cast<ThrowStatement*>(stmt)) {
        walkExpression(t->expression.get(), fn);
    } else if (auto f = dynamic_cast<FunctionDeclaration*>(stmt)) {
        walkStatements(f->body, fn);
    } else if (auto m = dynamic_cast<MethodDeclaration*>(stmt)) {
        walkStatements(m->body, fn);
    } else if (auto c = dynamic_cast<ConstructorDeclaration*>(stmt)) {
        walkStatements(c->body, fn);
    } else if (auto mf = dynamic_cast<MainFunction*>(stmt)) {
        walkStatements(mf->body, fn);
    } else if (auto i = dynamic_cast<IfStatement*>(stmt)) {
        walkExpression(i->condition.get(), fn);
        walkStatements(i->body, fn);
        walkStatements(i->elseBody, fn);
    } else if (auto w = dynamic_cast<WhileStatement*>(stmt)) {
        walkExpression(w->condition.get(), fn);
        walkStatements(w->body, fn);
    } else if (auto fo = dynamic_cast<ForStatement*>(stmt)) {
        walkExpression(fo->container.get(), fn);
        walkStatements(fo->body, fn);
    } else if (auto tr = dynamic_cast<TryStatement*>(stmt)) {
        walkStatements(tr->body, fn);
        walkStatements(tr->catchBody, fn);
    } else if (auto cls = dynamic_cast<ClassDeclaration*>(stmt)) {
        walkStatements(cls->members, fn);
    }
}


}  // namespace astwalk
}  // namespace cling
