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
        std::string kind;                 // "vector.sig", "string.sig", "map.sig", "vector.note", "vector.own", "function", "class", "opaque", "operator", "borrow", "fresh", "mutator"
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
    //
    // A PROJECT pack (`projectPack = true`, what `--facts` loads) may only hold facts about external classes and functions
    // (function, class, opaque, operator, borrow, fresh, mutator). It cannot touch how `std` is judged (the *.sig, vector.note, vector.own kinds), so a
    // project can add a library but never relax what the compiler assumes about the standard library.
    std::string addText(const std::string& text, const std::string& originName, bool projectPack = false);

    // Does the table hold a fact with this kind and subject?
    bool has(const std::string& kind, const std::string& subject) const;
    // Does it hold that fact AND does the fact carry `flag`?
    bool hasFlag(const std::string& kind, const std::string& subject, const std::string& flag) const;

    // Every fact of this kind whose subject starts with `prefix` (sorted by subject).
    std::vector<const Fact*> withPrefix(const std::string& kind, const std::string& prefix) const;

    // Facts a query answered "yes" from since the table was loaded, for the trusted-boundary report (--report-facts).
    std::vector<const Fact*> usedFacts() const;

    // The fact with this kind and subject, or nullptr.
    const Fact* find(const std::string& kind, const std::string& subject) const;

    const std::vector<Fact>& facts() const { return facts_; }
    // "pack@version" of every loaded pack, in load order (what `--report` will print).
    const std::vector<std::string>& packs() const { return packs_; }
    // The `@header <path> sha256=<hex>` lines of every loaded pack (what a scan read), "pack: path sha256=hex", as --report-facts prints them.
    const std::vector<std::string>& headers() const { return headers_; }

    // One `@header` line: the header a pack was scanned from. `baseDir` is the directory of the pack file (the recorded path is relative to it).
    struct HeaderBinding {
        std::string pack, path, sha256, baseDir;
    };
    const std::vector<HeaderBinding>& headerBindings() const { return bindings_; }

    // Compares the headers recorded by the packs added since binding number `fromIndex` with the files on disk (content hash, line endings
    // normalised to LF as the scanner does). Returns an empty string when every header that exists matches; otherwise a message naming the
    // pack, the header and both hashes. A header that cannot be found only adds a line to `missing` (a different directory layout is not a
    // different library): it is reported, never silently used as proof. `checked` receives the number of headers compared.
    std::string verifyHeaders(size_t fromIndex, std::vector<std::string>& missing, size_t& checked) const;

private:
    std::vector<Fact> facts_;
    std::map<std::string, size_t> index_;   // "kind|subject" -> position in facts_
    std::vector<std::string> packs_;
    std::vector<std::string> headers_;
    std::vector<HeaderBinding> bindings_;
    mutable std::set<size_t> used_;
};

// The facts in force: the packs that ship inside the compiler (facts/*.clfacts, embedded at build time) plus any project packs
// added with `--facts`. Parsed once. A parse error in a shipped pack is a build defect: it prints the error and aborts the
// process rather than running with half a table.
const FactTable& builtinFacts();

// Adds a project pack (`cling --facts file.clfacts`) to the facts in force. Call it before the first query. Returns an empty
// string on success, otherwise the error (unreadable file, or any load error of FactTable::addText, including a fact that
// duplicates a shipped one: a project pack can ADD facts, never replace or weaken a shipped one).
std::string addProjectFacts(const std::string& path);
// With strict headers on, a pack whose `@header` file cannot be found at the recorded place is an error instead of a warning.
// Set it BEFORE the first addProjectFacts (main.cpp reads `--strict-headers` from the whole command line first).
void setStrictHeaders(bool strict);
// (It also verifies the pack's `@header` hashes against the files on disk and returns an error on a mismatch.)

}  // namespace cling
