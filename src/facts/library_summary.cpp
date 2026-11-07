#include <cctype>
#include "library_summary.h"

#include <set>

#include "fact_table.h"

namespace cling {

namespace {
// "std::vector<Node>" or "vector<Node>" -- accept both the qualified and bare form,
// matching how `type` strings are captured verbatim from source text (a user could
// plausibly write either, though every fixture uses the qualified form).
constexpr const char* kQualifiedPrefix = "std::vector<";
constexpr const char* kBarePrefix = "vector<";

constexpr const char* kMapQualifiedPrefix = "std::map<";
constexpr const char* kMapBarePrefix = "map<";

// Strips a prefix/trailing '>' the same way vectorElementType does, returning the
// raw (unsplit) argument list text, e.g. "int, string". Empty if `type` isn't a
// map<...> shape.
std::string mapArgsText(const std::string& type) {
    size_t prefixLen = 0;
    if (type.rfind(kMapQualifiedPrefix, 0) == 0) {
        prefixLen = std::string(kMapQualifiedPrefix).size();
    } else if (type.rfind(kMapBarePrefix, 0) == 0) {
        prefixLen = std::string(kMapBarePrefix).size();
    } else {
        return {};
    }
    if (type.empty() || type.back() != '>') return {};
    return type.substr(prefixLen, type.size() - prefixLen - 1);
}

// Splits "K, V" on the top-level comma (not one nested inside a further '<...>',
// though nested generics as map arguments are rejected downstream by
// isPrimitiveType regardless -- this split just needs to not misparse them into
// garbage key/value strings before that rejection happens), trims surrounding
// whitespace. Returns {"", ""} if there isn't exactly one top-level comma.
std::pair<std::string, std::string> splitMapArgs(const std::string& argsText) {
    int depth = 0;
    size_t commaPos = std::string::npos;
    for (size_t i = 0; i < argsText.size(); ++i) {
        const char c = argsText[i];
        if (c == '<') ++depth;
        else if (c == '>') --depth;
        else if (c == ',' && depth == 0) {
            if (commaPos != std::string::npos) return {};  // more than one top-level comma
            commaPos = i;
        }
    }
    if (commaPos == std::string::npos) return {};

    auto trim = [](std::string s) {
        size_t start = s.find_first_not_of(" \t");
        size_t end = s.find_last_not_of(" \t");
        if (start == std::string::npos) return std::string{};
        return s.substr(start, end - start + 1);
    };
    return {trim(argsText.substr(0, commaPos)), trim(argsText.substr(commaPos + 1))};
}
} // namespace

bool isVectorType(const std::string& type) {
    return !vectorElementType(type).empty();
}

std::string vectorElementType(const std::string& type) {
    size_t prefixLen = 0;
    if (type.rfind(kQualifiedPrefix, 0) == 0) {
        prefixLen = std::string(kQualifiedPrefix).size();
    } else if (type.rfind(kBarePrefix, 0) == 0) {
        prefixLen = std::string(kBarePrefix).size();
    } else {
        return {};
    }
    if (type.empty() || type.back() != '>') return {};
    return type.substr(prefixLen, type.size() - prefixLen - 1);
}

namespace {

// ADMISSION BY RULE (2026-11-02). There is no list of admitted std::vector / std::string / std::map method names. The scan of
// the library's own declarations (facts/std-vector.clfacts, std-string.clfacts, std-map.clfacts) holds one fact per SIGNATURE:
// what it returns, whether it changes the receiver, and the kind of each parameter. A name is admitted when every signature
// CeeLing code could actually call returns a shape the checkers can honour. "Honour" means: after the call, the analysis still
// knows what the result may alias, so admitting it does not open a hole.
//   void, scalar      nothing to alias.
//   string            a fresh std::string (a value type).
//   self-reference    `append` & co return *this; CeeLing binds no observer to a call result, so it is used as a statement
//                     or a temporary.
// Everything else stays out until the checker can follow it, and the reason is reported with the rejected call:
//   raw-pointer       sealed: a raw pointer has nowhere legal to go (data(), c_str()).
//   iterator-view     an iterator dangles when the container grows; iterators are not tracked.
//   element-reference at()/front()/back(): only `v[i]` is tracked as an observer of the container.
//   view              string_view conversion: views are not tracked.
//   value             a type CeeLing has no shape for (std::allocator, a pair, a comparison object).
//   unresolved        the scan could not determine the result.
// A signature is CALLABLE unless a parameter is of a kind CeeLing has no value of: an iterator, an initializer_list, an
// allocator, a pair or map node, a pointer to writable memory (`vector::erase(const_iterator)` is not callable, `map::erase(key)`
// is). Every call that returns an iterator is refused, a string literal is the only `const char *`, and CeeLing has no pairs,
// so such an overload can never be written; leaving it out is exact, and it is what lets `map::erase(key)` and
// `string::insert(pos, text)` in while `erase(iterator)` stays out. A parameter pack (`emplace_back(args...)`) is callable.
// A callable signature with a non-const reference parameter (`swap(other)`, `merge(other)`) is out: CeeLing moves a container
// argument by default, which would empty the other operand.
bool shapeHonoured(const std::string& shape, const std::string& cls = "") {
    // A `view` result (std::string_view::substr) is a view of the SAME owner as the receiver: the checker follows it, but only for a string_view receiver.
    if (shape == "view" && (cls == "string_view" || cls == "span")) return true;
    if (shape == "element-reference" && cls == "span") return true;      // the element is a number: consumed as a copy
    // An optional holds a number, a bool, a char or a string (nothing else is allowed, see isOptionalElementAllowed). A REFERENCE to that element
    // (`value()`) can only be consumed as a value here: CeeLing cannot bind a reference to a primitive or a string, and a call result is not a valid
    // origin for a view. So the reference is a copy by the time anything can see it, and the element type itself (`value_or` returns it by value) is fresh.
    if (cls == "optional" && (shape == "element-reference" || shape == "value")) return true;
    return shape == "void" || shape == "scalar" || shape == "string" || shape == "self-reference";
}

std::string shapeRefusal(const std::string& shape) {
    if (shape == "raw-pointer") return "it returns a raw pointer, and raw pointers have nowhere legal to go";
    if (shape == "iterator-view") return "it returns an iterator, and iterators are not tracked yet, so nothing would stop one from dangling";
    if (shape == "element-reference") return "it returns a reference to an element, and only `v[i]` is tracked as an observer so far";
    if (shape == "view") return "it returns a view (string_view), and views are not tracked yet";
    if (shape == "value") return "it returns a type CeeLing has no shape for";
    return "the scan of the standard library could not determine what it returns";
}

// The parameter kinds of a signature fact, from its subject `name/const|nonconst/kind+kind`.
std::vector<std::string> signatureKinds(const std::string& subject) {
    size_t second = subject.find('/', subject.find('/') + 1);
    std::vector<std::string> kinds;
    if (second == std::string::npos) return kinds;
    std::string rest = subject.substr(second + 1);
    size_t start = 0;
    while (start <= rest.size()) {
        size_t plus = rest.find('+', start);
        std::string k = rest.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
        if (!k.empty() && k != "-") kinds.push_back(k);
        if (plus == std::string::npos) break;
        start = plus + 1;
    }
    return kinds;
}

bool signatureCallable(const std::vector<std::string>& kinds) {
    for (const auto& k : kinds) {
        if (k == "iterator" || k == "initlist" || k == "allocator" || k == "pair" || k == "ptr-w") return false;
    }
    return true;
}

struct MethodVerdict {
    bool admitted = false;
    std::string reason;   // why not, when !admitted
};

MethodVerdict methodVerdict(const std::string& cls, const std::string& name) {
    const FactTable& facts = builtinFacts();
    if (facts.has(cls + ".own", name)) return {true, ""};   // a method CeeLing itself defines (copyWithin)
    std::vector<const FactTable::Fact*> sigs = facts.withPrefix(cls + ".sig", name + "/");
    if (sigs.empty()) return {false, "the standard library has no such member"};
    bool anyCallable = false;
    for (const FactTable::Fact* f : sigs) {
        std::vector<std::string> kinds = signatureKinds(f->subject);
        if (!signatureCallable(kinds)) continue;
        anyCallable = true;
        for (const auto& k : kinds) {
            if (k == "mref") return {false, "an overload takes a non-const reference parameter, and CeeLing would move the argument out"};
        }
        const std::string& results = f->attrs.at("result");
        size_t start = 0;
        while (start <= results.size()) {
            size_t comma = results.find(',', start);
            std::string shape = results.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            if (!shapeHonoured(shape, cls)) return {false, shapeRefusal(shape)};
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    }
    if (!anyCallable) {
        return {false, "every overload needs an argument CeeLing cannot write (an iterator, a pair, an initializer_list, a pointer to writable memory)"};
    }
    return {true, ""};
}

// Does any signature of `name` change the receiver (so every observer of the container dies)? Counted over ALL signatures, callable or not.
bool methodMutates(const std::string& cls, const std::string& name) {
    for (const FactTable::Fact* f : builtinFacts().withPrefix(cls + ".sig", name + "/")) {
        if (f->attrs.at("mutates") == "yes") return true;
    }
    return false;
}

}  // namespace

bool isKnownVectorMethod(const std::string& methodName) {
    return methodVerdict("vector", methodName).admitted;
}

std::string vectorMethodRefusal(const std::string& methodName) {
    return methodVerdict("vector", methodName).reason;
}

std::string stringMethodRefusal(const std::string& methodName) {
    return methodVerdict("string", methodName).reason;
}

bool vectorMethodSharesArg(const std::string& methodName, size_t argIndex) {
    // push_back(x) links x into the container (like a field store), the same way
    // `a.b = obj;` does not consume `obj` -- see dangling_checker.cpp's member-
    // target-assignment handling for the precedent this follows.
    return argIndex == 0 && builtinFacts().hasFlag("vector.note", methodName, "shares-arg0");
}

bool vectorMethodInvalidatesReferences(const std::string& methodName) {
    // pop_back doesn't reallocate (capacity is unchanged), but it DOES destroy
    // the popped-back element itself, so any existing reference/observer of
    // that element is dangling afterward -- conservatively treated the same
    // as push_back's whole-container invalidation, since this codebase's
    // model tracks invalidation at the container level, not per-element (see
    // Chapter 18 Step 14's design note on this granularity choice).
    // Derived, not listed: a method that may change the receiver (a non-const member with no const twin, found by the scan,
    // `mutates=yes` in facts/std-vector.clfacts) invalidates. That is push_back, pop_back, resize, reserve, assign, clear,
    // shrink_to_fit, ... and nothing a name list could forget.
    return methodMutates("vector", methodName);
}

bool stringMethodInvalidatesReferences(const std::string& methodName) {
    return methodMutates("string", methodName);
}

bool isStringType(const std::string& type) {
    return type == "string" || type == "std::string";
}

bool isStringViewType(const std::string& type) {
    return type == "string_view" || type == "std::string_view";
}

bool isKnownStringViewMethod(const std::string& methodName) {
    return methodVerdict("string_view", methodName).admitted;
}

std::string stringViewMethodRefusal(const std::string& methodName) {
    return methodVerdict("string_view", methodName).reason;
}

// Does this method of a string_view return another view of the SAME owner (substr)? Read from the scan, not listed.
bool stringViewMethodReturnsView(const std::string& methodName) {
    for (const FactTable::Fact* f : builtinFacts().withPrefix("string_view.sig", methodName + "/")) {
        const std::string& r = f->attrs.at("result");
        if (r == "view" || r.find("view,") == 0 || r.find(",view") != std::string::npos) return true;
    }
    return false;
}

namespace {
std::string stripStdPrefix(const std::string& type) {
    return type.rfind("std::", 0) == 0 ? type.substr(5) : type;
}
}  // namespace

bool isSpanType(const std::string& type) {
    const std::string t = stripStdPrefix(type);
    return t.size() > 6 && t.rfind("span<", 0) == 0 && t.back() == '>';
}

std::string spanElementType(const std::string& type) {
    if (!isSpanType(type)) return {};
    const std::string t = stripStdPrefix(type);
    std::string elem = t.substr(5, t.size() - 6);
    size_t a = elem.find_first_not_of(" 	"), b = elem.find_last_not_of(" 	");
    elem = a == std::string::npos ? std::string{} : elem.substr(a, b - a + 1);
    if (elem.rfind("const", 0) == 0 && elem.size() > 5 && !(std::isalnum(static_cast<unsigned char>(elem[5])) || elem[5] == '_')) {
        a = elem.find_first_not_of(" 	", 5);
        elem = a == std::string::npos ? std::string{} : elem.substr(a);
    }
    return elem;
}

bool isSpanConstType(const std::string& type) {
    if (!isSpanType(type)) return false;
    const std::string t = stripStdPrefix(type);
    size_t a = t.find_first_not_of(" 	", 5);
    return a != std::string::npos && t.compare(a, 5, "const") == 0 && a + 5 < t.size() &&
           !(std::isalnum(static_cast<unsigned char>(t[a + 5])) || t[a + 5] == '_');
}

bool isSpanElementAllowed(const std::string& elem) {
    return isPrimitiveType(elem) && !isStringType(elem);      // numbers, bool, char, byte, int64
}

bool isViewType(const std::string& type) {
    return isStringViewType(type) || isSpanType(type);
}

bool isKnownSpanMethod(const std::string& methodName) {
    return methodVerdict("span", methodName).admitted;
}

std::string spanMethodRefusal(const std::string& methodName) {
    return methodVerdict("span", methodName).reason;
}

bool spanMethodReturnsView(const std::string& methodName) {
    for (const FactTable::Fact* f : builtinFacts().withPrefix("span.sig", methodName + "/")) {
        const std::string& r = f->attrs.at("result");
        if (r == "view" || r.find("view,") == 0 || r.find(",view") != std::string::npos) return true;
    }
    return false;
}

bool isOptionalType(const std::string& type) {
    const std::string t = stripStdPrefix(type);
    return t.size() > 10 && t.rfind("optional<", 0) == 0 && t.back() == '>';
}

std::string optionalElementType(const std::string& type) {
    if (!isOptionalType(type)) return {};
    const std::string t = stripStdPrefix(type);
    std::string elem = t.substr(9, t.size() - 10);
    size_t a = elem.find_first_not_of(" 	"), b = elem.find_last_not_of(" 	");
    return a == std::string::npos ? std::string{} : elem.substr(a, b - a + 1);
}

bool isOptionalElementAllowed(const std::string& elem) {
    return isPrimitiveType(elem);        // int float double bool char byte int64 string (isPrimitiveType includes string)
}

bool isKnownOptionalMethod(const std::string& methodName) {
    return methodVerdict("optional", methodName).admitted;
}

std::string optionalMethodRefusal(const std::string& methodName) {
    return methodVerdict("optional", methodName).reason;
}

bool isKnownMapMethod(const std::string& methodName) {
    return methodVerdict("map", methodName).admitted;
}

std::string mapMethodRefusal(const std::string& methodName) {
    return methodVerdict("map", methodName).reason;
}

bool mapMethodInvalidatesReferences(const std::string& methodName) {
    // Coarser than the standard (insert does not invalidate references into a map, erase only those of the erased element):
    // any method that changes the map kills its observers. Conservative, and no map observer can be written today (a map holds
    // primitives only, and `ref` to a primitive does not compile), so nothing is lost.
    return methodMutates("map", methodName);
}

bool isKnownStringMethod(const std::string& methodName) {
    return methodVerdict("string", methodName).admitted;
}

bool isOstreamType(const std::string& type) {
    return type == "ostream";
}

bool isMapType(const std::string& type) {
    const auto [key, value] = splitMapArgs(mapArgsText(type));
    return !key.empty() && !value.empty();
}

std::string mapKeyType(const std::string& type) {
    return splitMapArgs(mapArgsText(type)).first;
}

std::string mapValueType(const std::string& type) {
    return splitMapArgs(mapArgsText(type)).second;
}

bool isPrimitiveType(const std::string& type) {
    static const std::set<std::string> kKnown = {
        "int", "float", "double", "bool", "string", "std::string", "char", "byte", "int64"};
    return kKnown.count(type) > 0;
}

bool isPrimitiveConversionName(const std::string& name) {
    static const std::set<std::string> kKnown = {"int", "float", "double", "char", "byte", "int64"};
    return kKnown.count(name) > 0;
}

bool isCharType(const std::string& type) {
    return type == "char";
}

bool isKnownQualifiedFreeFunction(const std::string& qualifiedName) {
    // std::stoi / stod / to_string, the project shims (facts/shims.clfacts) and Eigen's static factory (facts/eigen.clfacts).
    // The check is a bare string match on the called name, whether or not a namespace qualifier is present.
    return builtinFacts().has("function", qualifiedName);
}

bool isKnownExternalClassType(const std::string& type) {
    return builtinFacts().has("class", type) || builtinFacts().has("opaque", type);
}

bool isOpaqueExternalClass(const std::string& type) {
    return builtinFacts().has("opaque", type);
}

bool isConservativeOpaqueCall(const std::string& className, const std::string& methodName) {
    const FactTable& f = builtinFacts();
    if (!f.has("opaque", className)) return false;
    const std::string key = className + "::" + methodName;
    return !f.has("borrow", key) && !f.has("fresh", key);
}

bool isKnownExternalOperator(const std::string& type, const std::string& operatorStr) {
    return builtinFacts().has("operator", type + "|" + operatorStr);
}

bool isKnownBorrowingMethod(const std::string& className, const std::string& methodName) {
    // Not exhaustive -- extend by consulting Eigen's own Block/Transpose/Ref/Map/Diagonal/Reverse expression-template
    // family (see this function's header doc comment), never by guessing. Each entry gets REAL dangling tracking
    // (DanglingChecker::resolveBorrowingMethodReceiver), unlike isKnownExternalOperator's set.
    // An opaque class's unlisted method is assumed to borrow the receiver (see isOpaqueExternalClass).
    return builtinFacts().has("borrow", className + "::" + methodName) || isConservativeOpaqueCall(className, methodName);
}

bool isKnownFreshMethod(const std::string& className, const std::string& methodName) {
    return builtinFacts().has("fresh", className + "::" + methodName);
}

bool isKnownMutatorMethod(const std::string& className, const std::string& methodName) {
    return builtinFacts().has("mutator", className + "::" + methodName);
}

bool callKillsReceiverObservers(const std::string& className, const std::string& methodName) {
    return isConservativeOpaqueCall(className, methodName) || isKnownMutatorMethod(className, methodName);
}

std::vector<std::string> knownExternalClassTypes() {
    std::vector<std::string> out;
    for (const auto& f : builtinFacts().facts()) {
        if (f.kind == "class" || f.kind == "opaque") out.push_back(f.subject);
    }
    return out;
}

bool isKnownRuntimeHelper(const std::string& qualifiedName) {
    // cling::spawn is produced only by closure_desugar's `spawn(closure ...)` rewrite (a hand-written call is
    // rejected there): it is what makes the sendable-capture check unavoidable.
    static const std::set<std::string> kKnown = {"cling::readFileToString", "cling::fileExists",
                                                  "cling::readFileToBytes", "cling::spawn",
                                                  "cling::setJobFailure", "cling::clearJobFailure",
                                                  "cling::setWorkerLimit", "cling::workerLimit", "cling::joinAllWorkers",
                                                  "cling::setExitGraceMs"};
    return kKnown.count(qualifiedName) > 0;
}

bool isChannelType(const std::string& type) {
    return !channelElementType(type).empty();
}

std::string channelElementType(const std::string& type) {
    static const std::string kPrefix = "Channel<";
    if (type.rfind(kPrefix, 0) != 0 || type.empty() || type.back() != '>') return {};
    std::string inner = type.substr(kPrefix.size(), type.size() - kPrefix.size() - 1);
    const size_t first = inner.find_first_not_of(" \t");
    const size_t last = inner.find_last_not_of(" \t");
    if (first == std::string::npos) return {};
    return inner.substr(first, last - first + 1);
}

bool isKnownChannelMethod(const std::string& methodName, size_t argCount) {
    if (methodName == "send" || methodName == "wait") return argCount == 1;
    return (methodName == "take" || methodName == "size" || methodName == "close" || methodName == "isClosed") && argCount == 0;
}

bool isVectorAlgorithm(const std::string& m) {
    return m == "sort" || m == "reverse" || m == "fill" || m == "indexOf" || m == "contains";
}

size_t vectorAlgorithmArity(const std::string& m) {
    return (m == "sort" || m == "reverse") ? 0 : 1;
}

} // namespace cling
