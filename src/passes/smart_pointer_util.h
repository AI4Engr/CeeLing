#pragma once
// Small helpers shared by the SmartPointerAnalyzer translation units.

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>
#include <string>

#include "passes/smart_pointer_analyzer.h"

namespace cling {

inline constexpr const char* kGlobalScope = "__global__";

inline std::string trim(const std::string& text) {
    size_t start = 0;
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) {
        ++start;
    }

    size_t end = text.size();
    while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }

    return text.substr(start, end - start);
}

inline int countChar(const std::string& text, char needle) {
    return static_cast<int>(std::count(text.begin(), text.end(), needle));
}

inline std::string makeMemberKey(const std::string& className, const std::string& memberName) {
    return className + "." + memberName;
}

inline std::string makeSymbolKey(const std::string& scopeName, const std::string& symbolName) {
    return scopeName + "|" + symbolName;
}

inline std::string pointerTypeName(PointerType type) {
    switch (type) {
        case PointerType::UNIQUE:
            return "std::unique_ptr";
        case PointerType::WEAK:
            return "std::weak_ptr";
        default:
            return "std::shared_ptr";
    }
}

inline bool isBackReferenceName(const std::string& memberName) {
    return memberName == "prev" || memberName == "previous" ||
           memberName == "parent" || memberName == "back" ||
           memberName == "owner";
}

struct ParsedValue {
    bool isNullptr = false;
    bool isNewExpression = false;
    bool isIdentifier = false;
    bool isMemberAccess = false;
    std::string identifier;
    std::string objectName;
    std::string memberName;
    std::string newClassName;
};

inline ParsedValue parseValue(const std::string& rawText) {
    ParsedValue parsed;
    const std::string text = trim(rawText);
    std::smatch match;

    if (text == "nullptr") {
        parsed.isNullptr = true;
        return parsed;
    }

    if (std::regex_match(text, match, std::regex(R"(^new\s+([A-Za-z_][A-Za-z0-9_]*)\s*\()"))) {
        parsed.isNewExpression = true;
        parsed.newClassName = match[1].str();
        return parsed;
    }

    if (std::regex_match(text, match, std::regex(R"(^([A-Za-z_][A-Za-z0-9_]*)$)"))) {
        parsed.isIdentifier = true;
        parsed.identifier = match[1].str();
        return parsed;
    }

    if (std::regex_match(text, match, std::regex(R"(^([A-Za-z_][A-Za-z0-9_]*)\s*(?:\.|->)\s*([A-Za-z_][A-Za-z0-9_]*)$)"))) {
        parsed.isMemberAccess = true;
        parsed.objectName = match[1].str();
        parsed.memberName = match[2].str();
        return parsed;
    }

    return parsed;
}

inline PointerType parsePointerType(const std::string& rawType) {
    const std::string type = trim(rawType);
    if (type == "unique" || type == "unique_ptr") {
        return PointerType::UNIQUE;
    }
    if (type == "weak" || type == "weak_ptr") {
        return PointerType::WEAK;
    }
    return PointerType::SHARED;
}

inline std::string chooseMethodLikeScope(const std::string& currentClass, const std::string& currentFunction) {
    if (!currentClass.empty() && !currentFunction.empty()) {
        return currentClass + "::" + currentFunction;
    }
    if (!currentClass.empty()) {
        return currentClass;
    }
    if (!currentFunction.empty()) {
        return currentFunction;
    }
    return kGlobalScope;
}


} // namespace cling
