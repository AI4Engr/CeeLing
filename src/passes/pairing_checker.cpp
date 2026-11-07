#include "passes/pairing_checker.h"

namespace cling {

namespace {
// Counts calls to `name` as `f(...)` expression statements anywhere in `body`, nested blocks included
// (widget code lives inside a frame loop). Does not track balance per branch.
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
            // Anchored to the first statement of the body.
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
