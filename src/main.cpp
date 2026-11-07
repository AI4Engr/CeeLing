#include "syntax/ast.h"
#include "emit/cpp_generator.h"
#include "passes/dangling_checker.h"
#include "passes/escape_hatch_checker.h"
#include "passes/pairing_checker.h"
#include "passes/closure_desugar.h"
#include "passes/struct_json.h"
#include "passes/channel_check.h"
#include "emit/ast_emitter.h"
#include "passes/smart_pointer_analyzer.h"
#include "facts/fact_table.h"
#include "facts/library_summary.h"
#include <iostream>
#include <vector>
#include <fstream>
#include <sstream>
#include <string>
#include <cctype>
#include <set>

#ifdef WITH_TREE_SITTER
#include "syntax/ast_builder.h"
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

// Reads a project-local `.ceeling_pairs` file, so PairingChecker's Begin/End-style pairs come from the
// project and not from the compiler. One `beginName endName` pair per line; `#` comments and blank lines
// are skipped. A missing file means no pairs.
std::vector<std::pair<std::string, std::string>> readPairingDeclarations(const std::string& path) {
    std::vector<std::pair<std::string, std::string>> pairs;
    std::ifstream file(path);
    if (!file.is_open()) return pairs;  // absent file -- not an error, just no pairs

    std::string line;
    while (std::getline(file, line)) {
        size_t start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos || line[start] == '#') continue;
        std::istringstream iss(line);
        std::string beginName, endName;
        if (iss >> beginName >> endName) {
            pairs.emplace_back(beginName, endName);
        }
    }
    return pairs;
}

// `#include "yaml_node.clh"` gives "yaml_node.clh". Returns "" for `#include <...>` or a malformed line.
std::string quotedIncludeTarget(const std::string& includeText) {
    size_t open = includeText.find('"');
    if (open == std::string::npos) return "";
    size_t close = includeText.find('"', open + 1);
    if (close == std::string::npos) return "";
    return includeText.substr(open + 1, close - open - 1);
}

#ifdef WITH_TREE_SITTER
// `#include "foo.clh"` makes the classes, functions and enums of the sibling foo.clg usable. The .clg is
// parsed and its top-level declarations are merged into `program` marked isExternalDeclaration: every
// semantic pass sees them, but the emitter does not emit them again (they live in foo's own generated .cpp).
// Includes are followed transitively; `seenClgFiles` stops cycles and duplicate merges.
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
        for (auto& includedStmt : includedProgram->statements) includedStmt->originFile = resolvedPath;
        std::string includedDir = dirnameOf(resolvedPath);
        // Resolve the included file's own includes first.
        mergeExternalDeclarations(*includedProgram, includedDir, seenClgFiles);
        // Generate the `decodeX(item)` decoders of the included file's structs too, so the includer can call them.
        // Here they are declarations only; the definition is emitted by the file that declares the struct.
        desugarStructJson(*includedProgram, true);
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

// `.clh` exists only in source; the generated header on disk is `.h`. Rewrites `#include "foo.clh"` to `#include "foo.h"`.
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
    std::cout << "  --no-bounds-checks - Do not insert runtime bounds checks on v[i] / s[i]" << std::endl;
    std::cout << "  --no-null-checks   - Do not insert runtime null checks on object-handle dereferences" << std::endl;
    std::cout << "  --ptr-demands-out <file> - Write the pointer kinds this file needs from the classes it includes (separate compilation; see smart_pointer_analyzer.h)" << std::endl;
    std::cout << "  --ptr-demands <file>     - Read such demands written by other files (repeatable)" << std::endl;
    std::cout << "  --facts <file>     - Add a project fact pack (functions, classes, operators, borrowing and fresh methods of a C++ library; repeatable). It can add, never replace, what the compiler knows" << std::endl;
    std::cout << "  --strict[=probed]  - Refuse to compile if the program relied on a fact of a --facts pack that nobody vouches for: accepted are `declared` facts (written by a person) and facts with review=reviewed:<who>:<date>; =probed also accepts review=probed:<date> (set by tools/fact-gauge/probe_pack.py --mark-probed). `opaque` is always accepted (it only makes checking stricter)" << std::endl;
    std::cout << "  --strict-all       - Like --strict, but also for the facts shipped inside the compiler (an audit: the scanned std facts are inferred/unreviewed, so this fails today)" << std::endl;
    std::cout << "  --strict-headers   - Refuse a fact pack whose scanned headers cannot be found to check (a header that IS found must always match its recorded sha256)" << std::endl;
    std::cout << "  --report-facts     - After a successful compile, list every fact the program relied on and where it came from (the trusted-boundary report)" << std::endl;
    std::cout << "  -g, --debug-lines  - Mark every generated statement with its .clg line (#line), so g++ -g and gdb show and step through the .clg source" << std::endl;
    std::cout << "  --pool-shared      - Allocate shared objects from a per-thread free list (faster node-heavy code)" << std::endl;
    std::cout << "  --legacy-cpp    - Use the legacy regex/text-substitution generator" << std::endl;
    std::cout << "                    instead of the default AST-driven generator" << std::endl;
    std::cout << "  --help         - Show this help message" << std::endl;
    std::cout << "  <file.clg>      - CeeLing source file to process" << std::endl;
    std::cout << std::endl;
    std::cout << "Note: CeeLing only supports C++ compilation. Use --cpp option." << std::endl;
}

bool isRunningInNewWindow() {
#ifdef _WIN32
    // Heuristic: if stdin or stdout is not a console, this is probably a script or pipe.
    return _isatty(_fileno(stdin)) && _isatty(_fileno(stdout));
#else
    return isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
#endif
}

void pauseIfNeeded() {
    // Pause only in a new console window, not when called from a script.
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
    // AstEmitter is the default generator: it emits only node types it recognizes, so a forbidden construct
    // cannot appear in its output. The text-substitution CppGenerator (--legacy-cpp) is kept for comparison
    // and does not give that guarantee.
    bool useLegacyGenerator = false;
    // Runtime checks are on by default; --no-bounds-checks and --no-null-checks turn them off.
    bool boundsChecks = true;
    bool nullChecks = true;
    bool poolShared = false;
    bool debugLines = false;                          // -g: #line directives (see AstEmitter::setDebugLines)
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--strict-headers") cling::setStrictHeaders(true);   // before any --facts is read, whatever the order
    }
    bool strictFacts = false, strictAcceptsProbed = false, strictIncludesBuiltin = false;   // --strict[=probed] / --strict-all
    bool reportFacts = false;                         // --report-facts: print the facts the program relied on
    std::string demandsOutFile;                       // --ptr-demands-out: write what this file needs from the classes it includes
    std::vector<std::string> demandsInFiles;          // --ptr-demands: read what other files need (see SmartPointerAnalyzer::setDemands)

    // Parse command line arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--emit-ast") {
            // AstEmitter is the default; the flag is accepted and ignored.
        } else if (arg == "--no-bounds-checks") {
            boundsChecks = false;
        } else if (arg == "--no-null-checks") {
            nullChecks = false;
        } else if (arg == "--pool-shared") {
            poolShared = true;
        } else if (arg == "-g" || arg == "--debug-lines") {
            debugLines = true;
        } else if (arg == "--ptr-demands-out" || arg == "--ptr-demands") {
            if (i + 1 >= argc) {
                std::cerr << "Error: " << arg << " requires a filename" << std::endl;
                return 1;
            }
            if (arg == "--ptr-demands-out") demandsOutFile = argv[++i];
            else demandsInFiles.push_back(argv[++i]);
        } else if (arg == "--facts") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --facts requires a filename" << std::endl;
                return 1;
            }
            std::string err = cling::addProjectFacts(argv[++i]);
            if (!err.empty()) {
                std::cerr << "Error: --facts: " << err << std::endl;
                return 2;
            }
        } else if (arg == "--strict" || arg == "--strict=probed" || arg == "--strict-all") {
            strictFacts = true;
            strictAcceptsProbed = (arg == "--strict=probed") || (arg == "--strict-all" && strictAcceptsProbed);
            strictIncludesBuiltin = strictIncludesBuiltin || arg == "--strict-all";
        } else if (arg == "--strict-headers") {
            // already read by the pre-scan above
        } else if (arg == "--report-facts") {
            reportFacts = true;
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
        std::string source = readFile(inputFile);

        std::unique_ptr<Program> program;

#ifdef WITH_TREE_SITTER
        ASTBuilder builder;
        program = builder.buildProgram(source);

        // Merge the declarations of `#include "*.clh"` files (see mergeExternalDeclarations).
        std::set<std::string> seenClgFiles{inputFile};
        for (auto& stmt : program->statements) stmt->originFile = inputFile;
        mergeExternalDeclarations(*program, dirnameOf(inputFile), seenClgFiles);

        // Resolve method specializations whose target class is declared in an included file; this can only
        // happen after the merge.
        ASTBuilder::spliceExternalSpecializations(*program);

        // `decodeX(item)` for a struct X: generate the decoder (struct_json.h) so the checkers below see ordinary code
        desugarStructJson(*program);
        checkChannels(*program);      // Channel<T>: the element must be a plain value, and nothing may hide a Channel in another type
        // Turn closures into ordinary classes before any checker runs, since EscapeHatchChecker rejects
        // a raw closure expression as unknown.
        desugarClosures(*program);
#else
        throw std::runtime_error("This build was configured without Tree-sitter support. Rebuild with -DWITH_TREE_SITTER=ON.");
#endif

        // Reject escape hatches (`.get()`, raw pointers, casts, ...) before the lifetime analysis, which relies on them being sealed.
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

        // The analyzer runs before the dangling checker, which asks it whether a push_back shares or moves its
        // argument (containerElementIsShared). It runs for --legacy-cpp too.
        SmartPointerAnalyzer analyzer;
        {
            std::vector<std::string> demandLines;
            for (const auto& file : demandsInFiles) {
                std::ifstream in(file);
                for (std::string line; std::getline(in, line);) demandLines.push_back(line);
            }
            analyzer.setDemands(demandLines);
        }
        analyzer.analyzeProgram(*program, source);
        if (!demandsOutFile.empty()) {
            std::ofstream out(demandsOutFile, std::ios::binary);
            for (const auto& line : analyzer.exportDemands(*program)) out << line << "\n";
        }

        // Memory-safety analysis: reject dangling / use-after-move before generating C++.
        DanglingChecker danglingChecker;
        std::vector<DanglingError> danglingErrors = danglingChecker.check(*program, analyzer);
        if (!danglingErrors.empty()) {
            for (const auto& err : danglingErrors) {
                std::cerr << "error[semantic/" << err.code << "]: " << err.message << "\n";
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

        // Advisory Begin/End call-pairing check: it only warns and never changes the exit code or blocks
        // codegen. The pairs come from the project's `.ceeling_pairs` file (see readPairingDeclarations).
        PairingChecker pairingChecker;
        for (const auto& [beginName, endName] :
             readPairingDeclarations(dirnameOf(inputFile) + ".ceeling_pairs")) {
            pairingChecker.registerPair(beginName, endName);
        }
        std::vector<PairingWarning> pairingWarnings = pairingChecker.check(*program);
        for (const auto& warn : pairingWarnings) {
            std::cerr << "warning[style/unpaired-call]: " << warn.message << "\n";
            if (warn.line > 0) {
                std::cerr << "  --> " << inputFile << ":" << warn.line << ":" << warn.column << "\n";
            }
            if (!warn.help.empty()) {
                std::cerr << "  help: " << warn.help << "\n";
            }
        }

        // Generate C++. An output name ending in `.h` requests a declaration-only header (signatures, no
        // bodies, except templates); any other extension gets the full definitions.
        // Include-guard spelling: "CEELING_GEN_" + uppercased basename, non-alphanumerics replaced by '_'.
        auto guardNameFor = [](const std::string& baseName) {
            std::string guardName = baseName;
            for (char& c : guardName) {
                c = (std::isalnum(static_cast<unsigned char>(c)) ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : '_');
            }
            return "CEELING_GEN_" + guardName;
        };

        std::string cppCode;
        bool wantsHeader = outputFile.size() >= 2 &&
            outputFile.compare(outputFile.size() - 2, 2, ".h") == 0;
        if (useLegacyGenerator) {
            CppGenerator generator;
            cppCode = generator.generateCpp(*program, source);
        } else if (wantsHeader) {
            AstEmitter emitter(analyzer);
            size_t slash = outputFile.find_last_of("/\\");
            std::string headerBaseName = (slash == std::string::npos) ? outputFile : outputFile.substr(slash + 1);
            cppCode = emitter.emitHeader(*program, guardNameFor(headerBaseName));
        } else {
            // Per-class declaration guards (AstEmitter::perClassGuardName) make a circular `.clh` include pair
            // safe: whichever copy of a class declaration is reached first wins.
            AstEmitter emitter(analyzer);
            emitter.setBoundsChecks(boundsChecks);
            emitter.setNullChecks(nullChecks);
            emitter.setPoolShared(poolShared);
            if (debugLines) {
                std::string shown = inputFile;                       // the path as given: g++ -g records it, gdb looks for it from the directory g++ ran in
                for (char& c : shown) if (c == '\\') c = '/';
                emitter.setDebugLines(true, shown);
            }
            cppCode = emitter.emit(*program);
        }
        cppCode = rewriteClhIncludes(cppCode);

        if (strictFacts) {
            // --strict: every fact the program relied on must be vouched for: `declared`, `reviewed:<who>:<date>`,
            // or (with =probed) `probed:<date>`. `opaque` is exempt (it only makes checking stricter), and so are
            // built-in facts unless --strict-all.
            std::vector<std::string> unvouched;
            for (const auto* f : cling::builtinFacts().usedFacts()) {
                if (!f->external && !strictIncludesBuiltin) continue;
                if (f->kind == "opaque" || f->source == "declared") continue;
                if (f->review.rfind("reviewed:", 0) == 0 && f->review.size() > 9) continue;
                if (strictAcceptsProbed && f->review.rfind("probed:", 0) == 0 && f->review.size() > 7) continue;
                unvouched.push_back(f->kind + " " + f->subject + "  [" + f->pack + ", " + f->source + (f->review.empty() ? "" : ", review=" + f->review) + "]");
            }
            if (!unvouched.empty()) {
                std::cerr << "Error: --strict: the program relies on " << unvouched.size() << " fact(s) nobody vouches for:" << std::endl;
                for (size_t k = 0; k < unvouched.size() && k < 12; ++k) std::cerr << "  " << unvouched[k] << std::endl;
                if (unvouched.size() > 12) std::cerr << "  ... and " << unvouched.size() - 12 << " more" << std::endl;
                std::cerr << "A scanned fact is a scanner's draft. Either have a person review it (tools/cling-scan/review_pack.py), run the probe against the real library"
                          << " (tools/fact-gauge/probe_pack.py --mark-probed, then --strict=probed), or write the fact by hand as `declared`." << std::endl;
                return 3;
            }
        }
        std::ofstream outFile(outputFile);
        if (!outFile.is_open()) {
            throw std::runtime_error("Could not create output file: " + outputFile);
        }
        outFile << cppCode;
        outFile.close();

        std::cout << "C++ code generated successfully: " << outputFile << std::endl;
        if (reportFacts) {
            // The trusted-boundary report: every fact a check answered "yes" from while compiling this file.
            const cling::FactTable& table = cling::builtinFacts();
            std::vector<const cling::FactTable::Fact*> used = table.usedFacts();
            std::cout << "Facts relied on (" << used.size() << "), packs loaded:";
            for (const auto& p : table.packs()) std::cout << " " << p;
            std::cout << std::endl;
            for (const auto& h : table.headers()) std::cout << "  scanned header  " << h << std::endl;
            for (const auto* f : used) {
                std::cout << "  " << f->kind << " " << f->subject << "  [" << f->pack << ", " << f->source
                          << (f->review.empty() ? "" : ", review=" + f->review) << "]" << std::endl;
            }
        }

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        pauseIfNeeded();
        return 1;
    }

    return 0;
}
