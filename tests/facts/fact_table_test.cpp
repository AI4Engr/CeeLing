// Tests for the fact-pack loader (src/core/generator/fact_table.cpp) and for the packs that ship in the compiler.
// The loader is strict on purpose: everything it does not understand must be an error, never silently skipped.

#include <cstdio>
#include <string>

#include "generator/fact_table.h"
#include "generator/sha256.h"
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

    {   // SHA-256 (binds a pack to its headers): references computed with Python's hashlib, including every padding boundary
        check(cling::sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256 of abc");
        check(cling::sha256Hex(std::string(1000000, 'a')) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "sha256 of a million a");
        check(cling::sha256Hex(std::string(0, 'x')) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "sha256 of 0 bytes");
        check(cling::sha256Hex(std::string(1, 'x')) == "2d711642b726b04401627ca9fbac32f5c8530fb1903cc4db02258717921a4881", "sha256 of 1 bytes");
        check(cling::sha256Hex(std::string(3, 'x')) == "cd2eb0837c9b4c962c22d2ff8b5441b7b45805887f051d39bf133b583baf6860", "sha256 of 3 bytes");
        check(cling::sha256Hex(std::string(55, 'x')) == "d5e285683cd4efc02d021a5c62014694958901005d6f71e89e0989fac77e4072", "sha256 of 55 bytes");
        check(cling::sha256Hex(std::string(56, 'x')) == "04c26261370ee7541549d16dee320c723e3fd14671e66a099afe0a377c16888e", "sha256 of 56 bytes");
        check(cling::sha256Hex(std::string(57, 'x')) == "ae14a2563ccf969d99aca69ce6bb74981f734bbf9f655f73b8f06db68cab5217", "sha256 of 57 bytes");
        check(cling::sha256Hex(std::string(63, 'x')) == "75220b47218278e656f2013bb8f0c455a25eaf01e86c64924e9d48d89776d6f2", "sha256 of 63 bytes");
        check(cling::sha256Hex(std::string(64, 'x')) == "7ce100971f64e7001e8fe5a51973ecdfe1ced42befe7ee8d5fd6219506b5393c", "sha256 of 64 bytes");
        check(cling::sha256Hex(std::string(65, 'x')) == "9537c5fdf120482f7d58d25e9ed583f52c02b4e304ea814db1633ad565aed7e9", "sha256 of 65 bytes");
        check(cling::sha256Hex(std::string(119, 'x')) == "000b48d4edf0fa7bee3c6236ecd2785baa5db4eeb8bb54341b029e0d9fa5fb0c", "sha256 of 119 bytes");
        check(cling::sha256Hex(std::string(120, 'x')) == "13f05a0b594787f5ecd315edc96141bd3243203d1b7d4f0836f37308b276ba98", "sha256 of 120 bytes");
        check(cling::sha256Hex(std::string(128, 'x')) == "24da1b81d0b16df6428eee73c69fcb2a93c76bc6df706f0c6670fe6bfe800464", "sha256 of 128 bytes");
        check(cling::sha256Hex(std::string(1000, 'x')) == "44f8354494a5ba03ba1792a8d3e9c534c47a9181980fde7a3f44b06ef2ae7c7f", "sha256 of 1000 bytes");
    }

    {   // @header lines: parsed, recorded with the pack's directory, and verified against the files on disk
        FactTable t;
        std::string h(64, 'a');
        check(t.addText(std::string(kHeader) + "@header lib/x.h sha256=" + h + "\n" "function f source=declared" "\n", "dir/pack.clfacts").empty(), "@header accepted");
        check(t.headerBindings().size() == 1 && t.headerBindings()[0].path == "lib/x.h" && t.headerBindings()[0].baseDir == "dir" &&
                  t.headerBindings()[0].sha256 == h, "@header recorded with the pack's directory");
        std::vector<std::string> missing;
        size_t checked = 0;
        check(t.verifyHeaders(0, missing, checked).empty() && checked == 0 && missing.size() == 1, "a header not found is reported as missing, not as a mismatch");
        rejects("@header x.h sha256=short" "\n" "function f source=declared" "\n", "@header");
        rejects("@header x.h" "\n" "function f source=declared" "\n", "@header");
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
            bool scanned = f.kind == "vector.sig" || f.kind == "string.sig" || f.kind == "string_view.sig" || f.kind == "map.sig";
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
        // std::string_view: readers are admitted, substr (a view of the same string) is admitted and known to return a view, data() is sealed
        for (const char* ok : {"size", "length", "empty", "find", "rfind", "compare", "substr", "remove_prefix"}) {
            check(cling::isKnownStringViewMethod(ok), std::string("string_view method admitted by rule: ") + ok);
        }
        for (const char* no : {"data", "begin", "end", "front", "back", "at", "swap", "copy", "frobnicate"}) {
            check(!cling::isKnownStringViewMethod(no), std::string("string_view method refused by rule: ") + no);
        }
        check(cling::stringViewMethodReturnsView("substr") && !cling::stringViewMethodReturnsView("size"), "substr returns a view, size does not");
        check(cling::isStringViewType("string_view") && cling::isStringViewType("std::string_view") && !cling::isStringViewType("string"), "string_view type names");
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
