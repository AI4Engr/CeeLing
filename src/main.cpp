#include "core/cst_to_ast/ast.h"
#include "core/generator/cpp_generator.h"
#include "core/generator/dangling_checker.h"
#include "core/generator/escape_hatch_checker.h"
#include "core/generator/ast_emitter.h"
#include "core/generator/smart_pointer_analyzer.h"
#include <iostream>
#include <vector>
#include <fstream>
#include <sstream>
#include <string>
#include <cctype>
#include <set>

#ifdef WITH_TREE_SITTER
#include "core/cst_to_ast/ast_builder.h"
#endif

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#else
#include <unistd.h>
#endif

using namespace cling;

std::string readFile(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        throw std::runtime_error("Could not open file: " + filename);
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

std::string dirnameOf(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? "" : path.substr(0, slash + 1);
}

// `#include "yaml_node.clh"` extracts the quoted target ("yaml_node.clh"). Returns ""
// for `#include <...>` (angle-bracket, real system header) or a malformed line.
std::string quotedIncludeTarget(const std::string& includeText) {
    size_t open = includeText.find('"');
    if (open == std::string::npos) return "";
    size_t close = includeText.find('"', open + 1);
    if (close == std::string::npos) return "";
    return includeText.substr(open + 1, close - open - 1);
}

#ifdef WITH_TREE_SITTER
// CeeLing's multi-file story: `#include "foo.clh"` in one .clg makes foo's classes/
// functions/enums usable there. `.clh` is a SOURCE-level, CeeLing-only spelling (no
// such file needs to exist) that names foo's declarations the way `.clg` names foo's
// definitions; resolved by finding foo's OWN .clg source (mergeExternalDeclarations
// parses that, not any .clh file, which does not exist as CeeLing input) and merging
// its top-level declarations into `program` marked isExternalDeclaration -- visible
// to every semantic pass (so `Node`/`Scanner`/etc. are no longer "unknown calls"),
// but never re-emitted by AstEmitter (the real definitions already live in foo's own
// separately-generated .cpp; see Statement::isExternalDeclaration). This is genuine
// separate compilation -- foo.clg's SOURCE is never copied into the including file's
// own generated .cpp -- not a text merge of the two .clg files. The emitted C++
// output rewrites `#include "foo.clh"` to `#include "foo.h"` (see rewriteClhIncludes)
// since `.h` is the real, on-disk generated header g++ needs to find.
// Recurses so a chain of #includes (parser -> node, parser -> scanner) resolves
// transitively; `seenClgFiles` prevents infinite recursion / duplicate merges on a
// diamond include.
void mergeExternalDeclarations(Program& program, const std::string& baseDir,
                                std::set<std::string>& seenClgFiles) {
    std::vector<StatementPtr> merged;
    for (const auto& stmt : program.statements) {
        auto inc = dynamic_cast<const IncludeStatement*>(stmt.get());
        if (inc == nullptr) continue;
        std::string target = quotedIncludeTarget(inc->includeFile);
        if (target.size() < 4 || target.compare(target.size() - 4, 4, ".clh") != 0) {
            continue; // angle-bracket or non-.clh quoted include -- real C++ header
        }
        std::string clgTarget = target.substr(0, target.size() - 4) + ".clg";
        std::string resolvedPath = baseDir + clgTarget;
        std::ifstream test(resolvedPath);
        if (!test.is_open()) {
            throw std::runtime_error("Could not resolve #include \"" + target +
                                     "\": no sibling source " + resolvedPath +
                                     " (a .clh include must have a .clg of the same name next to it)");
        }
        test.close();
        if (!seenClgFiles.insert(resolvedPath).second) {
            continue; // already merged (diamond include)
        }

        std::string includedSource = readFile(resolvedPath);
        ASTBuilder includedBuilder;
        std::unique_ptr<Program> includedProgram = includedBuilder.buildProgram(includedSource);
        std::string includedDir = dirnameOf(resolvedPath);
        // Resolve the included file's OWN includes first, so a transitive dependency
        // (parser -> node, parser -> scanner) is visible by the time we merge parser's.
        mergeExternalDeclarations(*includedProgram, includedDir, seenClgFiles);
        for (auto& includedStmt : includedProgram->statements) {
            includedStmt->isExternalDeclaration = true;
            merged.push_back(std::move(includedStmt));
        }
    }
    if (!merged.empty()) {
        merged.insert(merged.end(), std::make_move_iterator(program.statements.begin()),
                      std::make_move_iterator(program.statements.end()));
        program.statements = std::move(merged);
    }
}
#endif

// `.clh` is a source-level-only spelling (see mergeExternalDeclarations); the actual
// generated header on disk is named `.h`. Rewrite every `#include "foo.clh"` in the
// generated C++ to `#include "foo.h"` so the emitted #include lines resolve to real
// files g++ can find, without requiring the .clg author to know the on-disk name.
std::string rewriteClhIncludes(const std::string& cppCode) {
    const std::string from = ".clh\"";
    const std::string to = ".h\"";
    std::string result;
    result.reserve(cppCode.size());
    size_t pos = 0;
    while (true) {
        size_t next = cppCode.find(from, pos);
        if (next == std::string::npos) {
            result.append(cppCode, pos, std::string::npos);
            break;
        }
        result.append(cppCode, pos, next - pos);
        result += to;
        pos = next + from.size();
    }
    return result;
}

void showUsage(const char* programName) {
    std::cout << "Usage: " << programName << " [options] <file.clg>" << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  --cpp <output>  - Compile to C++ and save to <output> file" << std::endl;
    std::cout << "  --legacy-cpp    - Use the legacy regex/text-substitution generator" << std::endl;
    std::cout << "                    instead of the default AST-driven generator" << std::endl;
    std::cout << "  --help         - Show this help message" << std::endl;
    std::cout << "  <file.clg>      - CeeLing source file to process" << std::endl;
    std::cout << std::endl;
    std::cout << "Note: CeeLing only supports C++ compilation. Use --cpp option." << std::endl;
}

bool isRunningInNewWindow() {
#ifdef _WIN32
    // On Windows, check if we're running in a console that was created for this process
    // This is a heuristic - if stdin is not a console, we're likely in a script/pipe
    return _isatty(_fileno(stdin)) && _isatty(_fileno(stdout));
#else
    // On Unix-like systems, check if we have a controlling terminal
    return isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
#endif
}

void pauseIfNeeded() {
    // Only pause if we're running in what appears to be a new window
    // and not being called from a script
    if (isRunningInNewWindow()) {
        std::cout << "\nPress Enter to continue..." << std::endl;
        std::cin.get();
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        showUsage(argv[0]);
        pauseIfNeeded();
        return 1;
    }

    std::string inputFile;
    std::string outputFile;
    bool compileToCpp = false;
    // AstEmitter (the AST-driven generator) is the default: it can only emit node
    // types it recognizes, so a construct CeeLing cannot express structurally cannot
    // appear in its output -- the property Phase 1 (escape-hatch sealing) depends on.
    // The legacy regex/text-substitution CppGenerator remains available via
    // --legacy-cpp for comparison/debugging, but does not offer that guarantee (it
    // builds output from the raw source text, not from the validated AST).
    bool useLegacyGenerator = false;

    // Parse command line arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--emit-ast") {
            // AstEmitter is now the default; kept as a harmless accepted alias.
        } else if (arg == "--legacy-cpp") {
            useLegacyGenerator = true;
        } else if (arg == "--cpp") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --cpp requires an output filename" << std::endl;
                showUsage(argv[0]);
                pauseIfNeeded();
                return 1;
            }
            compileToCpp = true;
            outputFile = argv[++i];
        } else if (arg == "--help") {
            showUsage(argv[0]);
            return 0;
        } else if (arg[0] == '-') {
            std::cerr << "Error: Unknown option " << arg << std::endl;
            showUsage(argv[0]);
            pauseIfNeeded();
            return 1;
        } else {
            if (inputFile.empty()) {
                inputFile = arg;
            } else {
                std::cerr << "Error: Multiple input files specified" << std::endl;
                showUsage(argv[0]);
                pauseIfNeeded();
                return 1;
            }
        }
    }

    if (inputFile.empty()) {
        std::cerr << "Error: No input file specified" << std::endl;
        showUsage(argv[0]);
        pauseIfNeeded();
        return 1;
    }

    if (!compileToCpp) {
        std::cerr << "Error: CLing only supports C++ compilation. Use --cpp option." << std::endl;
        showUsage(argv[0]);
        pauseIfNeeded();
        return 1;
    }

    try {
        // Read CLing source file
        std::string source = readFile(inputFile);

        std::unique_ptr<Program> program;

#ifdef WITH_TREE_SITTER
        // Use Tree-sitter parser to build AST from source
        ASTBuilder builder;
        program = builder.buildProgram(source);

        // Resolve `#include "*.clh"` targets against their sibling .clg source so
        // this file's semantic analysis sees the included classes/functions as
        // known (see mergeExternalDeclarations). Codegen is unaffected: those
        // merged declarations are marked external and skipped by AstEmitter.
        std::set<std::string> seenClgFiles{inputFile};
        mergeExternalDeclarations(*program, dirnameOf(inputFile), seenClgFiles);
#else
        throw std::runtime_error("This build was configured without Tree-sitter support. Rebuild with -DWITH_TREE_SITTER=ON.");
#endif

        // Phase 1: seal escape hatches (e.g. `.get()` on a CeeLing object) before any
        // lifetime analysis -- a leaky boundary makes dangling analysis meaningless.
        EscapeHatchChecker escapeHatchChecker;
        std::vector<EscapeHatchError> escapeHatchErrors = escapeHatchChecker.check(*program);
        if (!escapeHatchErrors.empty()) {
            for (const auto& err : escapeHatchErrors) {
                std::cerr << "error[semantic/forbidden-construct]: " << err.message << "\n";
                if (err.line > 0) {
                    std::cerr << "  --> " << inputFile << ":" << err.line << ":" << err.column << "\n";
                }
                if (!err.help.empty()) {
                    std::cerr << "  help: " << err.help << "\n";
                }
            }
            pauseIfNeeded();
            return 1;
        }

        // SmartPointerAnalyzer runs before the dangling checker now: Phase 2.6
        // container support needs it there too (DanglingChecker consults
        // containerElementIsShared to know whether a specific push_back call
        // shares or moves its argument -- see Chapter 18). Cheap and side-effect
        // free, so running it unconditionally (even under --legacy-cpp, which
        // doesn't otherwise need it) is simpler than threading a conditional.
        SmartPointerAnalyzer analyzer;
        analyzer.analyzeProgram(*program, source);

        // Memory-safety analysis: reject dangling / use-after-move before generating C++.
        DanglingChecker danglingChecker;
        std::vector<DanglingError> danglingErrors = danglingChecker.check(*program, analyzer);
        if (!danglingErrors.empty()) {
            for (const auto& err : danglingErrors) {
                std::cerr << "error[semantic/use-after-move]: " << err.message << "\n";
                if (err.line > 0) {
                    std::cerr << "  --> " << inputFile << ":" << err.line << ":" << err.column << "\n";
                }
                if (!err.help.empty()) {
                    std::cerr << "  help: " << err.help << "\n";
                }
            }
            pauseIfNeeded();
            return 1;
        }

        // Generate C++ code. Output extension `.h` requests a declaration-only header
        // (class/method/function SIGNATURES, no bodies -- except templates, which
        // must stay fully defined at every instantiation point) instead of a full
        // translation unit; any other extension (.cpp, .cc, ...) emits the ordinary
        // full definition. This is what lets one .clg `#include "foo.clh"` another's
        // generated foo.h and reference its types across a real, separately compiled
        // translation unit -- not a text merge of the two .clg sources.
        std::string cppCode;
        bool wantsHeader = outputFile.size() >= 2 &&
            outputFile.compare(outputFile.size() - 2, 2, ".h") == 0;
        if (useLegacyGenerator) {
            CppGenerator generator;
            cppCode = generator.generateCpp(*program, source);
        } else if (wantsHeader) {
            AstEmitter emitter(analyzer);
            size_t slash = outputFile.find_last_of("/\\");
            std::string guardName = (slash == std::string::npos) ? outputFile : outputFile.substr(slash + 1);
            for (char& c : guardName) {
                c = (std::isalnum(static_cast<unsigned char>(c)) ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : '_');
            }
            cppCode = emitter.emitHeader(*program, "CEELING_GEN_" + guardName);
        } else {
            // AST-driven generator (default): walks the validated Program, so an
            // unsupported/forbidden construct cannot appear in the output.
            AstEmitter emitter(analyzer);
            cppCode = emitter.emit(*program);
        }
        cppCode = rewriteClhIncludes(cppCode);

        // Write to output file
        std::ofstream outFile(outputFile);
        if (!outFile.is_open()) {
            throw std::runtime_error("Could not create output file: " + outputFile);
        }
        outFile << cppCode;
        outFile.close();

        std::cout << "C++ code generated successfully: " << outputFile << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        pauseIfNeeded();
        return 1;
    }

    return 0;
}
