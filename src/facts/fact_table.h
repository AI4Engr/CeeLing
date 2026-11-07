#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

namespace cling {

// Library facts as data (docs/design/third-party-inference.md).
//
// A pack is a plain-text `.clfacts` file: header lines `@pack`, `@version`, `@origin`, then one fact per line,
//     <kind> <subject> [flag ...] [key=value ...] source=<declared|inferred> [review=<state>]
// (docs/internals/facts-format.md). The loader is strict: an unknown kind or flag, a missing `source=`, a
// duplicate fact or a missing header line is a load error, because a skipped fact would silently turn a "no"
// into a "yes". A pack is data only, and cannot make a sealed construct legal (raw pointers, `.get()`, casts and
// unions stay rejected by EscapeHatchChecker).
class FactTable {
public:
    // One loaded fact.
    struct Fact {
        std::string kind;                 // "vector.sig", "string.sig", "string_view.sig", "optional.sig", "span.sig", "map.sig", "vector.note", "vector.own", "function", "class", "opaque", "operator", "borrow", "fresh", "mutator"
        std::string subject;              // e.g. "push_back", "std::stoi", "Eigen::MatrixXd|*"
        std::set<std::string> flags;      // e.g. "shares-arg0"
        std::map<std::string, std::string> attrs;   // key=value words other than source= and review=, e.g. result=void
        std::string source;               // "declared" | "inferred"
        std::string review;               // free text after review=, empty if absent
        std::string pack;                 // the @pack it came from
        bool external = false;            // loaded with --facts (a project's pack), not shipped inside the compiler
    };

    // Parses `text` (one pack) and adds it. `originName` is for error messages. Returns "" on success, otherwise
    // one error "origin:LINE: what", and nothing from this text is added.
    // A project pack (`projectPack = true`, what `--facts` loads) may only hold facts about external classes and
    // functions (function, class, opaque, operator, borrow, fresh, mutator), so it cannot relax how std is judged.
    std::string addText(const std::string& text, const std::string& originName, bool projectPack = false);

    // Does the table hold a fact with this kind and subject?
    bool has(const std::string& kind, const std::string& subject) const;
    // Does it hold that fact AND does the fact carry `flag`?
    bool hasFlag(const std::string& kind, const std::string& subject, const std::string& flag) const;

    // Every fact of this kind whose subject starts with `prefix` (sorted by subject).
    std::vector<const Fact*> withPrefix(const std::string& kind, const std::string& prefix) const;

    // Facts a query answered "yes" from, for the trusted-boundary report (--report-facts).
    std::vector<const Fact*> usedFacts() const;

    // The fact with this kind and subject, or nullptr.
    const Fact* find(const std::string& kind, const std::string& subject) const;

    const std::vector<Fact>& facts() const { return facts_; }
    // "pack@version" of every loaded pack, in load order.
    const std::vector<std::string>& packs() const { return packs_; }
    // The `@header` lines of every loaded pack, as "pack: path sha256=hex" (printed by --report-facts).
    const std::vector<std::string>& headers() const { return headers_; }

    // One `@header` line: the header a pack was scanned from. `baseDir` is the pack file's directory (the path is relative to it).
    struct HeaderBinding {
        std::string pack, path, sha256, baseDir;
    };
    const std::vector<HeaderBinding>& headerBindings() const { return bindings_; }

    // Compares the headers recorded by packs added since binding `fromIndex` with the files on disk (content
    // hash, line endings normalised to LF as the scanner does). Returns "" when every existing header matches,
    // otherwise a message naming the pack, header and both hashes. A header that cannot be found is added to
    // `missing` instead. `checked` receives the number of headers compared.
    std::string verifyHeaders(size_t fromIndex, std::vector<std::string>& missing, size_t& checked) const;

private:
    std::vector<Fact> facts_;
    std::map<std::string, size_t> index_;   // "kind|subject" -> position in facts_
    std::vector<std::string> packs_;
    std::vector<std::string> headers_;
    std::vector<HeaderBinding> bindings_;
    mutable std::set<size_t> used_;
};

// The facts in force: the packs shipped inside the compiler (facts/*.clfacts, embedded at build time) plus any
// project packs added with `--facts`. Parsed once. A parse error in a shipped pack prints the error and aborts.
const FactTable& builtinFacts();

// Adds a project pack (`cling --facts file.clfacts`). Call before the first query. Returns "" on success,
// otherwise the error: an unreadable file, a load error, a fact that duplicates a shipped one, or a `@header`
// hash that does not match the file on disk.
std::string addProjectFacts(const std::string& path);
// With strict headers on, a pack whose `@header` file cannot be found is an error instead of a warning.
// Set it before the first addProjectFacts.
void setStrictHeaders(bool strict);

}  // namespace cling
