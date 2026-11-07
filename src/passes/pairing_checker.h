#pragma once

#include "syntax/ast.h"
#include <string>
#include <vector>

namespace cling {

// A single Begin/End pairing WARNING -- never an error. See PairingChecker's
// own doc comment for why this is safe to be advisory-only.
struct PairingWarning {
    int line = 0;
    int column = 0;
    std::string message;
    std::string help;
};

// Advisory-only imbalance check for ImGui-API-compatibility call pairs
// (beginGroup/endGroup, pushId/popId, ...) that imgui-cling (or any CeeLing
// project imitating ImGui's Begin/End call-pair style) may choose to expose
// for source-level familiarity, per the project owner's explicit direction:
// "we manage all memory ourselves, so an unpaired Begin/End is not a memory
// safety problem -- just warn, don't reject."
//
// This is DELIBERATELY separate from EscapeHatchChecker/DanglingChecker: those
// two exist to enforce CeeLing's safety CONTRACT (default-reject, never
// default-allow) and their errors always abort compilation. PairingChecker's
// findings are NEVER safety-relevant -- CeeLing's ownership model already
// guarantees no leak/dangle/double-free regardless of whether a Begin call's
// matching End was ever made (an "unpaired Begin" just means a plain object
// call was made, and the object's lifetime is managed the same way as any
// other CeeLing value, entirely independent of this style convention). So
// PairingChecker's output must NEVER affect the process exit code, and its
// analysis is intentionally conservative/best-effort in the OPPOSITE
// direction from the safety checkers: a missed imbalance (false negative) is
// completely fine here (unlike a safety checker, where a false negative would
// be a real hole) -- it only means a real style mistake goes unremarked, not
// that anything unsafe was allowed through.
//
// Scope (v2): counts calls to one declared (beginName, endName) pair
// ANYWHERE within a function/method body, including nested inside
// if/else/while/for/try blocks -- v1 only looked at a body's own top-level
// statements, which meant it never fired on real widget code (a frame loop's
// content always lives inside `while (!shouldClose()) { ... }`, at minimum
// one level of nesting deep). Confirmed by probe against imgui-cling's own
// layout_demo.clg: v1 silently missed a deliberately-removed endGroup() call
// nested inside `if (ok) { while (...) { ... } }`; v2 catches it. Still
// deliberately simple: does NOT track PER-BRANCH balance (a begin in an `if`
// arm and the matching end in the `else` arm nets to "balanced" here, even
// though neither single execution path actually is) -- acceptable per this
// checker's "false negatives are fine, this is advisory, not a safety check"
// design above. A mismatch between the begin-count and end-count for a body
// is reported as one warning per body, not per call.
class PairingChecker {
public:
    // Registers one (beginName, endName) pair to watch for imbalance within
    // each function/method body, independently of every other registered
    // pair. Call once per pair before check().
    void registerPair(const std::string& beginName, const std::string& endName);

    // Returns every imbalance found across the whole program. Always safe to
    // ignore/discard the result and still emit correct, safe C++ -- see class
    // doc comment.
    std::vector<PairingWarning> check(const Program& program);

private:
    void checkBody(const std::vector<StatementPtr>& body);

    std::vector<std::pair<std::string, std::string>> pairs_;
    std::vector<PairingWarning> warnings_;
};

} // namespace cling
