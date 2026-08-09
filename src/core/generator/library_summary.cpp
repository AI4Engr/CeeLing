#include "library_summary.h"

#include <set>

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

bool isKnownVectorMethod(const std::string& methodName) {
    static const std::set<std::string> kKnown = {"push_back", "size", "begin", "end"};
    return kKnown.count(methodName) > 0;
}

bool vectorMethodSharesArg(const std::string& methodName, size_t argIndex) {
    // push_back(x) links x into the container (like a field store), the same way
    // `a.b = obj;` does not consume `obj` -- see dangling_checker.cpp's member-
    // target-assignment handling for the precedent this follows.
    return methodName == "push_back" && argIndex == 0;
}

bool vectorMethodInvalidatesReferences(const std::string& methodName) {
    return methodName == "push_back";
}

bool isStringType(const std::string& type) {
    return type == "string" || type == "std::string";
}

bool isKnownStringMethod(const std::string& methodName) {
    static const std::set<std::string> kKnown = {
        "length", "size", "empty", "substr", "find", "append", "clear"};
    return kKnown.count(methodName) > 0;
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
        "int", "float", "double", "bool", "string", "std::string"};
    return kKnown.count(type) > 0;
}

bool isKnownQualifiedFreeFunction(const std::string& qualifiedName) {
    static const std::set<std::string> kKnown = {"std::stoi", "std::stod"};
    return kKnown.count(qualifiedName) > 0;
}

} // namespace cling
