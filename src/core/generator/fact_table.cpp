#include "fact_table.h"

#include <cstdio>
#include <cstdlib>
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

// Which flags each kind accepts. A kind not in this map is a load error.
const std::map<std::string, std::set<std::string>>& kindFlags() {
    static const std::map<std::string, std::set<std::string>> m = {
        {"vector.method", {"invalidates", "shares-arg0"}},
        {"string.method", {"invalidates"}},
        {"function", {}},
        {"class", {}},
        {"operator", {}},
        {"borrow", {}},
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

std::string FactTable::addText(const std::string& text, const std::string& originName) {
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

        auto kindIt = kindFlags().find(words[0]);
        if (kindIt == kindFlags().end()) return fail(lineNo, "unknown fact kind `" + words[0] + "`");

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
            } else if (kindIt->second.count(w) > 0) {
                f.flags.insert(w);
            } else {
                return fail(lineNo, "unknown flag `" + w + "` for kind `" + f.kind + "`");
            }
        }
        if (f.source.empty()) return fail(lineNo, "missing `source=`");

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
    return index_.count(kind + "|" + subject) > 0;
}

bool FactTable::hasFlag(const std::string& kind, const std::string& subject, const std::string& flag) const {
    auto it = index_.find(kind + "|" + subject);
    return it != index_.end() && facts_[it->second].flags.count(flag) > 0;
}

const FactTable& builtinFacts() {
    static const FactTable table = [] {
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

}  // namespace cling
