// The language version a source file is written for (docs/language-guide, "The language version"). A file may begin with
//
//     //ceeling 0.1
//
// in its header (the comment and blank lines before the first line of code). A file without the line is taken to be written for the current version, so
// existing files need no change. The line is an ordinary comment, so a compiler that does not know about it ignores it.
//
// What the compiler does with it, today: it knows one version (kLanguageMajor.kLanguageMinor) and refuses a file written for a newer one, with a message that says so,
// instead of failing on whatever syntax the newer version added. There is no older version yet to migrate from; when there is, an old file gets a message that names the
// change and, for a change that can be rewritten mechanically, a rewrite (see TODO.md, "Language versions").
#pragma once
#include <stdexcept>
#include <string>
#include <vector>

namespace cling {

// What checkLanguageVersion throws. The message is already complete (code, file, line, help), so the driver prints it as it is.
struct LanguageVersionError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

constexpr int kLanguageMajor = 0;
constexpr int kLanguageMinor = 1;

// "0.1"
std::string languageVersionText();

// Looks at the header of `source` (read from `file`, which is only used in the messages). Throws LanguageVersionError with a message in the compiler's usual form
// ("error[version/...]: ... --> file:line:col, help: ...") when the line is malformed, repeated, written for a newer version than this compiler knows, for a version that
// does not exist, or placed after code. Returns normally for a file without the line and for one written for the current version.
void checkLanguageVersion(const std::string& source, const std::string& file);

}  // namespace cling
