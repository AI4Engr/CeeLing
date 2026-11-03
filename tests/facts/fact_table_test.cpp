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
                                    "# a comment\n\nvector.note push_back shares-arg0 source=declared review=hand\n"
                                    "function std::stoi source=inferred\r\n",
                                    "t.clfacts");
        check(err.empty(), "valid pack should load: " + err);
        check(t.has("vector.note", "push_back"), "push_back present");
        check(t.hasFlag("vector.note", "push_back", "shares-arg0"), "shares-arg0 flag");
        check(!t.hasFlag("vector.note", "push_back", "nothing"), "absent flag");
        check(!t.has("vector.note", "pop_back"), "absent fact");
        check(!t.has("vector.own", "push_back"), "kind is part of the key");
        check(t.has("function", "std::stoi"), "CRLF line handled");
        check(t.facts().size() == 2, "two facts");
        check(t.facts()[1].source == "inferred", "source kept");
        check(t.facts()[0].review == "hand", "review kept");
        check(t.packs().size() == 1 && t.packs()[0] == "t@1", "pack list");
    }

    {   // a scanned signature fact
        FactTable t;
        std::string err = t.addText(std::string(kHeader) +
                                    "vector.sig f/const/- result=void,scalar mutates=no partial=no impls=a,b source=inferred review=unreviewed\n",
                                    "t.clfacts");
        check(err.empty(), "sig fact should load: " + err);
        const FactTable::Fact* f = t.find("vector.sig", "f/const/-");
        check(f != nullptr && f->attrs.at("result") == "void,scalar" && f->attrs.at("mutates") == "no", "sig attributes kept");
        check(t.find("vector.sig", "g/const/-") == nullptr, "find of an absent fact");
        check(t.withPrefix("vector.sig", "f/").size() == 1 && t.withPrefix("vector.sig", "g/").empty(), "withPrefix");
    }

    // everything the loader does not understand is an error
    rejects("frob x source=declared\n", "unknown fact kind");
    rejects("vector.note push_back wobbles source=declared\n", "unknown flag");
    rejects("vector.own copyWithin shares-arg0 source=declared\n", "unknown flag");          // a flag of another kind
    rejects("vector.sig f/const/- result=void mutates=no colour=red source=inferred\n", "unknown attribute");
    rejects("vector.sig f/const/- result=void mutates=maybe source=inferred\n", "bad value");
    rejects("vector.sig f/const/- mutates=no source=inferred\n", "needs `result=`");   // a .sig fact needs result and mutates
    rejects("vector.sig f/const/- result=void result=scalar mutates=no source=inferred\n", "given twice");
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

    {   // the shipped packs: they loaded (otherwise builtinFacts() would have aborted)
        const FactTable& b = cling::builtinFacts();
        check(b.packs().size() == 6, "six shipped packs, got " + std::to_string(b.packs().size()));
        for (const auto& f : b.facts()) {
            bool scanned = f.kind == "vector.sig" || f.kind == "string.sig" || f.kind == "map.sig";
            check(scanned ? (f.source == "inferred" && f.review == "unreviewed") : (f.source == "declared" && f.review == "hand"),
                  "provenance of a shipped fact: " + f.kind + " " + f.subject);
        }
        // admission is computed from the scanned signatures, not listed
        for (const char* ok : {"push_back", "pop_back", "size", "resize", "reserve", "clear", "empty", "capacity", "assign", "shrink_to_fit", "copyWithin"}) {
            check(cling::isKnownVectorMethod(ok), std::string("vector method admitted by rule: ") + ok);
        }
        for (const char* no : {"begin", "end", "data", "insert", "erase", "emplace_back", "at", "front", "back", "swap", "get_allocator", "frobnicate"}) {
            check(!cling::isKnownVectorMethod(no), std::string("vector method refused by rule: ") + no);
            check(!cling::vectorMethodRefusal(no).empty(), std::string("a refusal reason is given: ") + no);
        }
        check(cling::vectorMethodRefusal("push_back").empty(), "no refusal reason for an admitted method");
        for (const char* ok : {"length", "size", "empty", "substr", "find", "append", "clear", "push_back", "reserve", "compare", "assign", "resize", "insert", "erase"}) {   // callable overloads return *this
            check(cling::isKnownStringMethod(ok), std::string("string method admitted by rule: ") + ok);
        }
        for (const char* no : {"c_str", "data", "begin", "end", "copy", "swap", "front", "at", "copy_not_there"}) {
            check(!cling::isKnownStringMethod(no), std::string("string method refused by rule: ") + no);
        }
        check(cling::vectorMethodInvalidatesReferences("reserve") && cling::vectorMethodInvalidatesReferences("clear") &&
                  cling::vectorMethodInvalidatesReferences("assign"), "derived mutators invalidate");
        check(!cling::vectorMethodInvalidatesReferences("size") && !cling::vectorMethodInvalidatesReferences("empty"), "readers do not");
        // std::map: only the methods whose callable overloads return nothing or a number
        for (const char* ok : {"size", "empty", "clear", "count", "erase", "max_size"}) {
            check(cling::isKnownMapMethod(ok), std::string("map method admitted by rule: ") + ok);
        }
        for (const char* no : {"find", "begin", "end", "at", "insert", "emplace", "swap", "merge", "lower_bound", "try_emplace", "extract", "frobnicate"}) {
            check(!cling::isKnownMapMethod(no), std::string("map method refused by rule: ") + no);
            check(!cling::mapMethodRefusal(no).empty(), std::string("a map refusal reason is given: ") + no);
        }
        check(cling::mapMethodInvalidatesReferences("erase") && cling::mapMethodInvalidatesReferences("clear") && !cling::mapMethodInvalidatesReferences("size"), "map mutators");
        check(cling::vectorMethodRefusal("erase").find("cannot write") != std::string::npos, "vector erase: every overload needs an iterator");
        check(cling::vectorMethodSharesArg("push_back", 0) && !cling::vectorMethodSharesArg("push_back", 1), "push_back shares arg 0");
        check(cling::isKnownBorrowingMethod("Eigen::MatrixXd", "block") && !cling::isKnownBorrowingMethod("Eigen::MatrixXd", "Unit"), "eigen borrow");
    }

    if (failures == 0) std::printf("fact_table_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
