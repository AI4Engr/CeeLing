#include <cctype>
#include "library_summary.h"

#include <set>

#include "fact_table.h"

namespace cling {

namespace {
// Both the qualified and the bare spelling are accepted.
constexpr const char* kQualifiedPrefix = "std::vector<";
constexpr const char* kBarePrefix = "vector<";

constexpr const char* kMapQualifiedPrefix = "std::map<";
constexpr const char* kMapBarePrefix = "map<";

// The raw argument list text of a map<...> type, e.g. "int, string"; empty if `type` is not one.
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

// Splits "K, V" on the top-level comma and trims whitespace. Returns {"", ""} unless there is exactly one.
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

// Admission by rule: there is no list of admitted method names. The facts (facts/std-*.clfacts) hold one entry
// per signature: its result shape, whether it changes the receiver, and the kind of each parameter. A name is
// admitted when every callable signature returns a shape the checkers can follow:
//   void, scalar      nothing to alias.
//   string            a fresh value.
//   self-reference    `append` and friends return *this; used as a statement or temporary.
// Refused, with the reason reported (see shapeRefusal): raw-pointer, iterator-view, element-reference, view, value, unresolved.
// A signature is callable unless a parameter is of a kind CeeLing cannot write (iterator, initializer_list,
// allocator, pair, pointer to writable memory); that is why `map::erase(key)` is in and `erase(iterator)` is out.
// A callable signature with a non-const reference parameter (`swap(other)`) is out: the argument would be moved.
bool shapeHonoured(const std::string& shape, const std::string& cls = "") {
    // A `view` result (substr) is a view of the same owner as the receiver; the checker follows it for a string_view or span receiver.
    if (shape == "view" && (cls == "string_view" || cls == "span")) return true;
    if (shape == "element-reference" && cls == "span") return true;      // the element is a number: consumed as a copy
    // An optional holds a primitive or a string, and a reference to it can only be consumed as a copy.
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

// Does any signature of `name` change the receiver (so every observer of the container dies)? Counts all signatures, callable or not.
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
    // push_back(x) links x into the container like a field store; it does not consume x.
    return argIndex == 0 && builtinFacts().hasFlag("vector.note", methodName, "shares-arg0");
}

bool vectorMethodInvalidatesReferences(const std::string& methodName) {
    // Derived, not listed: a method that may change the receiver (`mutates=yes` in facts/std-vector.clfacts)
    // invalidates. Invalidation is tracked per container, not per element.
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

// Does this method of a string_view return another view of the same owner (substr)? Read from the facts.
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
    // Coarser than the standard: any method that changes the map kills its observers. No map observer can be
    // written today (a map holds primitives only), so nothing is lost.
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
    // A plain string match on the called name.
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
    // Each entry gets dangling tracking (DanglingChecker::resolveBorrowingMethodReceiver).
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
    // cling::spawn is produced only by closure_desugar's `spawn(closure ...)` rewrite; a hand-written call is rejected.
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
