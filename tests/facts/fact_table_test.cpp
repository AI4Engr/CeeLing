// Tests for the fact-pack loader (src/core/generator/fact_table.cpp) and for the packs that ship in the compiler.
// The loader is strict on purpose: everything it does not understand must be an error, never silently skipped.

#include <cstdio>
#include <string>

#include "generator/fact_table.h"
#include "generator/library_summary.h"

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what.c_str());
        ++failures;
    }
}

const char* kHeader = "@pack t\n@version 1\n@origin test\n";

// Loading `body` after a valid header must fail, and the message must mention `needle`.
void rejects(const std::string& body, const std::string& needle) {
    cling::FactTable t;
    std::string err = t.addText(std::string(kHeader) + body, "t.clfacts");
    check(!err.empty(), "should be rejected: " + body);
    check(err.find(needle) != std::string::npos, "message for `" + body + "` should mention `" + needle + "`, got: " + err);
    check(t.facts().empty(), "a failed load must add nothing: " + body);
}

}  // namespace

int main() {
    using cling::FactTable;

    {   // a valid pack
        FactTable t;
        std::string err = t.addText(std::string(kHeader) +
                                    "# a comment\n\nvector.method push_back invalidates shares-arg0 source=declared review=hand\n"
                                    "function std::stoi source=inferred\r\n",
                                    "t.clfacts");
        check(err.empty(), "valid pack should load: " + err);
        check(t.has("vector.method", "push_back"), "push_back present");
        check(t.hasFlag("vector.method", "push_back", "invalidates"), "invalidates flag");
        check(t.hasFlag("vector.method", "push_back", "shares-arg0"), "shares-arg0 flag");
        check(!t.hasFlag("vector.method", "push_back", "nothing"), "absent flag");
        check(!t.has("vector.method", "pop_back"), "absent fact");
        check(!t.has("string.method", "push_back"), "kind is part of the key");
        check(t.has("function", "std::stoi"), "CRLF line handled");
        check(t.facts().size() == 2, "two facts");
        check(t.facts()[1].source == "inferred", "source kept");
        check(t.facts()[0].review == "hand", "review kept");
        check(t.packs().size() == 1 && t.packs()[0] == "t@1", "pack list");
    }

    // everything the loader does not understand is an error
    rejects("frob x source=declared\n", "unknown fact kind");
    rejects("vector.method push_back wobbles source=declared\n", "unknown flag");
    rejects("string.method clear shares-arg0 source=declared\n", "unknown flag");          // a flag of another kind
    rejects("function f\n", "needs");                                                         // too few words
    rejects("function f review=hand\n", "missing `source=`");
    rejects("function f source=guessed\n", "source must be");
    rejects("function f source=declared\nfunction f source=declared\n", "duplicate");
    rejects("@color red\n", "unknown header line");
    rejects("@pack\n", "no value");

    {   // header problems
        FactTable t;
        check(!t.addText("function f source=declared\n", "t").empty(), "fact before header");
        check(!t.addText("@pack p\n@version 1\nfunction f source=declared\n", "t").empty(), "missing @origin");
        check(!t.addText("", "t").empty(), "empty file has no header");
        check(t.facts().empty(), "nothing added on any failure");
    }

    {   // duplicates across two packs are also rejected, and the first pack stays intact
        FactTable t;
        check(t.addText(std::string(kHeader) + "function f source=declared\n", "a").empty(), "first load");
        check(!t.addText(std::string(kHeader) + "function f source=declared\n", "b").empty(), "same fact in a second pack");
        check(t.facts().size() == 1, "first pack intact");
    }

    {   // the shipped packs: they loaded (otherwise builtinFacts() would have aborted) and carry what the old tables had
        const FactTable& b = cling::builtinFacts();
        check(b.packs().size() == 3, "three shipped packs, got " + std::to_string(b.packs().size()));
        for (const auto& f : b.facts()) {
            check(f.source == "declared" && f.review == "hand", "shipped fact is declared+hand until a scan replaces it: " + f.subject);
        }
        check(cling::isKnownVectorMethod("push_back") && cling::isKnownVectorMethod("copyWithin"), "vector names");
        check(!cling::isKnownVectorMethod("insert"), "insert is not admitted (yet)");
        check(cling::vectorMethodInvalidatesReferences("reserve"), "reserve invalidates");
        check(!cling::vectorMethodInvalidatesReferences("size"), "size does not");
        check(cling::vectorMethodSharesArg("push_back", 0) && !cling::vectorMethodSharesArg("push_back", 1), "push_back shares arg 0");
        check(!cling::isKnownStringMethod("c_str") && !cling::isKnownStringMethod("data"), "raw-pointer string methods stay out");
        check(cling::isKnownBorrowingMethod("Eigen::MatrixXd", "block") && !cling::isKnownBorrowingMethod("Eigen::MatrixXd", "Unit"), "eigen borrow");
    }

    if (failures == 0) std::printf("fact_table_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
