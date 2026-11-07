#include "emit/cpp_generator.h"

#include <cctype>
#include <functional>
#include <iostream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace cling {

namespace {

struct GenerationDiagnostic {
    int line = 0;
    std::string message;
    std::string suggestion;
};

std::string regexReplaceWithCallback(
    const std::string& input,
    const std::regex& pattern,
    const std::function<std::string(const std::smatch&)>& callback) {

    std::string result;
    std::sregex_iterator it(input.begin(), input.end(), pattern);
    std::sregex_iterator end;
    size_t lastPos = 0;

    for (; it != end; ++it) {
        const std::smatch& match = *it;
        result += input.substr(lastPos, match.position() - lastPos);
        result += callback(match);
        lastPos = match.position() + match.length();
    }
    result += input.substr(lastPos);
    return result;
}

std::string trimWhitespace(const std::string& text) {
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

std::string extractLastIdentifier(const std::string& expression) {
    std::string last;
    std::regex idPattern(R"([A-Za-z_][A-Za-z0-9_]*)");
    auto it = std::sregex_iterator(expression.begin(), expression.end(), idPattern);
    auto end = std::sregex_iterator();
    for (; it != end; ++it) {
        last = it->str();
    }
    return last;
}

std::string sanitizeCommentText(const std::string& text) {
    std::string result = text;
    size_t pos = 0;
    while ((pos = result.find("*/", pos)) != std::string::npos) {
        result.replace(pos, 2, "* /");
        pos += 3;
    }
    return result;
}

std::string buildFactoryExpression(const SmartPointerAnalyzer& analyzer,
                                   const std::string& className,
                                   const std::string& targetVariable,
                                   const std::string& args) {
    PointerType ptrType = targetVariable.empty()
        ? PointerType::SHARED
        : analyzer.getPointerType(className, targetVariable);

    return (ptrType == PointerType::UNIQUE)
        ? "std::make_unique<" + className + ">(" + args + ")"
        : "std::make_shared<" + className + ">(" + args + ")";
}

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream input(text);
    std::string line;
    while (std::getline(input, line)) {
        lines.push_back(line);
    }
    if (!text.empty() && text.back() == '\n') {
        lines.push_back("");
    }
    return lines;
}

std::string joinLines(const std::vector<std::string>& lines) {
    std::ostringstream output;
    for (size_t i = 0; i < lines.size(); ++i) {
        output << lines[i];
        if (i + 1 < lines.size()) {
            output << '\n';
        }
    }
    return output.str();
}

bool appearsLater(const std::vector<std::string>& lines, size_t startIndex, const std::string& identifier) {
    const std::regex pattern("\\b" + identifier + "\\b");
    for (size_t i = startIndex + 1; i < lines.size(); ++i) {
        if (std::regex_search(lines[i], pattern)) {
            return true;
        }
    }
    return false;
}

void addGenerationDiagnostic(std::vector<GenerationDiagnostic>& diagnostics,
                             int line, const std::string& message, const std::string& suggestion = "") {
    diagnostics.push_back({line, message, suggestion});
}

std::string buildAnalysisCommentBlock(const SmartPointerAnalyzer& analyzer,
                                      const std::vector<GenerationDiagnostic>& generationDiagnostics) {
    std::ostringstream comment;
    comment << "/*\n";
    comment << sanitizeCommentText(analyzer.buildAnalysisReport());
    for (const auto& diagnostic : generationDiagnostics) {
        comment << "- line " << diagnostic.line << ": " << diagnostic.message;
        if (!diagnostic.suggestion.empty()) {
            comment << " Suggestion: " << diagnostic.suggestion;
        }
        comment << "\n";
    }
    comment << "*/\n";
    return comment.str();
}

std::string optimizeMoveSemantics(std::string source,
                                  const SmartPointerAnalyzer& analyzer,
                                  std::vector<GenerationDiagnostic>& diagnostics) {
    std::vector<std::string> lines = splitLines(source);
    std::map<std::string, std::string> symbolKinds;
    const std::regex smartDeclRegex(R"(\bstd::(unique_ptr|shared_ptr|weak_ptr)<[^>]+>\s+([A-Za-z_][A-Za-z0-9_]*))");

    for (const auto& line : lines) {
        std::smatch match;
        if (std::regex_search(line, match, smartDeclRegex)) {
            symbolKinds[match[2].str()] = match[1].str();
        }
    }

    const std::regex declCopyRegex(
        R"(^(\s*std::(?:unique_ptr|shared_ptr|weak_ptr)<[^>]+>\s+[A-Za-z_][A-Za-z0-9_]*\s*=\s*)([A-Za-z_][A-Za-z0-9_]*)(\s*;\s*(?://.*)?)$)");
    const std::regex assignmentCopyRegex(
        R"(^(\s*[A-Za-z_][A-Za-z0-9_]*(?:\s*(?:->|\.)\s*[A-Za-z_][A-Za-z0-9_]*)*\s*=\s*)([A-Za-z_][A-Za-z0-9_]*)(\s*;\s*(?://.*)?)$)");
    const std::regex returnCopyRegex(R"(^(\s*return\s+)([A-Za-z_][A-Za-z0-9_]*)(\s*;\s*(?://.*)?)$)");

    for (size_t i = 0; i < lines.size(); ++i) {
        std::smatch match;
        std::string line = lines[i];

        if (std::regex_match(line, match, declCopyRegex)) {
            const std::string rhs = match[2].str();
            const auto rhsKindIt = symbolKinds.find(rhs);
            if (rhsKindIt != symbolKinds.end() && rhsKindIt->second != "weak_ptr") {
                if (!appearsLater(lines, i, rhs)) {
                    lines[i] = match[1].str() + "std::move(" + rhs + ")" + match[3].str();
                } else {
                    addGenerationDiagnostic(
                        diagnostics, static_cast<int>(i + 1),
                        "Potentially expensive copy of '" + rhs + "' retained because it is used again later.",
                        "Consume the value earlier or keep the last use so the generator can emit std::move.");
                }
            }
            continue;
        }

        if (std::regex_match(line, match, assignmentCopyRegex)) {
            const std::string lhs = trimWhitespace(match[1].str());
            const std::string rhs = match[2].str();
            const std::string targetName = extractLastIdentifier(lhs);
            const auto rhsKindIt = symbolKinds.find(rhs);
            const auto lhsKindIt = symbolKinds.find(targetName);

            if (rhsKindIt != symbolKinds.end() && rhsKindIt->second != "weak_ptr") {
                const PointerType targetPointerType = analyzer.getPointerTypeForVariable(targetName);
                if (targetPointerType != PointerType::WEAK && !appearsLater(lines, i, rhs)) {
                    lines[i] = match[1].str() + "std::move(" + rhs + ")" + match[3].str();
                } else if (targetPointerType != PointerType::WEAK &&
                           lhsKindIt != symbolKinds.end() && lhsKindIt->second == rhsKindIt->second) {
                    addGenerationDiagnostic(
                        diagnostics, static_cast<int>(i + 1),
                        "Copy of '" + rhs + "' could not be elided because a later use was detected.",
                        "If ownership should transfer here, avoid reusing the variable afterward.");
                }
            }
            continue;
        }

        if (std::regex_match(line, match, returnCopyRegex)) {
            const std::string rhs = match[2].str();
            const auto rhsKindIt = symbolKinds.find(rhs);
            if (rhsKindIt != symbolKinds.end() && rhsKindIt->second != "weak_ptr") {
                if (!appearsLater(lines, i, rhs)) {
                    lines[i] = match[1].str() + "std::move(" + rhs + ")" + match[3].str();
                } else {
                    addGenerationDiagnostic(
                        diagnostics, static_cast<int>(i + 1),
                        "Returning '" + rhs + "' copies a smart pointer that is still used later.",
                        "Make this the last use if you want the generator to emit std::move.");
                }
            }
            continue;
        }

    }

    return joinLines(lines);
}

} // namespace

std::string CppGenerator::generateCpp(const Program& program, const std::string& source) {
    output.str("");
    output.clear();

    std::cerr << "=== PERFORMING TWO-PASS SMART POINTER ANALYSIS ===" << std::endl;
    analyzer.analyzeProgram(program, source);

    classNames.clear();
    for (const auto& stmt : program.statements) {
        if (auto classDecl = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
            classNames.insert(classDecl->name);
        }
    }

    std::string result = source;
    const std::string sourceForHeuristics = source;

    if (!classNames.empty() && result.find("#include <memory>") == std::string::npos) {
        const size_t includePos = result.find("#include");
        if (includePos != std::string::npos) {
            result.insert(includePos, "#include <memory>\n");
        } else {
            result = "#include <memory>\n" + result;
        }
    }
    if (!classNames.empty() && result.find("#include <utility>") == std::string::npos) {
        const size_t includePos = result.find("#include");
        if (includePos != std::string::npos) {
            result.insert(includePos, "#include <utility>\n");
        } else {
            result = "#include <utility>\n" + result;
        }
    }

    for (const std::string& className : classNames) {
        const std::regex pattern("\\b" + className + "\\s+(\\w+);");
        result = regexReplaceWithCallback(result, pattern, [&](const std::smatch& match) {
            const std::string varName = match[1].str();
            std::string smartPtrType = getSmartPointerType(className, varName);
            const std::regex memberCopyReadPattern("=\\s*[A-Za-z_][A-Za-z0-9_]*\\." + varName + "\\b");
            if (std::regex_search(sourceForHeuristics, memberCopyReadPattern)) {
                smartPtrType = "std::shared_ptr";
            }
            return smartPtrType + "<" + className + "> " + varName + ";";
        });
    }

    for (const std::string& className : classNames) {
        const std::regex pattern("\\b" + className + "\\s+(\\w+)\\s*=\\s*new\\s+" + className + "\\s*\\(([^)]*)\\)");
        result = regexReplaceWithCallback(result, pattern, [&](const std::smatch& match) {
            const std::string varName = match[1].str();
            const std::string args = match[2].str();
            const std::string smartPtrType = getSmartPointerType(className, varName);
            const std::string factory = buildFactoryExpression(analyzer, className, varName, args);
            return smartPtrType + "<" + className + "> " + varName + " = " + factory;
        });
    }

    for (const std::string& className : classNames) {
        const std::regex pattern("\\b" + className + "\\s+(\\w+)\\s*=\\s*(?!new\\s)([^;]+);");
        result = regexReplaceWithCallback(result, pattern, [&](const std::smatch& match) {
            const std::string varName = match[1].str();
            const std::string assignment = match[2].str();
            std::string smartPtrType = getSmartPointerType(className, varName);

            const std::string rhs = trimWhitespace(assignment);
            if (std::regex_match(rhs, std::regex("^[A-Za-z_][A-Za-z0-9_]*$"))) {
                smartPtrType = getSmartPointerType(className, rhs);
            }

            return smartPtrType + "<" + className + "> " + varName + " = " + assignment + ";";
        });
    }

    for (const std::string& className : classNames) {
        const std::regex pattern("\\b" + className + "\\s+([A-Za-z_][A-Za-z0-9_]*)\\s*(?=[,)])");
        result = regexReplaceWithCallback(result, pattern, [&](const std::smatch& match) {
            const std::string paramName = match[1].str();
            return "std::shared_ptr<" + className + "> " + paramName;
        });
    }

    for (const std::string& className : classNames) {
        const std::regex pattern("\\b" + className + "\\s+([A-Za-z_][A-Za-z0-9_]*)\\s*\\(");
        result = regexReplaceWithCallback(result, pattern, [&](const std::smatch& match) {
            const std::string funcName = match[1].str();
            return "std::shared_ptr<" + className + "> " + funcName + "(";
        });
    }

    for (const std::string& className : classNames) {
        const std::regex pattern(
            "([A-Za-z_][A-Za-z0-9_]*(?:\\s*(?:->|\\.)\\s*[A-Za-z_][A-Za-z0-9_]*)*)\\s*=\\s*new\\s+"
            + className + "\\s*\\(([^)]*)\\)");
        result = regexReplaceWithCallback(result, pattern, [&](const std::smatch& match) {
            const std::string lhs = trimWhitespace(match[1].str());
            const std::string args = match[2].str();
            const std::string targetVar = extractLastIdentifier(lhs);
            const std::string factory = buildFactoryExpression(analyzer, className, targetVar, args);
            return lhs + " = " + factory;
        });
    }

    result = regexReplaceWithCallback(
        result,
        std::regex(R"(([A-Za-z_][A-Za-z0-9_]*(?:\s*(?:->|\.)\s*[A-Za-z_][A-Za-z0-9_]*)*)\s*=\s*nullptr\s*;)"),
        [&](const std::smatch& match) {
            const std::string assignment = trimWhitespace(match[1].str());
            const std::string targetVar = extractLastIdentifier(assignment);
            if (analyzer.getPointerTypeForVariable(targetVar) == PointerType::WEAK) {
                return assignment + ".reset();";
            }
            return match.str();
        });

    for (const std::string& className : classNames) {
        const std::regex pattern("\\breturn\\s+new\\s+" + className + "\\s*\\(([^)]*)\\)");
        result = regexReplaceWithCallback(result, pattern, [&](const std::smatch& match) {
            const std::string args = match[1].str();
            return "return std::make_shared<" + className + ">(" + args + ")";
        });
    }

    for (const std::string& className : classNames) {
        const std::regex pattern("\\bnew\\s+" + className + "\\s*\\(([^)]*)\\)");
        result = regexReplaceWithCallback(result, pattern, [&](const std::smatch& match) {
            const std::string args = match[1].str();
            return "std::make_shared<" + className + ">(" + args + ")";
        });
    }

    const std::regex memberAccessPattern("([A-Za-z_][A-Za-z0-9_]*)\\.([A-Za-z_][A-Za-z0-9_]*)");
    for (int i = 0; i < 6; ++i) {
        const std::string updated = regexReplaceWithCallback(result, memberAccessPattern, [&](const std::smatch& match) {
            const std::string varName = match[1].str();
            const std::string memberName = match[2].str();
            if (varName == "std" || memberName == "cout" || memberName == "endl" ||
                memberName == "size" || memberName == "length" || memberName == "empty" ||
                memberName == "str" || memberName == "clear" ||
                memberName == "reset" || memberName == "lock" ||
                memberName == "get" || memberName == "expired" ||
                memberName == "use_count") {
                return match.str();
            }
            return varName + "->" + memberName;
        });

        if (updated == result) {
            break;
        }
        result = updated;
    }

    result = regexReplaceWithCallback(
        result,
        std::regex(R"(//\s*@ptr\s+(unique|shared|weak)\s+([A-Za-z_][A-Za-z0-9_]*)->([A-Za-z_][A-Za-z0-9_]*))"),
        [&](const std::smatch& match) {
            return "//@ptr " + match[1].str() + " " + match[2].str() + "." + match[3].str();
        });
    result = regexReplaceWithCallback(
        result,
        std::regex(R"(([A-Za-z_][A-Za-z0-9_]*)->(reset|lock|get|expired|use_count)\s*\()"),
        [&](const std::smatch& match) {
            return match[1].str() + "." + match[2].str() + "(";
        });

    if (std::regex_search(result, std::regex("\\bfn\\b")) || result.find(":=") != std::string::npos) {
        throw std::runtime_error("Unsupported syntax remains after transformation (e.g., 'fn' or ':=').");
    }

    std::vector<GenerationDiagnostic> generationDiagnostics;
    result = optimizeMoveSemantics(result, analyzer, generationDiagnostics);

    const std::string analysisComment = buildAnalysisCommentBlock(analyzer, generationDiagnostics);
    result = analysisComment + result;

    for (const auto& diagnostic : analyzer.getDiagnostics()) {
        std::cerr << "Warning";
        if (diagnostic.line > 0) {
            std::cerr << " [line " << diagnostic.line << "]";
        }
        std::cerr << ": " << diagnostic.message;
        if (!diagnostic.suggestion.empty()) {
            std::cerr << " Suggestion: " << diagnostic.suggestion;
        }
        std::cerr << std::endl;
    }

    for (const auto& diagnostic : generationDiagnostics) {
        std::cerr << "Warning [line " << diagnostic.line << "]: " << diagnostic.message;
        if (!diagnostic.suggestion.empty()) {
            std::cerr << " Suggestion: " << diagnostic.suggestion;
        }
        std::cerr << std::endl;
    }

    return result;
}

std::string CppGenerator::makeFactory(const std::string& ptrType,
                                      const std::string& className,
                                      const std::vector<ExpressionPtr>& args) {
    const std::string factoryName = (ptrType == "std::unique_ptr") ? "std::make_unique" : "std::make_shared";
    std::string result = factoryName + "<" + className + ">(";
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) {
            result += ", ";
        }
        result += args[i] ? args[i]->toString() : "/* null */";
    }
    result += ")";
    return result;
}

bool CppGenerator::isClassName(const std::string& type) const {
    return classNames.find(type) != classNames.end();
}

std::string CppGenerator::getSmartPointerType(const std::string& className, const std::string& variableName) const {
    switch (analyzer.getPointerType(className, variableName)) {
        case PointerType::UNIQUE:
            return "std::unique_ptr";
        case PointerType::WEAK:
            return "std::weak_ptr";
        default:
            return "std::shared_ptr";
    }
}

} // namespace cling
