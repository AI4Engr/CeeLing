#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

namespace cling {

// Library facts as data (TODO.md "Third-party library facts", PLAN_third_party_inference.md).
//
// A pack is a plain-text `.clfacts` file: header lines `@pack`, `@version`, `@origin`, then one fact per line,
//     <kind> <subject> [flag ...] [key=value ...] source=<declared|inferred> [review=<state>]
// (docs/internals/facts-format.md). The loader is strict on purpose: an unknown kind, an unknown flag for a kind,
// a missing `source=`, a duplicate fact or a missing header line is a load error, never silently ignored, because a
// fact the compiler skips would turn a "no" into a "yes" for nothing to notice.
//
// Packs are DATA ONLY. Loading one never runs anything and never opens a file other than the one it was given.
//
// What a fact does NOT do: it cannot make a sealed construct legal. Raw pointers, `.get()`, casts and unions stay
// rejected by EscapeHatchChecker whatever a pack says.
class FactTable {
public:
    // One loaded fact.
    struct Fact {
        std::string kind;                 // "vector.sig", "string.sig", "map.sig", "vector.note", "vector.own", "function", "class", "operator", "borrow"
        std::string subject;              // e.g. "push_back", "std::stoi", "Eigen::MatrixXd|*"
        std::set<std::string> flags;      // e.g. "shares-arg0"
        std::map<std::string, std::string> attrs;   // key=value words other than source= and review=, e.g. result=void
        std::string source;               // "declared" | "inferred"
        std::string review;               // free text after review=, empty if absent
        std::string pack;                 // the @pack it came from
    };

    // Parses `text` (the content of one pack) and adds it. `originName` is used in error messages only.
    // Returns an empty string on success, otherwise one error message of the form "origin:LINE: what".
    // On error nothing from this text is added.
    std::string addText(const std::string& text, const std::string& originName);

    // Does the table hold a fact with this kind and subject?
    bool has(const std::string& kind, const std::string& subject) const;
    // Does it hold that fact AND does the fact carry `flag`?
    bool hasFlag(const std::string& kind, const std::string& subject, const std::string& flag) const;

    // Every fact of this kind whose subject starts with `prefix` (sorted by subject).
    std::vector<const Fact*> withPrefix(const std::string& kind, const std::string& prefix) const;

    // The fact with this kind and subject, or nullptr.
    const Fact* find(const std::string& kind, const std::string& subject) const;

    const std::vector<Fact>& facts() const { return facts_; }
    // "pack@version" of every loaded pack, in load order (what `--report` will print).
    const std::vector<std::string>& packs() const { return packs_; }

private:
    std::vector<Fact> facts_;
    std::map<std::string, size_t> index_;   // "kind|subject" -> position in facts_
    std::vector<std::string> packs_;
};

// The packs that ship inside the compiler (facts/*.clfacts, embedded at build time). Parsed once. A parse error in a
// shipped pack is a build defect: it prints the error and aborts the process rather than running with half a table.
const FactTable& builtinFacts();

}  // namespace cling
