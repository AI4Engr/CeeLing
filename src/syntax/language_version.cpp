#include "language_version.h"

#include <cctype>
#include <stdexcept>
#include <vector>

namespace cling {

namespace {

struct Line {
    std::string text;      // without the line end
    int number;            // 1-based
};

std::string trimmed(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}


bool versionLine(const std::string& line, std::string& rest, int& column) {
    const std::string t = trimmed(line);
    if (t.compare(0, 2, "//") != 0) return false;
    const size_t i = 2;                                  // no space between the slashes and the word, as in the `//@` lines of the test fixtures: a comment that
    static const std::string word = "ceeling";           // merely starts with the word ("// ceeling checks ...") is prose and is never an error
    if (t.compare(i, word.size(), word) != 0) return false;
    const size_t after = i + word.size();
    if (after < t.size() && t[after] != ' ' && t[after] != '\t') return false;      // "ceelings", "ceeling-x": prose, not the line
    size_t r = after;
    while (r < t.size() && (t[r] == ' ' || t[r] == '\t')) ++r;
    rest = t.substr(r);
    column = static_cast<int>(line.find("//")) + 1;
    return true;
}

std::string message(const std::string& code, const std::string& what, const std::string& file, int line, int column, const std::string& help) {
    return "error[version/" + code + "]: " + what + "\n  --> " + file + ":" + std::to_string(line) + ":" + std::to_string(column) + "\n  help: " + help;
}

// "0.1" -> 0, 1. Digits, a dot, digits; no sign, no leading zero in a part longer than one digit ("0.01" is not another way to write 0.1).
bool parseVersion(const std::string& s, int& major, int& minor) {
    const size_t dot = s.find('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= s.size()) return false;
    const std::string a = s.substr(0, dot), b = s.substr(dot + 1);
    for (const std::string* part : {&a, &b}) {
        if (part->size() > 4) return false;
        if (part->size() > 1 && (*part)[0] == '0') return false;
        for (char c : *part) if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    major = std::stoi(a);
    minor = std::stoi(b);
    return true;
}

}  // namespace

std::string languageVersionText() { return std::to_string(kLanguageMajor) + "." + std::to_string(kLanguageMinor); }

void checkLanguageVersion(const std::string& source, const std::string& file) {
    std::vector<Line> lines;
    {
        size_t start = 0;
        int number = 1;
        while (start <= source.size()) {
            size_t end = source.find('\n', start);
            if (end == std::string::npos) end = source.size();
            lines.push_back(Line{source.substr(start, end - start), number++});
            start = end + 1;
        }
    }
    bool inHeader = true;
    bool seen = false;
    for (const Line& l : lines) {
        const std::string t = trimmed(l.text);
        std::string rest;
        int column = 1;
        if (versionLine(l.text, rest, column)) {
            if (!inHeader) {
                throw LanguageVersionError(message("misplaced", "the language version line comes after code", file, l.number, column,
                                                 "move `//ceeling " + languageVersionText() + "` up to the header: the comment and blank lines at the top of the file"));
            }
            if (seen) {
                throw LanguageVersionError(message("repeated", "the language version is given twice", file, l.number, column, "keep one `//ceeling` line"));
            }
            seen = true;
            int major = 0, minor = 0;
            if (rest.empty() || !parseVersion(rest, major, minor)) {
                throw LanguageVersionError(message("malformed", "`" + rest + "` is not a language version", file, l.number, column,
                                                 "write it as `//ceeling " + languageVersionText() + "`: two numbers with a dot, no leading zero (this compiler implements " + languageVersionText() + ")"));
            }
            if (major > kLanguageMajor || (major == kLanguageMajor && minor > kLanguageMinor)) {
                throw LanguageVersionError(message("newer", "this file is written for CeeLing " + rest + ", and this compiler implements " + languageVersionText(), file, l.number, column,
                                                 "use a compiler that implements " + rest + ", or write the file for " + languageVersionText()));
            }
            if (major < kLanguageMajor || (major == kLanguageMajor && minor < kLanguageMinor)) {
                throw LanguageVersionError(message("unknown", "CeeLing " + rest + " is not a version of the language", file, l.number, column,
                                                 "the first version is " + languageVersionText() + "; write `//ceeling " + languageVersionText() + "` or leave the line out"));
            }
            continue;
        }
        if (!t.empty() && t.compare(0, 2, "//") != 0) inHeader = false;
    }
}

}  // namespace cling
