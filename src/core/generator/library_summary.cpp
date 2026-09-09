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
    static const std::set<std::string> kKnown = {"push_back", "pop_back", "size", "begin", "end"};
    return kKnown.count(methodName) > 0;
}

bool vectorMethodSharesArg(const std::string& methodName, size_t argIndex) {
    // push_back(x) links x into the container (like a field store), the same way
    // `a.b = obj;` does not consume `obj` -- see dangling_checker.cpp's member-
    // target-assignment handling for the precedent this follows.
    return methodName == "push_back" && argIndex == 0;
}

bool vectorMethodInvalidatesReferences(const std::string& methodName) {
    // pop_back doesn't reallocate (capacity is unchanged), but it DOES destroy
    // the popped-back element itself, so any existing reference/observer of
    // that element is dangling afterward -- conservatively treated the same
    // as push_back's whole-container invalidation, since this codebase's
    // model tracks invalidation at the container level, not per-element (see
    // Chapter 18 Step 14's design note on this granularity choice).
    return methodName == "push_back" || methodName == "pop_back";
}

bool isStringType(const std::string& type) {
    return type == "string" || type == "std::string";
}

bool isKnownStringMethod(const std::string& methodName) {
    static const std::set<std::string> kKnown = {
        "length", "size", "empty", "substr", "find", "append", "clear"};
    return kKnown.count(methodName) > 0;
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
        "int", "float", "double", "bool", "string", "std::string", "char"};
    return kKnown.count(type) > 0;
}

bool isCharType(const std::string& type) {
    return type == "char";
}

bool isKnownQualifiedFreeFunction(const std::string& qualifiedName) {
    static const std::set<std::string> kKnown = {
        "std::stoi", "std::stod",
        // imgui-cling Phase 0 feasibility spike (see imgui-cling/CLAUDE.md's
        // "On Third-Party Library Calls" section): a hand-written, UNCHECKED
        // C++ glue shim (imgui-cling/shim/gfx_shim.{h,cpp}) wraps GLFW+OpenGL
        // behind a handful of plain-primitives-only free functions (no
        // pointers, no callbacks, no object types cross the boundary), so no
        // ownership/borrow question ever reaches CeeLing's safety analysis.
        // Despite the name "isKnownQualifiedFreeFunction", the check here is
        // a bare string match on call.function_name regardless of whether a
        // namespace qualifier is present (confirmed by reading
        // EscapeHatchChecker::isKnownCallable's call site) -- these are
        // called bare (`gfx_init(...)`, not `something::gfx_init(...)`), and
        // that already works correctly through this same allow-list.
        "gfx_init", "gfx_should_close", "gfx_begin_frame",
        "gfx_draw_triangle", "gfx_end_frame", "gfx_shutdown",
        // Phase 1: rectangle drawing + mouse input, for the first clickable-
        // widget spike. gfx_get_mouse_pos itself is NOT here -- it still
        // takes float* out-params, so it stays C++-internal only; CeeLing
        // calls the two single-return wrappers instead.
        "gfx_draw_rect", "gfx_mouse_x", "gfx_mouse_y", "gfx_mouse_down",
        // Phase 1: bitmap-font text rendering (see gfx_shim.h -- deliberately
        // not real TTF rendering). Both take/return only primitives
        // (std::string by const&, like gfx_init's title parameter).
        "gfx_draw_text", "gfx_text_width",
        // Phase 3: DragFloat's value-as-text label needs float->string, which
        // CeeLing has no other way to express (std::to_string itself is not
        // on any allow-list -- confirmed via probe: it's rejected as an
        // unknown call). Same plain-primitives-only shape as every other
        // gfx_* entry (float in, std::string out), so it needs no borrow
        // summary either.
        "gfx_float_to_string",
        // EXPLORATORY Eigen spike (see isKnownExternalClassType's doc comment):
        // Eigen::MatrixXd::Identity(rows, cols) is a static factory returning a
        // fresh, non-aliasing matrix -- unlike this allow-list's other entries,
        // its return is NOT a primitive, but it needs no borrow summary for the
        // same reason `operator*` doesn't (see isKnownExternalOperator): the
        // result is always fresh/owned, never a view into anything.
        "Eigen::MatrixXd::Identity"};
    return kKnown.count(qualifiedName) > 0;
}

bool isKnownExternalClassType(const std::string& type) {
    static const std::set<std::string> kKnown = {"Eigen::MatrixXd"};
    return kKnown.count(type) > 0;
}

bool isKnownExternalOperator(const std::string& type, const std::string& operatorStr) {
    static const std::set<std::string> kKnown = {"Eigen::MatrixXd|*"};
    return kKnown.count(type + "|" + operatorStr) > 0;
}

bool isKnownBorrowingMethod(const std::string& className, const std::string& methodName) {
    // Not exhaustive -- extend by consulting Eigen's own Block/Transpose/Ref/
    // Map/Diagonal/Reverse expression-template family (see this function's
    // header doc comment), never by guessing. Each entry here gets REAL
    // dangling tracking (DanglingChecker::resolveBorrowingMethodReceiver),
    // unlike isKnownExternalOperator's set above.
    static const std::set<std::string> kKnown = {
        "Eigen::MatrixXd::block",    "Eigen::MatrixXd::row",
        "Eigen::MatrixXd::col",      "Eigen::MatrixXd::transpose",
        "Eigen::MatrixXd::segment",  "Eigen::MatrixXd::head",
        "Eigen::MatrixXd::tail",     "Eigen::MatrixXd::diagonal"};
    return kKnown.count(className + "::" + methodName) > 0;
}

bool isKnownRuntimeHelper(const std::string& qualifiedName) {
    static const std::set<std::string> kKnown = {"cling::readFileToString", "cling::fileExists"};
    return kKnown.count(qualifiedName) > 0;
}

std::string requiredRuntimeHelperDefinitions(const std::set<std::string>& used) {
    std::string out;
    bool needNamespace = used.count("cling::readFileToString") > 0 ||
                          used.count("cling::fileExists") > 0;
    if (needNamespace) out += "namespace cling {\n";
    if (used.count("cling::fileExists") > 0) {
        out +=
            "inline bool fileExists(const std::string& path) {\n"
            "    std::ifstream fin(path);\n"
            "    return static_cast<bool>(fin);\n"
            "}\n";
    }
    if (used.count("cling::readFileToString") > 0) {
        out +=
            "inline std::string readFileToString(const std::string& path) {\n"
            "    std::ifstream fin(path);\n"
            "    if (!fin) { return std::string(); }\n"
            "    std::stringstream ss;\n"
            "    ss << fin.rdbuf();\n"
            "    return ss.str();\n"
            "}\n";
    }
    if (needNamespace) out += "} // namespace cling\n";
    return out;
}

} // namespace cling
