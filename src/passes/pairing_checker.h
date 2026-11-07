#pragma once

#include "syntax/ast.h"
#include <string>
#include <vector>

namespace cling {

// A single Begin/End pairing warning. Never an error.
struct PairingWarning {
    int line = 0;
    int column = 0;
    std::string message;
    std::string help;
};

// Advisory check for Begin/End style call pairs (beginGroup/endGroup, pushId/popId, ...) that a
// project may expose for familiarity with ImGui. An unpaired call is not a memory-safety problem
// (ownership already guarantees no leak or dangle), so the findings are warnings and must never
// affect the exit code. A missed imbalance is fine here, unlike in the safety checkers.
//
// It counts calls to each registered pair anywhere in a function or method body, nested blocks
// included. It does not track balance per branch (a begin in an `if` arm and an end in the `else`
// arm count as balanced). A mismatch gives one warning per body, not per call.
class PairingChecker {
public:
    // Registers one (beginName, endName) pair; call once per pair before check().
    void registerPair(const std::string& beginName, const std::string& endName);

    // Returns every imbalance found. The result may be ignored without affecting the generated C++.
    std::vector<PairingWarning> check(const Program& program);

private:
    void checkBody(const std::vector<StatementPtr>& body);

    std::vector<std::pair<std::string, std::string>> pairs_;
    std::vector<PairingWarning> warnings_;
};

} // namespace cling
