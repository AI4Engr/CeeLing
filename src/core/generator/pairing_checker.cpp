#include "pairing_checker.h"

namespace cling {

namespace {
// Count calls to `name` as a bare `f(...)` or `f();` expression statement
// ANYWHERE within `body`, including nested inside if/else/while/for/try
// blocks -- real widget code almost always lives inside a frame loop
// (`while (!shouldClose()) { ... }`), so a checker that only looked at a
// function body's OWN top-level statements would essentially never fire in
// practice (confirmed by probe: it missed a real missing endGroup() call
// nested two levels down inside `if (ok) { while (...) { ... } }` in
// imgui-cling's own layout_demo.clg). Still deliberately simple: does not
// track branch-specific balance (e.g. begin in the `if` arm, end in the
// `else` arm nets to "balanced" here even though neither single execution
// path is) -- acceptable per this checker's "false negatives are fine, this
// is advisory, not a safety check" design (see pairing_checker.h).
int countCallsRecursive(const std::vector<StatementPtr>& body, const std::string& name) {
    int count = 0;
    for (const auto& stmt : body) {
        if (auto exprStmt = dynamic_cast<const ExpressionStatement*>(stmt.get())) {
            auto call = dynamic_cast<const FunctionCall*>(exprStmt->expression.get());
            if (call != nullptr && call->function_name == name) {
                ++count;
            }
        } else if (auto ifs = dynamic_cast<const IfStatement*>(stmt.get())) {
            count += countCallsRecursive(ifs->body, name);
            count += countCallsRecursive(ifs->elseBody, name);
        } else if (auto whiles = dynamic_cast<const WhileStatement*>(stmt.get())) {
            count += countCallsRecursive(whiles->body, name);
        } else if (auto fors = dynamic_cast<const ForStatement*>(stmt.get())) {
            count += countCallsRecursive(fors->body, name);
        } else if (auto trys = dynamic_cast<const TryStatement*>(stmt.get())) {
            count += countCallsRecursive(trys->body, name);
            count += countCallsRecursive(trys->catchBody, name);
        }
    }
    return count;
}
} // namespace

void PairingChecker::registerPair(const std::string& beginName, const std::string& endName) {
    pairs_.emplace_back(beginName, endName);
}

void PairingChecker::checkBody(const std::vector<StatementPtr>& body) {
    for (const auto& [beginName, endName] : pairs_) {
        const int begins = countCallsRecursive(body, beginName);
        const int ends = countCallsRecursive(body, endName);
        if (begins != ends) {
            PairingWarning w;
            // Anchor to the first statement in the body -- good enough for an
            // advisory message; per-call attribution isn't needed since this
            // is a per-body imbalance, not a per-call error.
            if (!body.empty()) {
                w.line = body.front()->loc.line;
                w.column = body.front()->loc.column;
            }
            w.message = beginName + "()/" + endName + "() call count mismatch (" +
                        std::to_string(begins) + " vs " + std::to_string(ends) + ")";
            w.help = "this is a style warning, not a safety error -- CeeLing manages " +
                     beginName + "'s object lifetime regardless of whether " + endName +
                     " is called; double-check this is intentional";
            warnings_.push_back(w);
        }
    }
}

std::vector<PairingWarning> PairingChecker::check(const Program& program) {
    warnings_.clear();
    if (pairs_.empty()) return warnings_;

    for (const auto& stmt : program.statements) {
        if (auto fn = dynamic_cast<const FunctionDeclaration*>(stmt.get())) {
            checkBody(fn->body);
        } else if (auto mn = dynamic_cast<const MainFunction*>(stmt.get())) {
            checkBody(mn->body);
        } else if (auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
            for (const auto& member : cls->members) {
                if (auto method = dynamic_cast<const MethodDeclaration*>(member.get())) {
                    checkBody(method->body);
                } else if (auto ctor = dynamic_cast<const ConstructorDeclaration*>(member.get())) {
                    checkBody(ctor->body);
                }
            }
        }
    }
    return warnings_;
}

} // namespace cling
