#include "fact_table.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace cling {

// Defined in the generated builtin_facts_data.cpp (cmake/embed_facts.cmake): the bytes of every shipped pack.
struct BuiltinPackText {
    const char* name;
    const unsigned char* data;
    unsigned long size;
};
extern const BuiltinPackText kBuiltinPacks[];
extern const unsigned long kBuiltinPackCount;

namespace {

// What each kind accepts: bare flags, and key=value attributes with their allowed values (an empty set = any value).
// A kind not in this map is a load error.
struct KindSpec {
    std::set<std::string> flags;
    std::map<std::string, std::set<std::string>> keys;
};

const std::map<std::string, KindSpec>& kindSpecs() {
    // A `*.sig` fact is what a scan found for one SIGNATURE, subject `name/const|nonconst/<parameter kinds joined by +>`:
    // the shapes it returns over the scanned implementations (`result`, comma-separated), whether calling it may change the
    // receiver (`mutates`), whether only some of the implementations resolved the result (`partial`), which were scanned (`impls`).
    const KindSpec sig = {{}, {{"result", {}},
                               {"mutates", {"yes", "no"}},
                               {"partial", {"yes", "no"}},
                               {"impls", {}}}};
    static const std::map<std::string, KindSpec> m = {
        {"vector.sig", sig},
        {"string.sig", sig},
        {"map.sig", sig},
        {"vector.note", {{"shares-arg0"}, {}}},   // a declared fact no signature can give (the argument is linked, not consumed)
        {"vector.own", {{}, {}}},                // a method CeeLing itself defines (no std counterpart)
        {"function", {{}, {}}},
        {"class", {{}, {}}},
        {"operator", {{}, {}}},
        {"borrow", {{}, {}}},
        {"fresh", {{}, {}}},          // a method of an external class that returns a fresh value (a number, an owned object): borrows nothing
    };
    return m;
}

std::vector<std::string> splitWords(const std::string& line) {
    std::vector<std::string> out;
    std::istringstream in(line);
    std::string w;
    while (in >> w) out.push_back(w);
    return out;
}

std::string trimRight(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    return s;
}

}  // namespace

std::string FactTable::addText(const std::string& text, const std::string& originName, bool projectPack) {
    std::vector<Fact> parsed;
    std::string pack, version, origin;
    std::set<std::string> seen;
    auto fail = [&](size_t lineNo, const std::string& what) {
        return originName + ":" + std::to_string(lineNo) + ": " + what;
    };

    std::istringstream in(text);
    std::string raw;
    size_t lineNo = 0;
    while (std::getline(in, raw)) {
        ++lineNo;
        std::string line = trimRight(raw);
        size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line[first] == '#') continue;
        std::vector<std::string> words = splitWords(line);

        if (words[0][0] == '@') {
            std::string value;
            for (size_t i = 1; i < words.size(); ++i) value += (i > 1 ? " " : "") + words[i];
            if (value.empty()) return fail(lineNo, "header line `" + words[0] + "` has no value");
            if (words[0] == "@pack") pack = value;
            else if (words[0] == "@version") version = value;
            else if (words[0] == "@origin") origin = value;
            else return fail(lineNo, "unknown header line `" + words[0] + "` (expected @pack, @version or @origin)");
            continue;
        }

        if (pack.empty() || version.empty() || origin.empty()) {
            return fail(lineNo, "a fact comes before the header lines @pack, @version and @origin");
        }
        if (words.size() < 3) return fail(lineNo, "a fact needs `<kind> <subject> source=<declared|inferred>`");

        auto kindIt = kindSpecs().find(words[0]);
        if (kindIt == kindSpecs().end()) return fail(lineNo, "unknown fact kind `" + words[0] + "`");

        if (projectPack && words[0] != "function" && words[0] != "class" && words[0] != "operator" && words[0] != "borrow" && words[0] != "fresh") {
            return fail(lineNo, "a project fact pack may only hold function, class, operator, borrow and fresh facts, not `" + words[0] + "` (it cannot change how std is judged)");
        }
        Fact f;
        f.kind = words[0];
        f.subject = words[1];
        f.pack = pack;
        for (size_t i = 2; i < words.size(); ++i) {
            const std::string& w = words[i];
            if (w.rfind("source=", 0) == 0) {
                f.source = w.substr(7);
                if (f.source != "declared" && f.source != "inferred") {
                    return fail(lineNo, "source must be `declared` or `inferred`, not `" + f.source + "`");
                }
            } else if (w.rfind("review=", 0) == 0) {
                f.review = w.substr(7);
            } else if (kindIt->second.flags.count(w) > 0) {
                f.flags.insert(w);
            } else if (size_t eq = w.find('='); eq != std::string::npos && eq > 0) {
                std::string key = w.substr(0, eq), value = w.substr(eq + 1);
                auto keyIt = kindIt->second.keys.find(key);
                if (keyIt == kindIt->second.keys.end()) {
                    return fail(lineNo, "unknown attribute `" + key + "` for kind `" + f.kind + "`");
                }
                if (value.empty() || (!keyIt->second.empty() && keyIt->second.count(value) == 0)) {
                    return fail(lineNo, "bad value `" + value + "` for attribute `" + key + "`");
                }
                if (!f.attrs.emplace(key, value).second) return fail(lineNo, "attribute `" + key + "` given twice");
            } else {
                return fail(lineNo, "unknown flag `" + w + "` for kind `" + f.kind + "`");
            }
        }
        if (f.source.empty()) return fail(lineNo, "missing `source=`");
        if (f.kind.size() > 4 && f.kind.compare(f.kind.size() - 4, 4, ".sig") == 0) {
            for (const char* required : {"result", "mutates"}) {
                if (f.attrs.count(required) == 0) return fail(lineNo, std::string("a `") + f.kind + "` fact needs `" + required + "=`");
            }
        }

        std::string key = f.kind + "|" + f.subject;
        if (!seen.insert(key).second || index_.count(key) > 0) {
            return fail(lineNo, "duplicate fact `" + f.kind + " " + f.subject + "`");
        }
        parsed.push_back(std::move(f));
    }
    if (pack.empty() || version.empty() || origin.empty()) {
        return fail(lineNo, "missing header line (@pack, @version and @origin are all required)");
    }

    for (auto& f : parsed) {
        index_[f.kind + "|" + f.subject] = facts_.size();
        facts_.push_back(std::move(f));
    }
    packs_.push_back(pack + "@" + version);
    return "";
}

bool FactTable::has(const std::string& kind, const std::string& subject) const {
    auto it = index_.find(kind + "|" + subject);
    if (it == index_.end()) return false;
    used_.insert(it->second);
    return true;
}

std::vector<const FactTable::Fact*> FactTable::withPrefix(const std::string& kind, const std::string& prefix) const {
    std::vector<const Fact*> out;
    const std::string start = kind + "|" + prefix;
    for (auto it = index_.lower_bound(start); it != index_.end() && it->first.compare(0, start.size(), start) == 0; ++it) {
        out.push_back(&facts_[it->second]);
        used_.insert(it->second);
    }
    return out;
}

const FactTable::Fact* FactTable::find(const std::string& kind, const std::string& subject) const {
    auto it = index_.find(kind + "|" + subject);
    if (it == index_.end()) return nullptr;
    used_.insert(it->second);
    return &facts_[it->second];
}

bool FactTable::hasFlag(const std::string& kind, const std::string& subject, const std::string& flag) const {
    auto it = index_.find(kind + "|" + subject);
    if (it == index_.end() || facts_[it->second].flags.count(flag) == 0) return false;
    used_.insert(it->second);
    return true;
}

std::vector<const FactTable::Fact*> FactTable::usedFacts() const {
    std::vector<const Fact*> out;
    for (size_t i : used_) out.push_back(&facts_[i]);
    return out;
}

namespace {
FactTable& factsInForce() {
    static FactTable table = [] {
        FactTable t;
        for (unsigned long i = 0; i < kBuiltinPackCount; ++i) {
            const BuiltinPackText& p = kBuiltinPacks[i];
            std::string err = t.addText(std::string(reinterpret_cast<const char*>(p.data), p.size), p.name);
            if (!err.empty()) {
                std::fprintf(stderr, "cling: built-in fact pack is invalid: %s\n", err.c_str());
                std::abort();
            }
        }
        return t;
    }();
    return table;
}
}  // namespace

const FactTable& builtinFacts() {
    return factsInForce();
}

std::string addProjectFacts(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return path + ": cannot open the fact pack";
    std::ostringstream text;
    text << in.rdbuf();
    return factsInForce().addText(text.str(), path, true);
}

}  // namespace cling
