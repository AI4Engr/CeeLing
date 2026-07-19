#include "core/cst_to_ast/ast.h"
#include "core/generator/cpp_generator.h"
#include "core/generator/dangling_checker.h"
#include <iostream>
#include <vector>
#include <fstream>
#include <sstream>
#include <string>

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

void showUsage(const char* programName) {
    std::cout << "Usage: " << programName << " [options] <file.clg>" << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  --cpp <output>  - Compile to C++ and save to <output> file" << std::endl;
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

    // Parse command line arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--cpp") {
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
#else
        throw std::runtime_error("This build was configured without Tree-sitter support. Rebuild with -DWITH_TREE_SITTER=ON.");
#endif

        // Memory-safety analysis: reject dangling / use-after-move before generating C++.
        DanglingChecker danglingChecker;
        std::vector<DanglingError> danglingErrors = danglingChecker.check(*program);
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

        // Generate C++ code
        CppGenerator generator;
        std::string cppCode = generator.generateCpp(*program, source);

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
